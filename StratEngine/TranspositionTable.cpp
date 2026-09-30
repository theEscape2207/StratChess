#include "TranspositionTable.h"

#if defined(__linux__)
#	include <cstdlib>
#	include <new>
#	include <sys/mman.h>

void* allocate_table_memory(std::size_t bytes, std::size_t alignment)
{
	constexpr std::size_t huge_page_bytes = std::size_t{2} << 20;
	const bool huge = bytes >= huge_page_bytes;
	const std::size_t block_alignment = huge ? huge_page_bytes : std::max(alignment, sizeof(void*));
	// aligned_alloc requires the size to be a multiple of the alignment.
	const std::size_t size = (bytes + block_alignment - 1) / block_alignment * block_alignment;
	void* memory = std::aligned_alloc(block_alignment, size);
	if (memory == nullptr)
		throw std::bad_alloc();
	if (huge)
		(void)madvise(memory, size, MADV_HUGEPAGE); // Advisory: on failure the memory stays 4 KiB-backed.
	return memory;
}

void free_table_memory(void* memory) noexcept { std::free(memory); }
#endif
