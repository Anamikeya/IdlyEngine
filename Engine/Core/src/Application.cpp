#include <algorithm>
#include <engine/core/Application.hpp>
#include <exception>
#include <limits>
#include <stdexcept>
namespace engine {
Application::Application(ApplicationClient &c, LoopConfig config) : config_(config), client_(c) {}
Application::~Application() {
    if (owner()) {
        auto ignored = shutdown();
        (void)ignored;
    } else
        std::terminate();
}
Result<void> Application::start() {
    if (!owner())
        return fail(ErrorCode::WrongThread, "Application start requires owner thread");
    if (state_ != ApplicationState::Created)
        return fail(ErrorCode::InvalidState, "Application can only be started once");
    if (config_.fixed_step.count() <= 0 || config_.max_frame < config_.fixed_step ||
        config_.max_catch_up == 0 || config_.max_catch_up > 1024 ||
        config_.max_frame.count() > std::numeric_limits<Duration::rep>::max() / 2)
        return fail(ErrorCode::InvalidArgument, "Invalid fixed-step configuration");
    try {
        scene_ = std::make_unique<Scene>();
        commands_ = std::make_unique<commands::CommandBus>(*scene_);
        state_ = ApplicationState::Running;
        callback_started_ = true;
        advancing_ = true;
        auto initialized = client_.on_start(*this);
        advancing_ = false;
        if (!initialized) {
            auto error = initialized.error();
            (void)shutdown();
            return error;
        }
        if (stop_requested_) {
            (void)shutdown();
            return {};
        }
        log_info("Application started");
        return {};
    } catch (const std::exception &e) {
        advancing_ = false;
        (void)shutdown();
        return fail(ErrorCode::Internal,
                    std::string("Application initialization failed: ") + e.what());
    } catch (...) {
        advancing_ = false;
        (void)shutdown();
        return fail(ErrorCode::Internal, "Application initialization threw an unknown exception");
    }
}
Result<FrameUpdate> Application::advance(Duration elapsed) {
    if (!owner())
        return fail(ErrorCode::WrongThread, "Application advance requires owner thread");
    if (state_ != ApplicationState::Running || advancing_)
        return fail(ErrorCode::InvalidState, "Application is not running or update is reentrant");
    if (elapsed.count() < 0 || elapsed.count() > std::numeric_limits<Duration::rep>::max() / 2)
        return fail(ErrorCode::InvalidArgument,
                    "Frame duration is negative or outside the supported range");
    if (stop_requested_) {
        (void)shutdown();
        return fail(ErrorCode::InvalidState, "Application stop was requested");
    }
    advancing_ = true;
    FrameUpdate frame{std::min(elapsed, config_.max_frame), 0, 0,
                      elapsed - std::min(elapsed, config_.max_frame)};
    accumulator_ += frame.delta;
    try {
        while (accumulator_ >= config_.fixed_step && frame.fixed_steps < config_.max_catch_up &&
               !stop_requested_) {
            if (tick_ == std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("Tick counter exhausted");
            client_.on_fixed_update(*this, FixedUpdate{tick_, config_.fixed_step});
            if (!stop_requested_ &&
                !scene_->ecs().progress(std::chrono::duration<double>(config_.fixed_step).count()))
                stop_requested_ = true;
            accumulator_ -= config_.fixed_step;
            ++tick_;
            ++frame.fixed_steps;
        }
        if (accumulator_ >= config_.fixed_step) {
            const auto dropped = accumulator_ - (accumulator_ % config_.fixed_step);
            frame.dropped += dropped;
            accumulator_ -= dropped;
        }
        frame.interpolation = static_cast<double>(accumulator_.count()) /
                              static_cast<double>(config_.fixed_step.count());
        if (!stop_requested_)
            client_.on_update(*this, frame);
        advancing_ = false;
        if (stop_requested_)
            (void)shutdown();
        return frame;
    } catch (const std::exception &e) {
        advancing_ = false;
        log_error(e.what());
        (void)shutdown();
        return fail(ErrorCode::Internal, std::string("Application callback failed: ") + e.what());
    } catch (...) {
        advancing_ = false;
        (void)shutdown();
        return fail(ErrorCode::Internal, "Application callback threw an unknown exception");
    }
}
Result<void> Application::shutdown() {
    if (!owner())
        return fail(ErrorCode::WrongThread, "Application shutdown requires owner thread");
    if (advancing_) {
        stop_requested_ = true;
        return {};
    }
    if (state_ == ApplicationState::Stopped)
        return {};
    // Mark stopped before invoking callbacks, preventing recursive shutdown.
    state_ = ApplicationState::Stopped;
    if (callback_started_) {
        callback_started_ = false;
        client_.on_stop(*this);
    }
    commands_.reset();
    scene_.reset();
    accumulator_ = Duration{0};
    return {};
}
} // namespace engine
