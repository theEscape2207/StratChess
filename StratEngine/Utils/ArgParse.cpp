#include "../StdAfx.h"
#include "ArgParse.h"

#include <charconv>

namespace Engine {

	std::optional<int> parse_int(std::string_view text) noexcept
	{
		if (text.empty()) {
			return std::nullopt;
		}

		// from_chars does not accept a leading '+', but callers passing argv may
		// reasonably write one. Nothing else is skipped: leading whitespace stays a
		// rejection, so " 12" does not silently become 12.
		std::string_view digits = text;
		if (digits.front() == '+') {
			digits.remove_prefix(1);
			if (digits.empty()) {
				return std::nullopt;
			}
		}

		int value = 0;
		const char* const begin = digits.data();
		const char* const end = begin + digits.size();
		const auto [stop, ec] = std::from_chars(begin, end, value);

		// stop != end catches trailing text ("12abc"), which is the case std::stoi
		// accepts silently. ec covers non-numeric input and overflow.
		if (ec != std::errc{} || stop != end) {
			return std::nullopt;
		}
		return value;
	}

} // namespace Engine
