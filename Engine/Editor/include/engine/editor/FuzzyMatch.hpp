#pragma once
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {
// How well `query` matches `text`: its letters in order, not necessarily together, ignoring case. Empty when it does not
// match at all. Higher is better; letters that run together or start a word count most, so "fs" finds "Focus Selected".
std::optional<int> fuzzy_score(std::string_view query, std::string_view text);
// The indices of the texts that match, best first; ties keep the shorter text, then the original order. `bonus`, when
// given, is added to each text's score, which is how recently used things come first.
std::vector<std::size_t> fuzzy_rank(std::string_view query, std::span<const std::string> texts, std::span<const int> bonus = {});
} // namespace engine::editor
