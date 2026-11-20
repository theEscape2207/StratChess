#pragma once
#include "Move.h"
#include "defines.h"
#include <algorithm>
#include <array>
#include <span>

// Triangular PV Table for principal variation caching
class PVTable {
  public:
	static constexpr int MAX_PV_LENGTH = 128;

	void clear_ply(int ply) noexcept
	{
		if (ply < MAX_PLY) {
			pv_lengths_[ply] = 0;
		}
	}

	void update(int ply, Move move) noexcept
	{
		if (ply >= MAX_PLY)
			return;

		pv_lines_[ply][0] = move;

		// Copy child PV
		const int child_len = (ply + 1 < MAX_PLY) ? pv_lengths_[ply + 1] : 0;
		for (int i = 0; i < child_len && i + 1 < MAX_PV_LENGTH; ++i) {
			pv_lines_[ply][i + 1] = pv_lines_[ply + 1][i];
		}
		pv_lengths_[ply] = std::min(child_len + 1, MAX_PV_LENGTH);
	}

	// Replaces row ply with line, cut to MAX_PV_LENGTH. Multi-PV writes its sorted rank 1 back into row 0,
	// so every reader of the root row sees the set's best line.
	void set_line(int ply, std::span<const Move> line) noexcept
	{
		if (ply >= MAX_PLY)
			return;
		const int length = static_cast<int>(std::min(line.size(), static_cast<size_t>(MAX_PV_LENGTH)));
		std::copy_n(line.begin(), length, pv_lines_[ply].begin());
		pv_lengths_[ply] = length;
	}

	[[nodiscard]] Move get_pv_move(int ply) const noexcept
	{
		return (ply < MAX_PLY && pv_lengths_[ply] > 0) ? pv_lines_[ply][0] : Move::EmptyMove();
	}

	[[nodiscard]] const std::array<Move, MAX_PV_LENGTH>& get_line(int ply) const noexcept { return pv_lines_[ply]; }

	[[nodiscard]] int get_length(int ply) const noexcept { return (ply < MAX_PLY) ? pv_lengths_[ply] : 0; }

  private:
	std::array<std::array<Move, MAX_PV_LENGTH>, MAX_PLY> pv_lines_{};
	std::array<int, MAX_PLY> pv_lengths_{};
};
