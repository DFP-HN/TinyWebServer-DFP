#ifndef CPU_TASK_H
#define CPU_TASK_H

#include <string>
#include <functional>
#include "task.h"
#include "thread_pool_bridge.h"
#include "../cpu_compute/cpu_intensive.h"
#include "../log/log.h"

/**
 * @brief CPU计算任务的协程包装器
 *
 * 提供便捷的API，让CPU密集型计算可以在协程中安全使用
 *
 * 核心思想：
 * - CPU任务在线程池执行，不阻塞协程调度器
 * - 使用co_await语法，代码清晰易读
 * - 自动处理异常和错误
 *
 * 使用示例：
 * @code
 * Task<void> handle_request() {
 *     // I/O操作
 *     auto request = co_await async_read(...);
 *
 *     // CPU密集计算（在线程池执行）
 *     auto result = co_await compute_primes_async(5, io_mgr);
 *
 *     // 继续I/O操作
 *     co_await async_write(..., result);
 * }
 * @endcode
 */

/**
 * @brief 在线程池中异步计算质数
 * @param level 计算级别（1-5，5最耗时）
 * @param io_mgr io_uring管理器
 * @return 协程任务，返回JSON格式的计算结果
 */
inline auto compute_primes_async(int level, IoUringManager* io_mgr) {
    return run_in_threadpool<std::string>([level]() {
        LOG_DEBUG("Thread pool: Computing primes (level=%d)", level);
        return CPUIntensive::compute_primes(level);
    }, io_mgr);
}

/**
 * @brief 在线程池中异步计算斐波那契数列
 * @param level 计算级别（1-5，5最耗时）
 * @param io_mgr io_uring管理器
 * @return 协程任务，返回JSON格式的计算结果
 */
inline auto compute_fibonacci_async(int level, IoUringManager* io_mgr) {
    return run_in_threadpool<std::string>([level]() {
        LOG_DEBUG("Thread pool: Computing fibonacci (level=%d)", level);
        return CPUIntensive::compute_fibonacci(level);
    }, io_mgr);
}

/**
 * @brief 在线程池中异步进行排序计算
 * @param level 计算级别（1-5，5最耗时）
 * @param io_mgr io_uring管理器
 * @return 协程任务，返回JSON格式的计算结果
 */
inline auto compute_sort_async(int level, IoUringManager* io_mgr) {
    return run_in_threadpool<std::string>([level]() {
        LOG_DEBUG("Thread pool: Computing sort (level=%d)", level);
        return CPUIntensive::compute_sort(level);
    }, io_mgr);
}

/**
 * @brief 在线程池中异步进行矩阵运算
 * @param level 计算级别（1-5，5最耗时）
 * @param io_mgr io_uring管理器
 * @return 协程任务，返回JSON格式的计算结果
 */
inline auto compute_matrix_async(int level, IoUringManager* io_mgr) {
    return run_in_threadpool<std::string>([level]() {
        LOG_DEBUG("Thread pool: Computing matrix (level=%d)", level);
        return CPUIntensive::compute_matrix(level);
    }, io_mgr);
}

/**
 * @brief 在线程池中异步进行混合计算
 * @param level 计算级别（1-5，5最耗时）
 * @param io_mgr io_uring管理器
 * @return 协程任务，返回JSON格式的计算结果
 */
inline auto compute_mixed_async(int level, IoUringManager* io_mgr) {
    return run_in_threadpool<std::string>([level]() {
        LOG_DEBUG("Thread pool: Computing mixed (level=%d)", level);
        return CPUIntensive::compute_mixed(level);
    }, io_mgr);
}

/**
 * @brief 通用的CPU任务异步执行器
 *
 * 可以执行任意的CPU密集型计算
 *
 * @tparam T 返回值类型
 * @param callable 要执行的计算任务
 * @param io_mgr io_uring管理器
 * @return 协程任务，返回计算结果
 *
 * 使用示例：
 * @code
 * auto result = co_await run_cpu_task<int>([&]() {
 *     // 自定义CPU密集计算
 *     int sum = 0;
 *     for (int i = 0; i < 1000000; ++i) {
 *         sum += i * i;
 *     }
 *     return sum;
 * }, io_mgr);
 * @endcode
 */
template<typename T>
inline auto run_cpu_task(std::function<T()> callable, IoUringManager* io_mgr) {
    LOG_DEBUG("Submitting custom CPU task to thread pool");
    return run_in_threadpool<T>(std::move(callable), io_mgr);
}

/**
 * @brief CPU任务工厂（根据任务类型分发）
 *
 * 根据task_type参数选择对应的CPU计算
 *
 * @param task_type 任务类型（"primes", "fibonacci", "sort", "matrix", "mixed"）
 * @param level 计算级别（1-5）
 * @param io_mgr io_uring管理器
 * @return 协程任务，返回JSON格式的计算结果
 *
 * 使用示例：
 * @code
 * // 根据URL参数决定计算类型
 * auto result = co_await dispatch_cpu_task(task_type_from_url, level, io_mgr);
 * @endcode
 */
inline Task<std::string> dispatch_cpu_task(
    const char* task_type,
    int level,
    IoUringManager* io_mgr
) {
    if (strcmp(task_type, "primes") == 0) {
        co_return co_await compute_primes_async(level, io_mgr);
    } else if (strcmp(task_type, "fibonacci") == 0) {
        co_return co_await compute_fibonacci_async(level, io_mgr);
    } else if (strcmp(task_type, "sort") == 0) {
        co_return co_await compute_sort_async(level, io_mgr);
    } else if (strcmp(task_type, "matrix") == 0) {
        co_return co_await compute_matrix_async(level, io_mgr);
    } else if (strcmp(task_type, "mixed") == 0) {
        co_return co_await compute_mixed_async(level, io_mgr);
    } else {
        // 默认使用混合计算
        LOG_WARN("Unknown CPU task type '%s', using 'mixed'", task_type);
        co_return co_await compute_mixed_async(level, io_mgr);
    }
}

/**
 * @brief 解析CPU计算请求的参数
 *
 * 从URL查询字符串中提取task_type和level参数
 *
 * @param url 完整URL（如："/cpu_compute?task=primes&level=3"）
 * @param task_type 输出：任务类型
 * @param level 输出：计算级别
 * @return true=解析成功, false=解析失败
 */
inline bool parse_cpu_task_params(const char* url, char* task_type, size_t task_type_size, int* level) {
    // 查找查询字符串
    const char* query = strchr(url, '?');
    if (!query) {
        return false;
    }
    query++;  // 跳过'?'

    // 默认值
    strncpy(task_type, "mixed", task_type_size);
    *level = 3;

    // 解析task参数
    const char* task_param = strstr(query, "task=");
    if (task_param) {
        task_param += 5;  // 跳过"task="
        const char* end = strchr(task_param, '&');
        size_t len = end ? (end - task_param) : strlen(task_param);
        if (len < task_type_size) {
            strncpy(task_type, task_param, len);
            task_type[len] = '\0';
        }
    }

    // 解析level参数
    const char* level_param = strstr(query, "level=");
    if (level_param) {
        level_param += 6;  // 跳过"level="
        *level = atoi(level_param);
        // 限制范围
        if (*level < 1) *level = 1;
        if (*level > 5) *level = 5;
    }

    return true;
}

#endif // CPU_TASK_H
