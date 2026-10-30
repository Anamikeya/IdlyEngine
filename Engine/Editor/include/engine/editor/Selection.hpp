#pragma once
#include <engine/scene/Scene.hpp>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <vector>

namespace engine::editor {
// The selected entities. Panels read and write this and never talk to each other, and listeners hear of every real
// change. Selecting is not an edit: it joins no undo step and does not make the scene dirty.
class SelectionSet {
    std::vector<EntityId> items_; // In the order they were selected.
    EntityId primary_ = 0;        // The last one selected; 0 when empty. Gizmos and the Inspector follow it.
    std::map<std::uint64_t, std::function<void()>> listeners_;
    std::uint64_t next_listener_ = 1;
    std::uint64_t changes_ = 0;
    void changed() {
        ++changes_;
        // A copy, so a listener may unsubscribe while being told.
        const auto listeners = listeners_;
        for (const auto &[token, listener] : listeners) listener();
    }
public:
    EntityId primary() const noexcept { return primary_; }
    const std::vector<EntityId> &items() const noexcept { return items_; }
    std::size_t size() const noexcept { return items_.size(); }
    bool empty() const noexcept { return items_.empty(); }
    bool contains(EntityId id) const noexcept { return std::ranges::find(items_, id) != items_.end(); }
    // Counts real changes, for whoever polls instead of listening.
    std::uint64_t changes() const noexcept { return changes_; }
    // Replaces the selection with one entity; zero clears it.
    void set(EntityId id) {
        if (id) set(std::span<const EntityId>(&id, 1)); else clear();
    }
    // Replaces the selection. The last entity becomes the primary one; zeros and repeats are dropped.
    void set(std::span<const EntityId> ids) {
        std::vector<EntityId> unique;
        for (const auto id : ids) if (id && std::ranges::find(unique, id) == unique.end()) unique.push_back(id);
        const EntityId primary = unique.empty() ? 0 : unique.back();
        if (unique == items_ && primary == primary_) return;
        items_ = std::move(unique); primary_ = primary;
        changed();
    }
    void clear() { set(std::span<const EntityId>{}); }
    // Adds it as the new primary one, or takes it away if it was selected: a Ctrl+click.
    void toggle(EntityId id) {
        if (!id) return;
        if (contains(id)) { remove(id); return; }
        items_.push_back(id); primary_ = id;
        changed();
    }
    void remove(EntityId id) {
        const auto at = std::ranges::find(items_, id);
        if (at == items_.end()) return;
        items_.erase(at);
        if (primary_ == id) primary_ = items_.empty() ? 0 : items_.back();
        changed();
    }
    // Drops what `keep` refuses, such as entities that no longer exist.
    template<class Keep> void retain(Keep keep) {
        std::vector<EntityId> kept;
        for (const auto id : items_) if (keep(id)) kept.push_back(id);
        if (kept.size() == items_.size()) return;
        items_ = std::move(kept);
        if (std::ranges::find(items_, primary_) == items_.end()) primary_ = items_.empty() ? 0 : items_.back();
        changed();
    }
    // Calls `listener` after each change. Returns the token for unsubscribe().
    std::uint64_t subscribe(std::function<void()> listener) {
        listeners_.emplace(next_listener_, std::move(listener));
        return next_listener_++;
    }
    void unsubscribe(std::uint64_t token) { listeners_.erase(token); }
};
} // namespace engine::editor
