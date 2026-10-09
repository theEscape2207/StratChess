#pragma once

#include <cstddef>
#include <cstdint>

// Where Board prefetches a child position's transposition-table bucket: the table's first bucket and
// the mask its probe() and store() index with. Board holds the target opaquely and never names the
// table, so two engines in one process each bind their own (#776). The default is a dummy bucket with
// mask 0, so a board no search has bound prefetches the same cached 64 bytes without a branch.
struct PrefetchTarget {
	static constexpr std::size_t BUCKET_BYTES = 64;
	alignas(64) static inline const char dummy_bucket[BUCKET_BYTES] = {};

	const char* base = dummy_bucket;
	std::uint64_t mask = 0;

	const char* bucket_for(std::uint64_t key) const noexcept { return base + (key & mask) * BUCKET_BYTES; }

	bool operator==(const PrefetchTarget&) const = default;
};
