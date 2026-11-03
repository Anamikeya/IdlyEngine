#include <engine/editor/Feedback.hpp>
#include <algorithm>
#include <utility>

namespace engine::editor {
const char *to_string(Severity severity) noexcept {
    switch (severity) {
    case Severity::Info: return "Info";
    case Severity::Success: return "Success";
    case Severity::Warning: return "Warning";
    case Severity::Error: return "Error";
    }
    return "Info";
}
bool LogFilter::allows(Severity severity) const noexcept {
    switch (severity) {
    case Severity::Info: return info;
    case Severity::Success: return success;
    case Severity::Warning: return warning;
    case Severity::Error: return error;
    }
    return true;
}
void MessageLog::add(LogEntry entry) {
    if (entries_.size() == limit) entries_.erase(entries_.begin());
    entries_.push_back(std::move(entry));
}
std::size_t MessageLog::count(Severity severity) const noexcept {
    return std::size_t(std::ranges::count_if(entries_, [&](const LogEntry &entry) { return !entry.from_console && entry.severity == severity; }));
}
std::size_t MessageLog::shown(const LogFilter &filter) const noexcept {
    return std::size_t(std::ranges::count_if(entries_, [&](const LogEntry &entry) { return filter.allows(entry); }));
}

double ToastQueue::duration(Severity severity) noexcept {
    switch (severity) {
    case Severity::Info: return 4;
    case Severity::Success: return 3;
    case Severity::Warning: return 6;
    case Severity::Error: return 10;
    }
    return 4;
}
std::uint64_t ToastQueue::show(Severity severity, std::string text, LogLink link) {
    const auto same = std::ranges::find_if(toasts_, [&](const Toast &toast) { return toast.severity == severity && toast.text == text; });
    if (same != toasts_.end()) {
        ++same->count;
        same->remaining = duration(severity);
        return same->id;
    }
    toasts_.push_back({next_id_++, severity, std::move(text), std::move(link), duration(severity)});
    if (toasts_.size() > visible_limit) {
        // An error is the last to make way. Everything also stays in the log.
        const auto gone = std::ranges::find_if(toasts_, [](const Toast &toast) { return toast.severity != Severity::Error; });
        toasts_.erase(gone == toasts_.end() ? toasts_.begin() : gone);
    }
    return toasts_.back().id;
}
void ToastQueue::update(double seconds, std::uint64_t hovered) {
    for (auto &toast : toasts_) if (toast.id != hovered) toast.remaining -= seconds;
    std::erase_if(toasts_, [](const Toast &toast) { return toast.remaining <= 0; });
}
void ToastQueue::dismiss(std::uint64_t id) {
    std::erase_if(toasts_, [&](const Toast &toast) { return toast.id == id; });
}
std::size_t ToastQueue::count(Severity severity) const noexcept {
    return std::size_t(std::ranges::count_if(toasts_, [&](const Toast &toast) { return toast.severity == severity; }));
}

std::uint64_t TaskList::begin(std::string name, bool can_cancel) {
    tasks_.push_back({next_id_, std::move(name), -1, can_cancel, 0});
    return next_id_++;
}
void TaskList::set_name(std::uint64_t id, std::string name) {
    for (auto &task : tasks_) if (task.id == id) task.name = std::move(name);
}
void TaskList::set_progress(std::uint64_t id, double fraction) {
    for (auto &task : tasks_) if (task.id == id) task.progress = std::clamp(fraction, 0.0, 1.0);
}
void TaskList::finish(std::uint64_t id) {
    std::erase_if(tasks_, [&](const BackgroundTask &task) { return task.id == id; });
}
void TaskList::update(double seconds) {
    for (auto &task : tasks_) task.age += seconds;
}
std::size_t TaskList::visible() const noexcept {
    return std::size_t(std::ranges::count_if(tasks_, [](const BackgroundTask &task) { return task.age >= show_after; }));
}
} // namespace engine::editor
