// TTAllocationTests.cpp — Catch2 unit tests for TranspositionTable: allocation, geometry, huge-page alignment and the
// prefetch target.
//
// See Docs/TestDesign.md for the rationale.

#include <catch2/catch_test_macros.hpp>
#include "TTTestFixture.h"
#include "AIPerplex.h" // DEFAULT_HASH_MB, for the equal-capacity invariant only

#include <cstdint>
#include <vector>

#if defined(__linux__)
#	include <malloc.h>
#endif

// ── Allocation reporting ──────────────────────────────────────────────────────
//
// The bucket count is rounded down to a power of two, so the table is generally
// smaller than the megabytes requested. These pin the two properties that make
// that acceptable: the diagnostic must describe the allocation rather than the
// request, and the allocation must never exceed the request.

TEST_CASE("TT - memory_mb reports the non-exact allocation, not the request", "[tt]")
{
	// 192 MiB / 64-byte buckets = 3145728, floored to 2^21 = 2097152 buckets,
	// which is 128 MiB of entries. Reporting 192 would hide that shortfall.
	TranspositionTable tt(192);

	REQUIRE(tt.requested_memory_mb() == 192);
	REQUIRE(tt.bucket_count() == 2097152u);
	REQUIRE(tt.memory_mb() == 128);
}

TEST_CASE("TT - a 256 MiB request uses the packed table capacity", "[tt]")
{
	TranspositionTable tt(256);

	REQUIRE(tt.bucket_count() == 4194304u);
	REQUIRE(tt.memory_mb() == 256);
}

TEST_CASE("TT - bucket_count_for matches the packed geometry across the UCI range", "[tt]")
{
	constexpr size_t MIB = size_t{1024} * 1024;
	auto floor_pow2 = [](size_t value) constexpr {
		size_t power = 1;
		while ((power << 1) <= value)
			power <<= 1;
		return power;
	};

	size_t same_capacity = 0;
	size_t doubled_capacity = 0;
	for (size_t requested = 0; requested <= 1536; ++requested) {
		const size_t packed = TranspositionTable::bucket_count_for(requested);
		const size_t old = floor_pow2((requested * MIB) / 96);
		const bool same = packed == old;
		const bool doubled = packed == old * 2;

		INFO("requested " << requested << " MiB");
		CHECK((same || doubled));
		if (requested != 0)
			CHECK(packed * size_t{64} <= requested * MIB);
		same_capacity += same;
		doubled_capacity += doubled;
	}

	REQUIRE(same_capacity == 513);
	REQUIRE(doubled_capacity == 1024);

	// At the shipped default the packed layout keeps the 96-byte layout's bucket count, so the default
	// search has the capacity it was tuned with.
	CHECK(TranspositionTable::bucket_count_for(AIPerplex::DEFAULT_HASH_MB) == 2097152u);
	CHECK(TranspositionTable::bucket_count_for(AIPerplex::DEFAULT_HASH_MB) ==
	      floor_pow2((size_t{AIPerplex::DEFAULT_HASH_MB} * MIB) / 96));
}

TEST_CASE("TT - allocation never exceeds the requested budget", "[tt]")
{
	// The argument is a memory cap, so rounding must not overshoot it. Sizes
	// either side of a power-of-two boundary, where round-to-nearest would.
	for (const size_t requested :
	     {size_t{1}, size_t{7}, size_t{64}, size_t{100}, size_t{256}, size_t{300}, size_t{511}}) {
		TranspositionTable tt(requested);

		INFO("requested " << requested << " MiB");
		REQUIRE(tt.requested_memory_mb() == requested);
		REQUIRE(tt.memory_mb() <= requested);
	}
}

TEST_CASE("TT - byte accounting is self-consistent", "[tt]")
{
	TranspositionTable tt(16);

	REQUIRE(tt.bucket_count() > 0);
	REQUIRE(tt.entry_bytes() == tt.bucket_count() * 64); // 4 x 16-byte packed entries
	REQUIRE(tt.memory_mb() == tt.entry_bytes() / (size_t{1024} * 1024));
}

TEST_CASE("TT - bucket count is a power of two", "[tt]")
{
	// probe/store index with `key & index_mask`, which is only equivalent to a
	// modulo when the bucket count is a power of two.
	for (const size_t requested : {size_t{1}, size_t{5}, size_t{16}, size_t{100}, size_t{256}}) {
		TranspositionTable tt(requested);
		const size_t n = tt.bucket_count();

		INFO("requested " << requested << " MiB, got " << n << " buckets");
		REQUIRE(n > 0);
		REQUIRE((n & (n - 1)) == 0);
	}
}

TEST_CASE("TT - the prefetch target names the bucket store() writes", "[tt][tt_prefetch]")
{
	TranspositionTable tt(1);
	const std::uint64_t last = tt.bucket_count() - 1;
	// High bits give the small keys distinct, nonzero key48s without moving their buckets.
	constexpr std::uint64_t high = std::uint64_t{1} << 40;
	const std::vector<std::uint64_t> keys{1 | high,
	                                      last | (2 * high),
	                                      (last + 1) | (3 * high),
	                                      ~std::uint64_t{0},
	                                      std::uint64_t{1} << 63,
	                                      0x9E37'79B9'7F4A'7C15,
	                                      0xBF58'476D'1CE4'E5B9,
	                                      0x94D0'49BB'1331'11EB,
	                                      0x2545'F491'4F6C'DD1D,
	                                      0xD6E8'FEB8'6659'FD93,
	                                      0xA076'1D64'78BD'642F,
	                                      0xE703'7ED1'A0B4'28DB,
	                                      0x8EBC'6AF0'9C88'C6E3};

	const PrefetchTarget target = tt.prefetch_target();
	for (const std::uint64_t key : keys) {
		INFO("key " << key);
		do_store(tt, key, 1);
		CHECK(target.bucket_for(key) == TranspositionTableTestFixture::bucket_holding(tt, key));
	}
}

TEST_CASE("TT - a request too small for one bucket still allocates one", "[tt]")
{
	// 0 MiB would compute zero buckets; the table must stay usable rather than
	// producing an index_mask of SIZE_MAX.
	TranspositionTable tt(0);

	REQUIRE(tt.bucket_count() == 1);
	REQUIRE(tt.memory_mb() == 0);

	do_store(tt, KEY_A, 123);
	auto result = tt.probe(KEY_A, 0);
	REQUIRE(result.has_value());
	REQUIRE(result->value == 123);
}

// ── Huge-page storage (Linux) ─────────────────────────────────────────────────

#if defined(__linux__)
TEST_CASE("TT - table memory is huge-page aligned from 2 MiB and not rounded up below it", "[tt]")
{
	constexpr size_t huge_page = size_t{2} << 20;
	for (const size_t bytes : {size_t{64}, huge_page / 2, huge_page, 3 * huge_page}) {
		void* memory = allocate_table_memory(bytes, 64);
		const auto address = reinterpret_cast<uintptr_t>(memory);
		CHECK(address % 64 == 0);
		if (bytes >= huge_page)
			CHECK(address % huge_page == 0);
		else
			CHECK(malloc_usable_size(memory) < huge_page);
		free_table_memory(memory);
	}
}
#endif
