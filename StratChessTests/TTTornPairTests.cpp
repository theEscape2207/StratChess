// TTTornPairTests.cpp — Catch2 unit tests for TranspositionTable: torn pairs and concurrent probes on the lock-free slot.
//
// See Docs/TestDesign.md for the rationale.

#include <catch2/catch_test_macros.hpp>
#include "TTTestFixture.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <latch>
#include <random>
#include <thread>
#include <utility>
#include <vector>

// ── Lock-free slots ───────────────────────────────────────────────────────────
//
// A slot is two atomic words, (key48 | eval) ^ payload * K and payload. Racing stores can leave
// one word from each; these pin what such a slot decodes to.

namespace {
	using SlotEntry = TranspositionTableTestFixture::Entry;

	SlotEntry make_entry(uint64_t key, int16_t value, int16_t depth, Move move, int16_t static_eval = 37)
	{
		SlotEntry entry;
		entry.set_key_eval(key, static_eval);
		entry.value = value;
		entry.depth = depth;
		entry.best_move = move;
		entry.set_bound(BoundType::LOWER);
		entry.set_node_type(NodeType::CUT_NODE);
		return entry;
	}

	SlotEntry entry_a() { return make_entry(KEY_A, 100, 5, Move(e2, e4, MoveFlags::QUIET)); }
	SlotEntry entry_b() { return make_entry(KEY_B, 200, 7, Move(d2, d4, MoveFlags::QUIET)); }

	uint64_t key_of(const SlotEntry& entry) { return entry.key48() << SlotEntry::KEY_SHIFT; }

	// Writes one word of the entry's encoding into the table's first slot.
	void write_word_of(TranspositionTable& tt, size_t word, const SlotEntry& entry)
	{
		TranspositionTableTestFixture::write_word(tt, word, TranspositionTableTestFixture::encode(entry)[word]);
	}

	// A slot holding word 0 of one entry and word 1 of another claims neither key, but the pseudo-key
	// that the two words decode to, carrying the payload of word 1's entry.
	void check_mixed_slot(const TranspositionTable& tt, const SlotEntry& word0_entry, const SlotEntry& word1_entry)
	{
		CHECK_FALSE(tt.probe(key_of(word0_entry), 0).has_value());
		CHECK_FALSE(tt.probe(key_of(word1_entry), 0).has_value());

		const uint64_t pseudo_key =
		    TranspositionTableTestFixture::encode(word0_entry)[0] ^
		    TranspositionTableTestFixture::payload_word(word1_entry) * TranspositionTableTestFixture::slot_mix();
		REQUIRE(pseudo_key >> SlotEntry::KEY_SHIFT != word0_entry.key48());
		REQUIRE(pseudo_key >> SlotEntry::KEY_SHIFT != word1_entry.key48());
		const auto result = tt.probe(pseudo_key, 0);
		REQUIRE(result.has_value());
		CHECK(result->value == word1_entry.value);
		CHECK(result->depth == word1_entry.depth);
		CHECK(result->best_move == word1_entry.best_move);
	}

	// The payload fields as (shift, width) within word 1: value, depth, move, metadata, age.
	constexpr std::array<std::pair<unsigned, unsigned>, 5> PAYLOAD_FIELDS{
	    {{0, 16}, {16, 16}, {32, 16}, {48, 8}, {56, 8}}};

