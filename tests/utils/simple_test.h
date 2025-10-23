#ifndef TESTS_UTILS_SIMPLE_TEST_H
#define TESTS_UTILS_SIMPLE_TEST_H

/**
 * @brief 简化的测试框架（当Google Test不可用时使用）
 *
 * 提供基本的TEST宏和EXPECT_* / ASSERT_* 宏
 */

#include <iostream>
#include <vector>
#include <functional>
#include <string>
#include <sstream>
#include <cmath>

// 尝试包含Google Test，如果失败则使用自定义实现
#ifdef USE_GTEST
    #include <gtest/gtest.h>
#else

// 测试统计
struct TestStats {
    int total = 0;
    int passed = 0;
    int failed = 0;
};

static TestStats g_test_stats;
static std::string g_current_test_name;

// 颜色输出
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_RESET   "\x1b[0m"

// 测试用例定义
#define TEST(test_suite, test_name) \
    void test_suite##_##test_name##_TestBody(); \
    static void test_suite##_##test_name##_Runner() { \
        g_current_test_name = #test_suite "." #test_name; \
        g_test_stats.total++; \
        std::cout << "[ RUN      ] " << g_current_test_name << std::endl; \
        try { \
            test_suite##_##test_name##_TestBody(); \
            g_test_stats.passed++; \
            std::cout << ANSI_COLOR_GREEN << "[       OK ] " << ANSI_COLOR_RESET \
                      << g_current_test_name << std::endl; \
        } catch (const std::exception& e) { \
            g_test_stats.failed++; \
            std::cout << ANSI_COLOR_RED << "[  FAILED  ] " << ANSI_COLOR_RESET \
                      << g_current_test_name << std::endl; \
            std::cout << "  Exception: " << e.what() << std::endl; \
        } catch (...) { \
            g_test_stats.failed++; \
            std::cout << ANSI_COLOR_RED << "[  FAILED  ] " << ANSI_COLOR_RESET \
                      << g_current_test_name << std::endl; \
            std::cout << "  Unknown exception" << std::endl; \
        } \
    } \
    static struct test_suite##_##test_name##_Registrar { \
        test_suite##_##test_name##_Registrar() { \
            get_test_registry().push_back(test_suite##_##test_name##_Runner); \
        } \
    } test_suite##_##test_name##_registrar; \
    void test_suite##_##test_name##_TestBody()

// Fixture测试
#define TEST_F(test_fixture, test_name) \
    class test_fixture##_##test_name##_Test : public test_fixture { \
    public: \
        void TestBody(); \
    }; \
    void test_fixture##_##test_name##_Runner() { \
        g_current_test_name = #test_fixture "." #test_name; \
        g_test_stats.total++; \
        std::cout << "[ RUN      ] " << g_current_test_name << std::endl; \
        try { \
            test_fixture##_##test_name##_Test test; \
            test.SetUp(); \
            test.TestBody(); \
            test.TearDown(); \
            g_test_stats.passed++; \
            std::cout << ANSI_COLOR_GREEN << "[       OK ] " << ANSI_COLOR_RESET \
                      << g_current_test_name << std::endl; \
        } catch (const std::exception& e) { \
            g_test_stats.failed++; \
            std::cout << ANSI_COLOR_RED << "[  FAILED  ] " << ANSI_COLOR_RESET \
                      << g_current_test_name << std::endl; \
            std::cout << "  Exception: " << e.what() << std::endl; \
        } catch (...) { \
            g_test_stats.failed++; \
            std::cout << ANSI_COLOR_RED << "[  FAILED  ] " << ANSI_COLOR_RESET \
                      << g_current_test_name << std::endl; \
        } \
    } \
    static struct test_fixture##_##test_name##_Registrar { \
        test_fixture##_##test_name##_Registrar() { \
            get_test_registry().push_back(test_fixture##_##test_name##_Runner); \
        } \
    } test_fixture##_##test_name##_registrar; \
    void test_fixture##_##test_name##_Test::TestBody()

// 断言宏
#define EXPECT_TRUE(condition) \
    if (!(condition)) { \
        std::cerr << __FILE__ << ":" << __LINE__ << ": Failure" << std::endl; \
        std::cerr << "Expected: " << #condition << " is true" << std::endl; \
        std::cerr << "  Actual: false" << std::endl; \
        throw std::runtime_error("EXPECT_TRUE failed"); \
    }

#define EXPECT_FALSE(condition) \
    if (condition) { \
        std::cerr << __FILE__ << ":" << __LINE__ << ": Failure" << std::endl; \
        std::cerr << "Expected: " << #condition << " is false" << std::endl; \
        std::cerr << "  Actual: true" << std::endl; \
        throw std::runtime_error("EXPECT_FALSE failed"); \
    }

#define EXPECT_EQ(val1, val2) \
    if ((val1) != (val2)) { \
        std::cerr << __FILE__ << ":" << __LINE__ << ": Failure" << std::endl; \
        std::cerr << "Expected: " << #val1 << " == " << #val2 << std::endl; \
        std::cerr << "  Actual: " << (val1) << " vs " << (val2) << std::endl; \
        throw std::runtime_error("EXPECT_EQ failed"); \
    }

