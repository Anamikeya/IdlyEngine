#pragma once
#include <engine/core/Result.hpp>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {
// What the console can do: variables to read and set, and actions to run. A variable reads and writes through its
// functions, so it always shows the real value, however that value was last changed. No ImGui here.
class ConsoleRegistry {
public:
    struct Suggestion {
        std::string name, description;
        std::string value; // A variable's current value; empty for an action.
    };
private:
    struct Entry {
        std::string name, description;
        std::function<std::string()> read;                              // Set for a variable.
        std::function<Result<void>(std::string_view)> write;            // Set for a variable.
        std::function<std::string(std::string_view)> run;               // Set for an action. Returns what to print.
        std::string default_value;
    };
    std::vector<Entry> entries_;
    Entry *find(std::string_view name);
public:
    // A name may have spaces, as in "stat fps". Registering a name again replaces it.
    void add_bool(std::string name, std::string description, std::function<bool()> read, std::function<void(bool)> write);
    // `low` equal to `high` means any finite number.
    void add_number(std::string name, std::string description, std::function<double()> read, std::function<void(double)> write, double low = 0, double high = 0);
    void add_action(std::string name, std::string description, std::function<std::string(std::string_view)> run);
    // Runs a line: "name" shows a variable or runs an action, "name value" sets one or passes the rest to the other.
    // The longest registered name that starts the line wins. Returns what to print.
    Result<std::string> execute(std::string_view line);
    // Names matching what has been typed so far, best first.
    std::vector<Suggestion> suggest(std::string_view typed, std::size_t limit = 8) const;
    // Everything, sorted by name.
    std::vector<Suggestion> all() const;
};
// Lines entered so far, for Up and Down. Repeats in a row are kept once.
class ConsoleHistory {
    std::vector<std::string> lines_;
    int cursor_ = -1; // -1: the line being typed, not one from the past.
public:
    static constexpr std::size_t limit = 200;
    void add(std::string line);
    // Older, or newer. The text to show; empty when back at the line being typed.
    std::string previous();
    std::string next();
    void reset_cursor() { cursor_ = -1; }
    std::size_t size() const noexcept { return lines_.size(); }
};
} // namespace engine::editor