	// The fewest payload fields whose per-field differences d_i, |d_i| < 2^width_i, sum to `delta`
	// modulo 2^64; returns `cap` when it takes at least that many.
	int min_fields_for(uint64_t delta, size_t field, int used, int cap)
	{
		if (used >= cap)
			return cap;
		if (delta == 0)
			return used;
		if (field == PAYLOAD_FIELDS.size())
			return cap;
		const auto [shift, width] = PAYLOAD_FIELDS[field];
		if (delta & ((uint64_t{1} << shift) - 1))
			return cap; // a lower bit no later field can reach
		const unsigned span =
		    (field + 1 == PAYLOAD_FIELDS.size()) ? 64 - shift : PAYLOAD_FIELDS[field + 1].first - shift;
		const int64_t residue = static_cast<int64_t>((delta >> shift) & ((uint64_t{1} << span) - 1));
		int best = cap;
		// This field's difference is the residue, or the residue less one span (a borrow upwards).
		for (const int64_t d : {residue, residue - (int64_t{1} << span)}) {
			if (std::abs(d) >= (int64_t{1} << width))
				continue;
			const uint64_t rest = delta - (static_cast<uint64_t>(d) << shift);
			best = std::min(best, min_fields_for(rest, field + 1, used + (d != 0), best));
		}
		return best;
	}

	constexpr uint64_t inverse_mod_2_64(uint64_t k)
	{
		uint64_t x = k; // correct to 3 bits for odd k; each Newton step doubles that
		for (int i = 0; i < 5; ++i)
			x *= 2 - k * x;
		return x;
	}
} // namespace

TEST_CASE("TT - no torn pair differing in two or fewer payload fields decodes to a stored key", "[tt][tt_torn]")
{
	// A pair (A0, B1) passes the key check only if (A1 - B1) * K lands within 2^16 of zero, so the
	// payload difference must be j * K^-1 for some 0 < |j| < 2^16. j runs over both signs, which is
	// the other mixing direction (B0, A1). None of these may be a change in two or fewer fields.
	constexpr uint64_t k = TranspositionTableTestFixture::slot_mix();
	static_assert(k % 2 == 1);
	constexpr uint64_t k_inverse = inverse_mod_2_64(k);
	static_assert(k * k_inverse == 1);

	int fewest = 3;
	int64_t worst_j = 0;
	for (int64_t j = -(int64_t{1} << 16) + 1; j < (int64_t{1} << 16); ++j) {
		if (j == 0)
			continue;
		const int fields = min_fields_for(static_cast<uint64_t>(j) * k_inverse, 0, 0, fewest);
		if (fields < fewest) {
			fewest = fields;
			worst_j = j;
		}
	}
	INFO("fewest fields " << fewest << " at j = " << worst_j);
	CHECK(fewest >= 3);
}

