#pragma once
#include <optional>
#include <string_view>

namespace Engine {

	/// Parse a whole decimal integer from external input.
	///
	/// Returns nullopt on anything that is not exactly an integer: empty or blank
	/// text, trailing characters, or a value outside int. Never throws — every
	/// caller is argv or a JSON key, where a thrown exception is precisely the
	/// failure this replaces.
	///
	/// Stricter than std::stoi, which accepts trailing garbage ("12abc" parses as
	/// 12) and reports the rest of its failures by throwing.
	[[nodiscard]] std::optional<int> parse_int(std::string_view text) noexcept;

} // namespace Engine
