#pragma once
#include <cstddef>
#include <string>
#include <utility>
#include <variant>
namespace engine {
enum class ErrorCode {
    InvalidArgument,
    NotFound,
    Conflict,
    PermissionDenied,
    WrongThread,
    InvalidState,
    Cycle,
    ParseError,
    Internal
};
struct Error {
    ErrorCode code;
    std::string message;
    std::size_t operation = 0;
};
template <class T> class Result {
    std::variant<T, Error> data_;

  public:
    Result(T value) : data_(std::move(value)) {}
    Result(Error error) : data_(std::move(error)) {}
    explicit operator bool() const noexcept {
        return std::holds_alternative<T>(data_);
    }
    T &value() {
        return std::get<T>(data_);
    }
    const T &value() const {
        return std::get<T>(data_);
    }
    const Error &error() const {
        return std::get<Error>(data_);
    }
};
template <> class Result<void> {
    std::variant<std::monostate, Error> data_;

  public:
    Result() = default;
    Result(Error error) : data_(std::move(error)) {}
    explicit operator bool() const noexcept {
        return std::holds_alternative<std::monostate>(data_);
    }
    const Error &error() const {
        return std::get<Error>(data_);
    }
};
inline Error fail(ErrorCode code, std::string message) {
    return {code, std::move(message), 0};
}
} // namespace engine
