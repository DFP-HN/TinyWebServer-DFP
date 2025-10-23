#ifndef TASK_DISPATCHER_H
#define TASK_DISPATCHER_H

#include <string>
#include <unordered_set>
#include <atomic>
#include <cstring>
#include "../log/log.h"

/**
 * @brief 任务类型
 */
enum class TaskType {
    IO_INTENSIVE,      // I/O密集型（适合协程）
    CPU_INTENSIVE,     // CPU密集型（适合线程池）
    MIXED,             // 混合型（少量CPU计算+I/O）
    UNKNOWN            // 未知类型
};

/**
 * @brief 任务分发统计信息
 */
struct DispatchStats {
    std::atomic<size_t> total_dispatched{0};
    std::atomic<size_t> io_tasks{0};
    std::atomic<size_t> cpu_tasks{0};
    std::atomic<size_t> mixed_tasks{0};
    std::atomic<size_t> unknown_tasks{0};

    void record(TaskType type) {
        total_dispatched++;
        switch (type) {
            case TaskType::IO_INTENSIVE:
                io_tasks++;
                break;
            case TaskType::CPU_INTENSIVE:
                cpu_tasks++;
                break;
            case TaskType::MIXED:
                mixed_tasks++;
                break;
            case TaskType::UNKNOWN:
                unknown_tasks++;
                break;
        }
    }

    void print_stats() const {
        LOG_INFO("=== Task Dispatcher Stats ===");
        LOG_INFO("Total dispatched: %zu", total_dispatched.load());
        LOG_INFO("I/O tasks: %zu (%.1f%%)",
                 io_tasks.load(),
                 total_dispatched > 0 ? 100.0 * io_tasks.load() / total_dispatched.load() : 0.0);
        LOG_INFO("CPU tasks: %zu (%.1f%%)",
                 cpu_tasks.load(),
                 total_dispatched > 0 ? 100.0 * cpu_tasks.load() / total_dispatched.load() : 0.0);
        LOG_INFO("Mixed tasks: %zu (%.1f%%)",
                 mixed_tasks.load(),
                 total_dispatched > 0 ? 100.0 * mixed_tasks.load() / total_dispatched.load() : 0.0);
        LOG_INFO("Unknown tasks: %zu (%.1f%%)",
                 unknown_tasks.load(),
                 total_dispatched > 0 ? 100.0 * unknown_tasks.load() / total_dispatched.load() : 0.0);
    }
};

/**
 * @brief 智能任务分发器
 *
 * 功能：
 * - 根据URL路径自动识别任务类型
 * - 维护CPU密集路径白名单
 * - 提供分发决策API
 * - 收集统计信息
 *
 * 使用示例：
 * @code
 * TaskDispatcher dispatcher;
 * dispatcher.add_cpu_intensive_path("/cpu_compute");
 * dispatcher.add_cpu_intensive_path("/hash");
 *
 * TaskType type = dispatcher.classify_request("GET /cpu_compute?level=5 HTTP/1.1");
 * if (type == TaskType::CPU_INTENSIVE) {
 *     // 使用线程池
 * } else {
 *     // 使用协程
 * }
 * @endcode
 */
class TaskDispatcher {
public:
    TaskDispatcher() {
        // 初始化CPU密集路径白名单
        init_cpu_intensive_paths();

        LOG_INFO("TaskDispatcher initialized");
    }

    /**
     * @brief 添加CPU密集路径
     */
    void add_cpu_intensive_path(const char* path) {
        cpu_intensive_paths_.insert(path);
        LOG_DEBUG("Added CPU intensive path: %s", path);
    }

    /**
     * @brief 分类HTTP请求
     * @param request_line HTTP请求行（如："GET /cpu_compute HTTP/1.1"）
     * @return 任务类型
     */
    TaskType classify_request(const char* request_line) {
        if (!request_line) {
            return TaskType::UNKNOWN;
        }

        // 提取URL路径
        char url[256];
        if (!extract_url(request_line, url, sizeof(url))) {
            stats_.record(TaskType::UNKNOWN);
            return TaskType::UNKNOWN;
        }

        // 去除查询参数
        char* query_pos = strchr(url, '?');
        if (query_pos) {
            *query_pos = '\0';
        }

        // 检查是否在CPU密集路径列表中
        TaskType type;
        if (cpu_intensive_paths_.count(url) > 0) {
            type = TaskType::CPU_INTENSIVE;
        } else if (is_upload_request(url)) {
            type = TaskType::IO_INTENSIVE;
        } else if (is_download_request(url)) {
            type = TaskType::IO_INTENSIVE;
        } else if (is_database_request(url)) {
            type = TaskType::MIXED;  // 数据库操作通常是混合型
        } else {
            type = TaskType::IO_INTENSIVE;  // 默认I/O密集
        }

        stats_.record(type);

        LOG_DEBUG("Classified request '%s' -> %s", url, task_type_to_string(type));
        return type;
    }

