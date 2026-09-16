// SPDX-License-Identifier: MIT
//
// The smallest thing that can be called a test runner.
//
// No gtest, no Catch2, and the reason is the same rule that governs the
// library under test: the MedFramework has exactly two external dependencies
// and that is a claim the thesis makes. A test suite that needed a third would
// not break the library's claim, but it would make "clone the repository and
// run the evidence" depend on a package the reader has to install first - and
// evidence nobody can rerun is the thing this directory exists to fix.
//
// What it gives: a count, a file:line for every failure, a non-zero exit
// status, and a section header so a failure says which behaviour broke.

#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <type_traits>

namespace medtest {

struct Counters {
    int checks = 0;
    int failures = 0;
    std::string section;
};

inline Counters& counters() {
    static Counters instance;
    return instance;
}

inline void section(const char* name) {
    counters().section = name;
    std::printf("\n-- %s\n", name);
}

inline void record(bool passed, const char* expression, const char* file, int line,
                   const std::string& detail) {
    Counters& c = counters();
    ++c.checks;
    if (passed) {
        return;
    }
    ++c.failures;
    std::printf("   FALHA  %s:%d\n          %s\n", file, line, expression);
    if (!detail.empty()) {
        std::printf("          %s\n", detail.c_str());
    }
}

inline int report() {
    const Counters& c = counters();
    std::printf("\n%d verificações, %d falhas\n", c.checks, c.failures);
    return c.failures == 0 ? 0 : 1;
}

/// Comparison helpers that print the operands. A failure that only says
/// "expected == actual" costs a debugging session that a printed value does
/// not.
template <typename T>
std::string render(const T& value) {
    std::ostringstream out;
    if constexpr (std::is_same_v<T, std::string>) {
        out << '\'' << value << '\'';
    } else if constexpr (std::is_same_v<T, bool>) {
        out << (value ? "true" : "false");
    } else {
        out << value;
    }
    return out.str();
}

/// The two operands may have different types: CHECK_EQ deduces each separately,
/// so a comparison of a size_t against a literal must still print both.
template <typename A, typename B>
std::string describe(const A& a, const B& b) {
    return "esperado " + render(b) + ", obtido " + render(a);
}

}  // namespace medtest

#define CHECK(expr) \
    ::medtest::record((expr), #expr, __FILE__, __LINE__, std::string())

#define CHECK_MSG(expr, detail) \
    ::medtest::record((expr), #expr, __FILE__, __LINE__, (detail))

#define CHECK_EQ(actual, expected)                                          \
    do {                                                                    \
        const auto medtest_a = (actual);                                    \
        const auto medtest_b = (expected);                                  \
        ::medtest::record(medtest_a == medtest_b, #actual " == " #expected, \
                          __FILE__, __LINE__,                               \
                          ::medtest::describe(medtest_a, medtest_b));       \
    } while (0)

/// Absolute tolerance, stated at the call site. There is no default epsilon on
/// purpose: every numeric comparison in this suite is about a physical
/// quantity, and how close is close enough is a property of that quantity.
#define CHECK_NEAR(actual, expected, tolerance)                                  \
    do {                                                                         \
        const double medtest_a = static_cast<double>(actual);                    \
        const double medtest_b = static_cast<double>(expected);                  \
        const double medtest_t = static_cast<double>(tolerance);                 \
        ::medtest::record(std::fabs(medtest_a - medtest_b) <= medtest_t,         \
                          #actual " ~= " #expected, __FILE__, __LINE__,          \
                          ::medtest::describe(medtest_a, medtest_b) +            \
                              " (tolerância " + std::to_string(medtest_t) + ")"); \
    } while (0)

#define CHECK_STATUS(actual, expected)                                       \
    do {                                                                     \
        const ::med::Status medtest_a = (actual);                            \
        const ::med::Status medtest_b = (expected);                          \
        ::medtest::record(medtest_a == medtest_b, #actual " == " #expected,  \
                          __FILE__, __LINE__,                                \
                          ::medtest::describe(                               \
                              std::string(::med::toString(medtest_a)),       \
                              std::string(::med::toString(medtest_b))));     \
    } while (0)
