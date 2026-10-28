#pragma once
#include <flecs.h>
#include <thread>
namespace engine::ecs {
class World final {
    std::thread::id owner_ = std::this_thread::get_id();
    flecs::world native_;

  public:
    World();
    World(const World &) = delete;
    World &operator=(const World &) = delete;
    bool on_owner_thread() const noexcept {
        return owner_ == std::this_thread::get_id();
    }
    flecs::world &native();
    const flecs::world &native() const;
    bool progress(double fixed_seconds);
};
} // namespace engine::ecs
