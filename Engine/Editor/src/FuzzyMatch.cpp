#include <engine/editor/FuzzyMatch.hpp>
#include <algorithm>
#include <cctype>
#include <numeric>

namespace engine::editor {
namespace {
constexpr int match_score = 1, run_bonus = 8, word_bonus = 10, first_letter_bonus = 6, skip_penalty = 1;
char lower(char c) { return char(std::tolower(static_cast<unsigned char>(c))); }
bool starts_word(std::string_view text, std::size_t at) {
    if (at == 0) return true;
    const auto before = static_cast<unsigned char>(text[at - 1]), here = static_cast<unsigned char>(text[at]);
    return !std::isalnum(before) || (std::islower(before) && std::isupper(here));
}
}
std::optional<int> fuzzy_score(std::string_view query, std::string_view text) {
    if (query.empty()) return 0;
    if (query.size() > text.size()) return std::nullopt;
    constexpr int none = -1000000;
    // ended[j]: the best score of the query so far with its last letter at text[j].
    std::vector<int> ended(text.size(), none), next(text.size(), none);
    for (std::size_t i = 0; i < query.size(); ++i) {
        std::ranges::fill(next, none);
        int best_before = none; // The best of ended[0..j-2], penalized for the letters skipped since.
        for (std::size_t j = i; j < text.size(); ++j) {
            if (j >= 2 && ended[j - 2] != none) best_before = std::max(best_before, ended[j - 2]);
            if (best_before != none) best_before -= skip_penalty;
            if (lower(query[i]) != lower(text[j])) continue;
            int score = match_score + (starts_word(text, j) ? word_bonus : 0);
            if (i == 0) score += j == 0 ? first_letter_bonus : -int(std::min<std::size_t>(j, 3));
            else {
                const int after_gap = best_before;
                const int in_a_row = j >= 1 && ended[j - 1] != none ? ended[j - 1] + run_bonus : none;
                const int previous = std::max(after_gap, in_a_row);
                if (previous == none) continue;
                score += previous;
            }
            next[j] = score;
        }
        ended.swap(next);
    }
    const int best = *std::ranges::max_element(ended);
    if (best == none) return std::nullopt;
    return best;
}
std::vector<std::size_t> fuzzy_rank(std::string_view query, std::span<const std::string> texts, std::span<const int> bonus) {
    struct Candidate { std::size_t index; int score; };
    std::vector<Candidate> found;
    for (std::size_t i = 0; i < texts.size(); ++i)
        if (const auto score = fuzzy_score(query, texts[i])) found.push_back({i, *score + (i < bonus.size() ? bonus[i] : 0)});
    std::ranges::stable_sort(found, [&](const Candidate &a, const Candidate &b) {
        if (a.score != b.score) return a.score > b.score;
        // With nothing typed, the given order is the one that was asked for.
        return !query.empty() && texts[a.index].size() < texts[b.index].size();
    });
    std::vector<std::size_t> indices;
    indices.reserve(found.size());
    for (const auto &candidate : found) indices.push_back(candidate.index);
    return indices;
}
} // namespace engine::editor
