#include <engine/ecs/World.hpp>
#include <stdexcept>
namespace engine::ecs {
World::World() {
    native_.set_threads(1);
}
flecs::world &World::native() {
    if (!on_owner_thread())
        throw std::logic_error("Flecs world accessed from a non-owner thread");
    return native_;
}
bool World::progress(double seconds) {
    return native().progress(static_cast<ecs_ftime_t>(seconds));
}
} // namespace engine::ecs

namespace engine::ecs {
const flecs::world &World::native() const {
    if (!on_owner_thread())
        throw std::logic_error("Flecs world accessed from a non-owner thread");
    return native_;
}
} // namespace engine::ecs
