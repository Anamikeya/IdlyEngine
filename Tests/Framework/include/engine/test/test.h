#pragma once

// Minimal self-registering test framework. No third-party dependency so it builds on every target.
//
//   ENGINE_TEST(physics_world_creates) { ENGINE_CHECK(world.valid()); }
//
// ENGINE_CHECK records a failure and continues; ENGINE_REQUIRE aborts the current case.
// The runner supports `--list` (used by CTest discovery) and `--run <name>`; with no arguments it runs all cases.

#include <cmath>
#include <sstream>
#include <string>
#include <string_view>

namespace engine::test {

using TestFunction = void (*)();

struct TestCase {
	std::string_view name;
	TestFunction function;
	std::string_view file;
	int line;
};

void register_test(const TestCase& test_case);
void report_failure(std::string_view file, int line, std::string_view message);

// Thrown by ENGINE_REQUIRE to leave the current case. Not derived from std::exception on purpose,
// so code under test that catches std::exception cannot swallow it.
struct RequireFailed {};

struct Registrar {
	Registrar(std::string_view name, TestFunction function, std::string_view file, int line)
	{
		register_test(TestCase{name, function, file, line});
	}
};

template <typename A, typename B>
std::string describe_comparison(std::string_view expression, const A& a, const B& b)
{
	std::ostringstream out;
	out << expression << " (" << a << " vs " << b << ")";
	return out.str();
}

inline bool near(double a, double b, double tolerance)
{
	return std::fabs(a - b) <= tolerance;
}

} // namespace engine::test

#define ENGINE_TEST_CONCAT_INNER(a, b) a##b
#define ENGINE_TEST_CONCAT(a, b) ENGINE_TEST_CONCAT_INNER(a, b)

#define ENGINE_TEST(name)                                                                                    \
	static void name();                                                                                      \
	static const ::engine::test::Registrar ENGINE_TEST_CONCAT(name, _registrar){#name, &name, __FILE__, __LINE__}; \
	static void name()

#define ENGINE_CHECK(condition)                                                                              \
	do {                                                                                                     \
		if (!(condition)) {                                                                                  \
			::engine::test::report_failure(__FILE__, __LINE__, "CHECK(" #condition ")");                     \
		}                                                                                                    \
	} while (false)

#define ENGINE_REQUIRE(condition)                                                                            \
	do {                                                                                                     \
		if (!(condition)) {                                                                                  \
			::engine::test::report_failure(__FILE__, __LINE__, "REQUIRE(" #condition ")");                   \
			throw ::engine::test::RequireFailed{};                                                           \
		}                                                                                                    \
	} while (false)

#define ENGINE_CHECK_EQ(a, b)                                                                                \
	do {                                                                                                     \
		const auto& engine_test_a = (a);                                                                     \
		const auto& engine_test_b = (b);                                                                     \
		if (!(engine_test_a == engine_test_b)) {                                                             \
			::engine::test::report_failure(__FILE__, __LINE__,                                               \
				::engine::test::describe_comparison("CHECK_EQ(" #a ", " #b ")", engine_test_a, engine_test_b)); \
		}                                                                                                    \
	} while (false)

#define ENGINE_CHECK_NEAR(a, b, tolerance)                                                                   \
	do {                                                                                                     \
		const double engine_test_a = static_cast<double>(a);                                                 \
		const double engine_test_b = static_cast<double>(b);                                                 \
		if (!::engine::test::near(engine_test_a, engine_test_b, static_cast<double>(tolerance))) {           \
			::engine::test::report_failure(__FILE__, __LINE__,                                               \
				::engine::test::describe_comparison("CHECK_NEAR(" #a ", " #b ", " #tolerance ")", engine_test_a, engine_test_b)); \
		}                                                                                                    \
	} while (false)