TEST_CASE("TT - a torn pair whose payloads differ only in value is rejected", "[tt][tt_torn]")
{
	// The hole plain XOR leaves: B's value sits under A's eval bits in word 0. The second pair is a
	// multiplier counterexample: with K = 3 its two payload words, 0x0110000000050064 and ...65,
	// multiply to products differing only in the low two bits.
	const auto check_rejected = [](const SlotEntry& a, const SlotEntry& b) {
		TranspositionTable tt(0);
		write_word_of(tt, 0, a);
		write_word_of(tt, 1, b);
		CHECK_FALSE(tt.probe(key_of(a), 0).has_value());
		write_word_of(tt, 0, b);
		write_word_of(tt, 1, a);
		CHECK_FALSE(tt.probe(key_of(b), 0).has_value());
	};

	check_rejected(make_entry(KEY_A, 100, 5, Move(e2, e4, MoveFlags::QUIET)),
	               make_entry(KEY_A, 900, 5, Move(e2, e4, MoveFlags::QUIET)));

	auto value_100 = make_entry(KEY_A, 100, 5, Move(uint16_t{0}));
	value_100.set_bound(BoundType::EXACT);
	value_100.set_node_type(NodeType::ALL_NODE);
	value_100.age = 1;
	auto value_101 = value_100;
	value_101.value = 101;
	REQUIRE(TranspositionTableTestFixture::payload_word(value_100) == 0x0110'0000'0005'0064);
	REQUIRE(TranspositionTableTestFixture::payload_word(value_101) == 0x0110'0000'0005'0065);
	check_rejected(value_100, value_101);
}

TEST_CASE("TT - a slot holding words from two entries decodes to a pseudo-key", "[tt]")
{
	TranspositionTable tt(0);
	write_word_of(tt, 0, entry_a());
	write_word_of(tt, 1, entry_b());

	check_mixed_slot(tt, entry_a(), entry_b());
}

TEST_CASE("TT - two interleaved stores into one slot leave a mixed pair", "[tt]")
{
	TranspositionTable tt(0);

	SECTION("A's payload, then all of B, then A's key word")
	{
		write_word_of(tt, 1, entry_a());
		write_word_of(tt, 1, entry_b());
		write_word_of(tt, 0, entry_b());
		write_word_of(tt, 0, entry_a());
		check_mixed_slot(tt, entry_a(), entry_b());
	}
	SECTION("A's key word, then all of B, then A's payload")
	{
		write_word_of(tt, 0, entry_a());
		write_word_of(tt, 0, entry_b());
		write_word_of(tt, 1, entry_b());
		write_word_of(tt, 1, entry_a());
		check_mixed_slot(tt, entry_b(), entry_a());
	}
}

TEST_CASE("TT - concurrent probes return only the probed key's payload", "[tt][smp]")
{
	// One bucket, so all four threads contend for four slots and every probe compares key bits 16-63.
	TranspositionTable tt(0);
	tt.newSearch();

	std::mt19937_64 key_rng(747); // NOLINT(bugprone-random-generator-seed): a reproducible key pool
	std::array<uint64_t, 64> keys{};
	for (auto& key : keys) {
		do
			key = key_rng();
		while (key == 0);
	}

	// Every payload field is a function of the key, and the value stays outside the mate range.
	struct Payload {
		int16_t value;
		int16_t depth;
		Move move;
		BoundType bound;
		NodeType node_type;
	};
	const auto payload_of = [](uint64_t key) {
		return Payload{static_cast<int16_t>(static_cast<int>(key % 2000) - 1000), static_cast<int16_t>(key % 20 + 1),
		               Move(static_cast<uint16_t>((key >> 20) & 0x0FFF)), static_cast<BoundType>((key >> 40) % 3),
		               static_cast<NodeType>((key >> 44) % 3)};
	};

	constexpr int THREADS = 4;
	constexpr int ITERATIONS = 100'000;
	struct Tally {
		long probes = 0;
		long hits = 0;
		long wrong = 0;
	};
	std::array<Tally, THREADS> tallies{};
	std::latch start(THREADS);
	{
		std::vector<std::jthread> workers;
		for (int t = 0; t < THREADS; ++t) {
			workers.emplace_back([&, t] {
				std::mt19937 rng(static_cast<unsigned>(t + 1));
				Tally& tally = tallies[static_cast<size_t>(t)];
				start.arrive_and_wait();
				for (int i = 0; i < ITERATIONS; ++i) {
					const uint64_t stored = keys[rng() % keys.size()];
					const Payload p = payload_of(stored);
					tt.store(stored, p.value, p.depth, 0, p.move, p.bound, p.node_type, SearchPhase::MAIN,
					         kNoStaticEval);

					const uint64_t probed = (rng() & 1) ? stored : keys[rng() % keys.size()];
					const auto result = tt.probe(probed, 0);
					++tally.probes;
					if (!result)
						continue;
					++tally.hits;
					const Payload expected = payload_of(probed);
					tally.wrong += result->value != expected.value || result->depth != expected.depth ||
					               result->best_move != expected.move || result->bound != expected.bound ||
					               result->node_type != expected.node_type;
				}
			});
		}
	}

	Tally total;
	for (const Tally& tally : tallies) {
		total.probes += tally.probes;
		total.hits += tally.hits;
		total.wrong += tally.wrong;
	}
	INFO("probes " << total.probes << ", hits " << total.hits << ", wrong payloads " << total.wrong);
	CHECK(total.wrong == 0);
	CHECK(total.hits * 10 >= total.probes);
}