#define EXPECT_NE(val1, val2) \
    if ((val1) == (val2)) { \
        std::cerr << __FILE__ << ":" << __LINE__ << ": Failure" << std::endl; \
        std::cerr << "Expected: " << #val1 << " != " << #val2 << std::endl; \
        throw std::runtime_error("EXPECT_NE failed"); \
    }

#define EXPECT_LT(val1, val2) \
    if ((val1) >= (val2)) { \
        std::cerr << __FILE__ << ":" << __LINE__ << ": Failure" << std::endl; \
        std::cerr << "Expected: " << #val1 << " < " << #val2 << std::endl; \
        std::cerr << "  Actual: " << (val1) << " vs " << (val2) << std::endl; \
        throw std::runtime_error("EXPECT_LT failed"); \
    }

#define EXPECT_GT(val1, val2) \
    if ((val1) <= (val2)) { \
        std::cerr << __FILE__ << ":" << __LINE__ << ": Failure" << std::endl; \
        std::cerr << "Expected: " << #val1 << " > " << #val2 << std::endl; \
        std::cerr << "  Actual: " << (val1) << " vs " << (val2) << std::endl; \
        throw std::runtime_error("EXPECT_GT failed"); \
    }

#define EXPECT_LE(val1, val2) \
    if ((val1) > (val2)) { \
        std::cerr << __FILE__ << ":" << __LINE__ << ": Failure" << std::endl; \
        std::cerr << "Expected: " << #val1 << " <= " << #val2 << std::endl; \
        std::cerr << "  Actual: " << (val1) << " vs " << (val2) << std::endl; \
        throw std::runtime_error("EXPECT_LE failed"); \
    }

#define EXPECT_GE(val1, val2) \
    if ((val1) < (val2)) { \
        std::cerr << __FILE__ << ":" << __LINE__ << ": Failure" << std::endl; \
        std::cerr << "Expected: " << #val1 << " >= " << #val2 << std::endl; \
        std::cerr << "  Actual: " << (val1) << " vs " << (val2) << std::endl; \
        throw std::runtime_error("EXPECT_GE failed"); \
    }

#define EXPECT_NEAR(val1, val2, abs_error) \
    if (std::abs((val1) - (val2)) > (abs_error)) { \
        std::cerr << __FILE__ << ":" << __LINE__ << ": Failure" << std::endl; \
        std::cerr << "Expected: abs(" << #val1 << " - " << #val2 << ") <= " << (abs_error) << std::endl; \
        std::cerr << "  Actual: " << std::abs((val1) - (val2)) << std::endl; \
        throw std::runtime_error("EXPECT_NEAR failed"); \
    }

#define EXPECT_STREQ(str1, str2) \
    if (std::string(str1) != std::string(str2)) { \
        std::cerr << __FILE__ << ":" << __LINE__ << ": Failure" << std::endl; \
        std::cerr << "Expected: " << #str1 << " == " << #str2 << std::endl; \
        std::cerr << "  Actual: \"" << (str1) << "\" vs \"" << (str2) << "\"" << std::endl; \
        throw std::runtime_error("EXPECT_STREQ failed"); \
    }

// ASSERT 宏（同EXPECT，但立即退出）
#define ASSERT_TRUE EXPECT_TRUE
#define ASSERT_FALSE EXPECT_FALSE
#define ASSERT_EQ EXPECT_EQ
#define ASSERT_NE EXPECT_NE
#define ASSERT_LT EXPECT_LT
#define ASSERT_GT EXPECT_GT
#define ASSERT_LE EXPECT_LE
#define ASSERT_GE EXPECT_GE
#define ASSERT_STREQ EXPECT_STREQ

// 测试注册表
inline std::vector<std::function<void()>>& get_test_registry() {
    static std::vector<std::function<void()>> registry;
    return registry;
}

// 运行所有测试
inline int RUN_ALL_TESTS() {
    std::cout << ANSI_COLOR_GREEN << "[==========] " << ANSI_COLOR_RESET
              << "Running " << get_test_registry().size() << " tests" << std::endl;

    for (auto& test_func : get_test_registry()) {
        test_func();
    }

    std::cout << ANSI_COLOR_GREEN << "[==========] " << ANSI_COLOR_RESET
              << g_test_stats.total << " tests ran" << std::endl;
    std::cout << ANSI_COLOR_GREEN << "[  PASSED  ] " << ANSI_COLOR_RESET
              << g_test_stats.passed << " tests" << std::endl;

    if (g_test_stats.failed > 0) {
        std::cout << ANSI_COLOR_RED << "[  FAILED  ] " << ANSI_COLOR_RESET
                  << g_test_stats.failed << " tests" << std::endl;
    }

    return (g_test_stats.failed == 0) ? 0 : 1;
}

#endif // USE_GTEST

#endif // TESTS_UTILS_SIMPLE_TEST_H