    /**
     * @brief 根据URL路径分类
     */
    TaskType classify_url(const char* url) {
        if (!url) {
            return TaskType::UNKNOWN;
        }

        // 去除查询参数
        char url_copy[256];
        strncpy(url_copy, url, sizeof(url_copy) - 1);
        url_copy[sizeof(url_copy) - 1] = '\0';

        char* query_pos = strchr(url_copy, '?');
        if (query_pos) {
            *query_pos = '\0';
        }

        TaskType type;
        if (cpu_intensive_paths_.count(url_copy) > 0) {
            type = TaskType::CPU_INTENSIVE;
        } else if (is_upload_request(url_copy)) {
            type = TaskType::IO_INTENSIVE;
        } else if (is_download_request(url_copy)) {
            type = TaskType::IO_INTENSIVE;
        } else if (is_database_request(url_copy)) {
            type = TaskType::MIXED;
        } else {
            type = TaskType::IO_INTENSIVE;
        }

        stats_.record(type);
        return type;
    }

    /**
     * @brief 检查是否应使用协程处理
     */
    bool should_use_coroutine(TaskType type) const {
        return type == TaskType::IO_INTENSIVE || type == TaskType::MIXED;
    }

    /**
     * @brief 检查是否应使用线程池处理
     */
    bool should_use_threadpool(TaskType type) const {
        return type == TaskType::CPU_INTENSIVE;
    }

    /**
     * @brief 获取统计信息
     */
    const DispatchStats& get_stats() const {
        return stats_;
    }

    /**
     * @brief 打印统计信息
     */
    void print_stats() const {
        stats_.print_stats();
    }

private:
    /**
     * @brief 初始化CPU密集路径白名单
     */
    void init_cpu_intensive_paths() {
        // CPU计算端点
        cpu_intensive_paths_.insert("/cpu_compute");
        cpu_intensive_paths_.insert("/compute/primes");
        cpu_intensive_paths_.insert("/compute/fibonacci");
        cpu_intensive_paths_.insert("/compute/sort");
        cpu_intensive_paths_.insert("/compute/matrix");
        cpu_intensive_paths_.insert("/compute/mixed");

        // 哈希计算
        cpu_intensive_paths_.insert("/hash");
        cpu_intensive_paths_.insert("/hash/md5");
        cpu_intensive_paths_.insert("/hash/sha256");

        // 图片处理（如果有）
        cpu_intensive_paths_.insert("/image/resize");
        cpu_intensive_paths_.insert("/image/compress");

        // 数据分析（如果有）
        cpu_intensive_paths_.insert("/analytics");
        cpu_intensive_paths_.insert("/report");
    }

    /**
     * @brief 从HTTP请求行提取URL
     */
    bool extract_url(const char* request_line, char* url, size_t url_size) {
        // 格式: "GET /path HTTP/1.1"
        const char* space1 = strchr(request_line, ' ');
        if (!space1) {
            return false;
        }
        space1++;  // 跳过空格

        const char* space2 = strchr(space1, ' ');
        if (!space2) {
            return false;
        }

        size_t url_len = space2 - space1;
        if (url_len >= url_size) {
            return false;
        }

        strncpy(url, space1, url_len);
        url[url_len] = '\0';
        return true;
    }

    /**
     * @brief 检查是否是上传请求
     */
    bool is_upload_request(const char* url) const {
        return strstr(url, "/upload") != nullptr ||
               strstr(url, "/file") != nullptr;
    }

    /**
     * @brief 检查是否是下载请求
     */
    bool is_download_request(const char* url) const {
        return strstr(url, "/download") != nullptr;
    }

    /**
     * @brief 检查是否是数据库请求
     */
    bool is_database_request(const char* url) const {
        return strstr(url, "/api/") != nullptr ||
               strstr(url, "/search") != nullptr ||
               strstr(url, "/query") != nullptr;
    }

    /**
     * @brief 任务类型转字符串
     */
    const char* task_type_to_string(TaskType type) const {
        switch (type) {
            case TaskType::IO_INTENSIVE: return "I/O_INTENSIVE";
            case TaskType::CPU_INTENSIVE: return "CPU_INTENSIVE";
            case TaskType::MIXED: return "MIXED";
            case TaskType::UNKNOWN: return "UNKNOWN";
            default: return "INVALID";
        }
    }

private:
    std::unordered_set<std::string> cpu_intensive_paths_;  // CPU密集路径白名单
    DispatchStats stats_;                                   // 统计信息
};

/**
 * @brief 全局任务分发器实例（单例）
 */
inline TaskDispatcher& get_global_task_dispatcher() {
    static TaskDispatcher dispatcher;
    return dispatcher;
}

#endif // TASK_DISPATCHER_H
