// Minimal self-registering test harness for SAIGE graphics modules.
//
// Temporary until Core selects the engine-wide test framework (issue #2, D-17). The macro names are a
// subset of doctest's (TEST_CASE, CHECK, CHECK_FALSE, REQUIRE, REQUIRE_FALSE), so migrating to doctest
// is a header swap, and to GoogleTest a mechanical rename.
#pragma once

#include <cstdio>
#include <cstring>
#include <exception>
#include <vector>

namespace saige::test {

struct TestCase {
    const char* name;
    void (*fn)();
    const char* file;
    int line;
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, void (*fn)(), const char* file, int line) { registry().push_back({name, fn, file, line}); }
};

/// Thrown by REQUIRE to abort the current test case. Test code only; runtime modules never throw.
struct RequireFailure {};

inline int& currentFailures()
{
    static int failures = 0;
    return failures;
}

inline void reportFailure(const char* macro, const char* expr, const char* file, int line)
{
    ++currentFailures();
    std::fprintf(stderr, "  %s:%d: %s(%s) failed\n", file, line, macro, expr);
}

/// Runs all registered tests. An optional argv[1] filters tests by substring.
inline int runAll(int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int passed = 0;
    int failed = 0;
    for (const TestCase& tc : registry()) {
        if (filter != nullptr && std::strstr(tc.name, filter) == nullptr) {
            continue;
        }
        currentFailures() = 0;
        try {
            tc.fn();
        } catch (const RequireFailure&) {
            // already reported
        } catch (const std::exception& e) {
            reportFailure("EXCEPTION", e.what(), tc.file, tc.line);
        } catch (...) {
            reportFailure("EXCEPTION", "unknown exception", tc.file, tc.line);
        }
        if (currentFailures() == 0) {
            ++passed;
            std::printf("[  PASS  ] %s\n", tc.name);
        } else {
            ++failed;
            std::printf("[  FAIL  ] %s (%d failure(s))\n", tc.name, currentFailures());
        }
    }
    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}

} // namespace saige::test

#define SAIGE_TEST_CAT_IMPL(a, b) a##b
#define SAIGE_TEST_CAT(a, b) SAIGE_TEST_CAT_IMPL(a, b)

#define SAIGE_TEST_CASE_IMPL(name, fn)                                                                                 \
    static void fn();                                                                                                  \
    static const ::saige::test::Registrar SAIGE_TEST_CAT(fn, _registrar){name, &fn, __FILE__, __LINE__};               \
    static void fn()

#define TEST_CASE(name) SAIGE_TEST_CASE_IMPL(name, SAIGE_TEST_CAT(saige_test_case_, __COUNTER__))

#define CHECK(...)                                                                                                     \
    ((__VA_ARGS__) ? static_cast<void>(0) : ::saige::test::reportFailure("CHECK", #__VA_ARGS__, __FILE__, __LINE__))

#define CHECK_FALSE(...)                                                                                               \
    (!(__VA_ARGS__) ? static_cast<void>(0)                                                                             \
                    : ::saige::test::reportFailure("CHECK_FALSE", #__VA_ARGS__, __FILE__, __LINE__))

#define REQUIRE(...)                                                                                                   \
    do {                                                                                                               \
        if (!(__VA_ARGS__)) {                                                                                          \
            ::saige::test::reportFailure("REQUIRE", #__VA_ARGS__, __FILE__, __LINE__);                                 \
            throw ::saige::test::RequireFailure{};                                                                     \
        }                                                                                                              \
    } while (false)

#define REQUIRE_FALSE(...) REQUIRE(!(__VA_ARGS__))
