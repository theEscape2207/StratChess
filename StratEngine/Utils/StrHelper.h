#pragma once
#include <string>
#include <algorithm>
#include <ranges>
#include <cctype>

namespace StringHelper
{
    std::string toLower(std::string s) {
        std::ranges::transform(s, s.begin(),
            [](unsigned char c) { return std::tolower(c); });
        return s;
    }

    std::string trim(std::string s) {
        auto view = s
            | std::views::drop_while([](unsigned char c) { return std::isspace(c); })
            | std::views::reverse
            | std::views::drop_while([](unsigned char c) { return std::isspace(c); })
            | std::views::reverse;
        return std::string(view.begin(), view.end());
    }
}
