#include <engine/editor/ConsoleRegistry.hpp>
#include <engine/editor/FuzzyMatch.hpp>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace engine::editor {
namespace {
std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}
std::string number_text(double value) {
    char text[32];
    std::snprintf(text, sizeof text, "%g", value);
    return text;
}
bool same_word(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
}
}
ConsoleRegistry::Entry *ConsoleRegistry::find(std::string_view name) {
    const auto at = std::ranges::find_if(entries_, [&](const Entry &entry) { return same_word(entry.name, name); });
    return at == entries_.end() ? nullptr : &*at;
}
void ConsoleRegistry::add_bool(std::string name, std::string description, std::function<bool()> read, std::function<void(bool)> write) {
    Entry entry;
    entry.name = std::move(name); entry.description = std::move(description);
    entry.default_value = read() ? "1" : "0";
    entry.read = [read] { return std::string(read() ? "1" : "0"); };
    entry.write = [write = std::move(write)](std::string_view text) -> Result<void> {
        for (const auto *word : {"1", "on", "true"}) if (same_word(text, word)) { write(true); return {}; }
        for (const auto *word : {"0", "off", "false"}) if (same_word(text, word)) { write(false); return {}; }
        return fail(ErrorCode::InvalidArgument, "Expected 1 or 0, on or off");
    };
    if (auto *existing = find(entry.name)) *existing = std::move(entry); else entries_.push_back(std::move(entry));
}
void ConsoleRegistry::add_number(std::string name, std::string description, std::function<double()> read, std::function<void(double)> write, double low, double high) {
    Entry entry;
    entry.name = std::move(name); entry.description = std::move(description);
    entry.default_value = number_text(read());
    entry.read = [read] { return number_text(read()); };
    entry.write = [write = std::move(write), low, high](std::string_view text) -> Result<void> {
        double value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) return fail(ErrorCode::InvalidArgument, "Expected a number");
        if (low < high && (value < low || value > high)) return fail(ErrorCode::InvalidArgument, "Expected a number from " + number_text(low) + " to " + number_text(high));
        write(value);
        return {};
    };
    if (auto *existing = find(entry.name)) *existing = std::move(entry); else entries_.push_back(std::move(entry));
}
void ConsoleRegistry::add_action(std::string name, std::string description, std::function<std::string(std::string_view)> run) {
    Entry entry;
    entry.name = std::move(name); entry.description = std::move(description);
    entry.run = std::move(run);
    if (auto *existing = find(entry.name)) *existing = std::move(entry); else entries_.push_back(std::move(entry));
}
Result<std::string> ConsoleRegistry::execute(std::string_view line) {
    line = trim(line);
    if (line.empty()) return std::string{};
    // The longest name that is the whole line or is followed by a space.
    Entry *chosen = nullptr;
    for (auto &entry : entries_) {
        const std::string_view name(entry.name);
        if (line.size() < name.size() || !same_word(line.substr(0, name.size()), name)) continue;
        if (line.size() > name.size() && line[name.size()] != ' ') continue;
        if (!chosen || name.size() > chosen->name.size()) chosen = &entry;
    }
    if (!chosen) return fail(ErrorCode::NotFound, "No such command or variable: " + std::string(line));
    const auto rest = trim(line.substr(chosen->name.size()));
    if (chosen->run) return chosen->run(rest);
    if (rest.empty()) return chosen->name + " = " + chosen->read() + "  (default " + chosen->default_value + "). " + chosen->description;
    if (auto written = chosen->write(rest); !written) return written.error();
    return chosen->name + " = " + chosen->read();
}
std::vector<ConsoleRegistry::Suggestion> ConsoleRegistry::suggest(std::string_view typed, std::size_t limit) const {
    // Once a value is being typed after a whole name, that name is still what is being talked about.
    typed = trim(typed);
    std::vector<std::string> names;
    names.reserve(entries_.size());
    for (const auto &entry : entries_) names.push_back(entry.name);
    std::vector<Suggestion> result;
    const auto describe = [&](std::size_t i) {
        const auto &entry = entries_[i];
        result.push_back({entry.name, entry.description, entry.read ? entry.read() : std::string{}});
    };
    for (std::size_t i = 0; i < entries_.size() && !typed.empty(); ++i)
        if (typed.size() > names[i].size() && same_word(typed.substr(0, names[i].size()), names[i]) && typed[names[i].size()] == ' ') { describe(i); return result; }
    for (const auto index : fuzzy_rank(typed, names)) {
        if (result.size() >= limit) break;
        describe(index);
    }
    return result;
}
std::vector<ConsoleRegistry::Suggestion> ConsoleRegistry::all() const {
    std::vector<Suggestion> result;
    result.reserve(entries_.size());
    for (const auto &entry : entries_) result.push_back({entry.name, entry.description, entry.read ? entry.read() : std::string{}});
    std::ranges::sort(result, {}, &Suggestion::name);
    return result;
}
void ConsoleHistory::add(std::string line) {
    cursor_ = -1;
    if (line.empty() || (!lines_.empty() && lines_.back() == line)) return;
    lines_.push_back(std::move(line));
    if (lines_.size() > limit) lines_.erase(lines_.begin());
}
std::string ConsoleHistory::previous() {
    if (lines_.empty()) return {};
    cursor_ = cursor_ < 0 ? int(lines_.size()) - 1 : std::max(cursor_ - 1, 0);
    return lines_[std::size_t(cursor_)];
}
std::string ConsoleHistory::next() {
    if (cursor_ < 0) return {};
    if (++cursor_ >= int(lines_.size())) { cursor_ = -1; return {}; }
    return lines_[std::size_t(cursor_)];
}
} // namespace engine::editor
