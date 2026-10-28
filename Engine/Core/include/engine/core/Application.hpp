#pragma once
#include <chrono>
#include <engine/commands/CommandBus.hpp>
#include <memory>
#include <thread>
namespace engine {
using Duration = std::chrono::nanoseconds;
struct LoopConfig {
    Duration fixed_step = std::chrono::milliseconds(10);
    Duration max_frame = std::chrono::milliseconds(250);
    std::uint32_t max_catch_up = 8;
};
struct FixedUpdate {
    std::uint64_t tick;
    Duration delta;
};
struct FrameUpdate {
    Duration delta;
    double interpolation;
    std::uint32_t fixed_steps;
    Duration dropped;
};
class Application;
class ApplicationClient {
  public:
    virtual ~ApplicationClient() = default;
    virtual Result<void> on_start(Application &) = 0;
    virtual void on_fixed_update(Application &, const FixedUpdate &) {}
    virtual void on_update(Application &, const FrameUpdate &) {}
    virtual void on_stop(Application &) noexcept {}
};
enum class ApplicationState { Created, Running, Stopped };
class Application final {
    LoopConfig config_;
    ApplicationClient &client_;
    std::thread::id owner_ = std::this_thread::get_id();
    std::unique_ptr<Scene> scene_;
    std::unique_ptr<commands::CommandBus> commands_;
    ApplicationState state_ = ApplicationState::Created;
    Duration accumulator_{0};
    std::uint64_t tick_ = 0;
    bool callback_started_ = false, advancing_ = false, stop_requested_ = false;
    bool owner() const noexcept {
        return owner_ == std::this_thread::get_id();
    }

  public:
    explicit Application(ApplicationClient &, LoopConfig = {});
    ~Application();
    Application(const Application &) = delete;
    Application &operator=(const Application &) = delete;
    Result<void> start();
    Result<FrameUpdate> advance(Duration);
    void request_stop() noexcept {
        stop_requested_ = true;
    } // Owner thread only.
    Result<void> shutdown();
    ApplicationState state() const noexcept {
        return state_;
    }
    std::uint64_t tick() const noexcept {
        return tick_;
    }
    Scene *scene() noexcept {
        return scene_.get();
    }
    commands::CommandBus *commands() noexcept {
        return commands_.get();
    }
};
class SteadyClock {
    std::chrono::steady_clock::time_point previous_ = std::chrono::steady_clock::now();

  public:
    Duration sample() {
        auto now = std::chrono::steady_clock::now();
        auto delta = now - previous_;
        previous_ = now;
        return std::chrono::duration_cast<Duration>(delta);
    }
};
void log_info(std::string_view message) noexcept;
void log_error(std::string_view message) noexcept;
} // namespace engine
