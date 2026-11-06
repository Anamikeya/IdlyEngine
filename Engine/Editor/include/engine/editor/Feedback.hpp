#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// What the editor tells the user: the message log, toasts, and tasks still running. None of it needs a window, so
// time is passed in and it can be tested alone.
namespace engine::editor {
enum class Severity : std::uint8_t { Info, Success, Warning, Error };
const char *to_string(Severity severity) noexcept;

// Something a message is about, which a click can take the user to.
struct LogLink {
    // Script: a line of a script asset, opened in the Script Editor (Milestone 6).
    enum class Kind : std::uint8_t { None, Entity, Asset, Script };
    Kind kind = Kind::None;
    std::uint64_t id = 0;
    std::string label;
    int line = 0; // Of a script, 1-based; 0 for none.
    explicit operator bool() const noexcept { return kind != Kind::None; }
    static LogLink entity(std::uint64_t id, std::string label) { return {Kind::Entity, id, std::move(label)}; }
    static LogLink asset(std::uint64_t id, std::string label) { return {Kind::Asset, id, std::move(label)}; }
    static LogLink script(std::uint64_t id, std::string label, int line) { return {Kind::Script, id, std::move(label), line}; }
};
struct LogEntry {
    Severity severity = Severity::Info;
    std::string text;
    LogLink link;
    bool from_console = false; // What was typed in the Console and what it said. The Message Log leaves these out.
};
struct LogFilter {
    bool info = true, success = true, warning = true, error = true;
    bool allows(Severity severity) const noexcept;
    bool allows(const LogEntry &entry) const noexcept { return !entry.from_console && allows(entry.severity); }
};
class MessageLog {
    std::vector<LogEntry> entries_;
public:
    static constexpr std::size_t limit = 512; // The oldest go first.
    void add(LogEntry entry);
    void clear() noexcept { entries_.clear(); }
    const std::vector<LogEntry> &entries() const noexcept { return entries_; }
    std::size_t count(Severity severity) const noexcept; // Not counting the Console's own lines.
    std::size_t shown(const LogFilter &filter) const noexcept;
};

struct Toast {
    std::uint64_t id = 0;
    Severity severity = Severity::Info;
    std::string text;
    LogLink link;
    double remaining = 0; // Seconds.
    int count = 1;        // The same message again adds to this rather than stacking up.
};
class ToastQueue {
    std::vector<Toast> toasts_;
    std::uint64_t next_id_ = 1;
public:
    static constexpr std::size_t visible_limit = 5;
    static double duration(Severity severity) noexcept;
    std::uint64_t show(Severity severity, std::string text, LogLink link = {});
    // `hovered`: this one waits while the mouse is on it, so it can be read and clicked.
    void update(double seconds, std::uint64_t hovered = 0);
    void dismiss(std::uint64_t id);
    const std::vector<Toast> &toasts() const noexcept { return toasts_; }
    std::size_t count(Severity severity) const noexcept;
};

struct BackgroundTask {
    std::uint64_t id = 0;
    std::string name;
    double progress = -1; // From 0 to 1. Negative: how far along is not known.
    bool can_cancel = false;
    double age = 0; // Seconds.
};
class TaskList {
    std::vector<BackgroundTask> tasks_;
    std::uint64_t next_id_ = 1;
public:
    static constexpr double show_after = 0.3; // Seconds. What finishes sooner never needs a toast.
    std::uint64_t begin(std::string name, bool can_cancel = false);
    void set_name(std::uint64_t id, std::string name);
    void set_progress(std::uint64_t id, double fraction);
    void finish(std::uint64_t id);
    void update(double seconds);
    const std::vector<BackgroundTask> &tasks() const noexcept { return tasks_; }
    std::size_t visible() const noexcept;
};
} // namespace engine::editor
