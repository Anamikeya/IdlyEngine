#include <cstdio>
#include <engine/core/Application.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
namespace engine {
namespace {
spdlog::logger &logger() {
    // Engine-local logger: no global registry/default-logger mutation.
    static auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    static spdlog::logger instance("engine", sink);
    return instance;
}
} // namespace
void log_info(std::string_view message) noexcept {
    try {
        logger().info("{}", message);
    } catch (...) {
        std::fputs("Engine logging failed\n", stderr);
    }
}
void log_error(std::string_view message) noexcept {
    try {
        logger().error("{}", message);
    } catch (...) {
        std::fputs("Engine error logging failed\n", stderr);
    }
}
} // namespace engine
