#include "engine/test/test.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace engine::test {
namespace {

std::vector<TestCase>& registry()
{
	static std::vector<TestCase> cases;
	return cases;
}

int g_failures_in_case = 0;

bool run_case(const TestCase& test_case)
{
	g_failures_in_case = 0;
	const auto start = std::chrono::steady_clock::now();
	try {
		test_case.function();
	} catch (const RequireFailed&) {
		// Already reported.
	} catch (const std::exception& e) {
		report_failure(test_case.file, test_case.line, std::string("unhandled exception: ") + e.what());
	} catch (...) {
		report_failure(test_case.file, test_case.line, "unhandled non-standard exception");
	}
	const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start);
	const bool passed = g_failures_in_case == 0;
	std::printf("[%s] %.*s (%.2f ms)\n", passed ? "PASS" : "FAIL", static_cast<int>(test_case.name.size()),
		test_case.name.data(), elapsed.count());
	std::fflush(stdout);
	return passed;
}

} // namespace

void register_test(const TestCase& test_case)
{
	registry().push_back(test_case);
}

void report_failure(std::string_view file, int line, std::string_view message)
{
	++g_failures_in_case;
	std::fprintf(stderr, "%.*s(%d): failure: %.*s\n", static_cast<int>(file.size()), file.data(), line,
		static_cast<int>(message.size()), message.data());
	std::fflush(stderr);
}

} // namespace engine::test

int main(int argc, char** argv)
{
	using namespace engine::test;

	auto& cases = registry();
	std::sort(cases.begin(), cases.end(), [](const TestCase& a, const TestCase& b) { return a.name < b.name; });

	const std::vector<std::string_view> args(argv + 1, argv + argc);
	if (args.size() == 1 && args[0] == "--list") {
		for (const TestCase& test_case : cases) {
			std::printf("%.*s\n", static_cast<int>(test_case.name.size()), test_case.name.data());
		}
		return 0;
	}

	if (args.size() == 2 && args[0] == "--run") {
		const auto it = std::find_if(cases.begin(), cases.end(), [&](const TestCase& c) { return c.name == args[1]; });
		if (it == cases.end()) {
			std::fprintf(stderr, "unknown test case: %.*s\n", static_cast<int>(args[1].size()), args[1].data());
			return 2;
		}
		return run_case(*it) ? 0 : 1;
	}

	if (!args.empty()) {
		std::fprintf(stderr, "usage: %s [--list | --run <case>]\n", argv[0]);
		return 2;
	}

	int failed = 0;
	for (const TestCase& test_case : cases) {
		failed += run_case(test_case) ? 0 : 1;
	}
	std::printf("%zu cases, %d failed\n", cases.size(), failed);
	return failed == 0 ? 0 : 1;
}
