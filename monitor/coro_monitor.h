#ifndef CORO_MONITOR_H
#define CORO_MONITOR_H

#include <atomic>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>
#include "../log/log.h"

/**
 * @brief 系统健康状态
 */
enum class HealthStatus {
    HEALTHY,    // 健康
    WARNING,    // 警告（接近阈值）
    CRITICAL    // 严重（超过阈值）
};

/**
 * @brief 协程系统监控器
 *
 * 功能：
 * - 实时监控协程和线程池状态
 * - 性能指标统计
 * - 健康检查
 * - 自动告警
 *
 * 使用示例：
 * @code
 * CoroMonitor monitor;
 * monitor.record_coroutine_spawn();
 * monitor.record_cpu_task_submit();
 * monitor.print_dashboard();
 * @endcode
 */
class CoroMonitor {
public:
    CoroMonitor() : start_time_(std::chrono::steady_clock::now()) {
        LOG_INFO("Coroutine monitor initialized");
    }

    // ========== 协程监控 ==========

    void record_coroutine_spawn() {
        total_coroutines_spawned_++;
        active_coroutines_++;
        peak_active_coroutines_ = std::max(
            peak_active_coroutines_.load(),
            active_coroutines_.load()
        );
    }

    void record_coroutine_complete() {
        total_coroutines_completed_++;
        active_coroutines_--;
    }

    void record_coroutine_io_wait() {
        coroutines_waiting_io_++;
    }

    void record_coroutine_io_resume() {
        coroutines_waiting_io_--;
    }

    // ========== CPU任务监控 ==========

    void record_cpu_task_submit() {
        total_cpu_tasks_submitted_++;
        active_cpu_tasks_++;
        peak_active_cpu_tasks_ = std::max(
            peak_active_cpu_tasks_.load(),
            active_cpu_tasks_.load()
        );
    }

    void record_cpu_task_complete(uint64_t execution_time_us) {
        total_cpu_tasks_completed_++;
        active_cpu_tasks_--;
        total_cpu_task_time_us_ += execution_time_us;
    }

    void record_cpu_task_queue_full() {
        cpu_task_queue_full_count_++;
    }

    // ========== I/O监控 ==========

    void record_io_operation(size_t bytes) {
        total_io_operations_++;
        total_io_bytes_ += bytes;
    }

    void record_io_error() {
        total_io_errors_++;
    }

    // ========== 请求统计 ==========

    void record_request() {
        total_requests_++;
    }

    void record_request_complete(uint64_t latency_us) {
        total_requests_completed_++;
        total_request_latency_us_ += latency_us;

        // 更新延迟直方图
        if (latency_us < 1000) {
            latency_0_1ms_++;
        } else if (latency_us < 10000) {
            latency_1_10ms_++;
        } else if (latency_us < 100000) {
            latency_10_100ms_++;
        } else {
            latency_100ms_plus_++;
        }
    }

    // ========== 健康检查 ==========

    HealthStatus check_health() const {
        // 检查协程数量
        if (active_coroutines_ > 8000) {
            LOG_WARN("Health check: Too many active coroutines (%zu)", active_coroutines_.load());
            return HealthStatus::CRITICAL;
        }
        if (active_coroutines_ > 5000) {
            return HealthStatus::WARNING;
        }

        // 检查CPU任务队列
        if (cpu_task_queue_full_count_ > 100) {
            LOG_WARN("Health check: CPU task queue frequently full (%zu times)",
                     cpu_task_queue_full_count_.load());
            return HealthStatus::CRITICAL;
        }

        // 检查I/O错误率
        if (total_io_operations_ > 0) {
            double error_rate = (double)total_io_errors_ / total_io_operations_;
            if (error_rate > 0.1) {  // >10% 错误率
                LOG_WARN("Health check: High I/O error rate (%.1f%%)", error_rate * 100);
                return HealthStatus::CRITICAL;
            }
            if (error_rate > 0.05) {  // >5% 错误率
                return HealthStatus::WARNING;
            }
        }

        return HealthStatus::HEALTHY;
    }

    // ========== 性能指标 ==========

    double get_avg_request_latency_ms() const {
        if (total_requests_completed_ == 0) return 0.0;
        return (double)total_request_latency_us_ / total_requests_completed_ / 1000.0;
    }

    double get_avg_cpu_task_time_ms() const {
        if (total_cpu_tasks_completed_ == 0) return 0.0;
        return (double)total_cpu_task_time_us_ / total_cpu_tasks_completed_ / 1000.0;
    }

    double get_requests_per_second() const {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
        if (elapsed == 0) return 0.0;
        return (double)total_requests_ / elapsed;
    }

    double get_io_throughput_mbps() const {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();
        if (elapsed == 0) return 0.0;
        return (double)total_io_bytes_ / elapsed / (1024 * 1024);
    }

    // ========== 监控仪表盘 ==========

    void print_dashboard() const {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();

        LOG_INFO("====================================================");
        LOG_INFO("          COROUTINE SYSTEM DASHBOARD");
        LOG_INFO("====================================================");
        LOG_INFO("Uptime: %ld seconds (%.1f hours)", elapsed, elapsed / 3600.0);
        LOG_INFO("");

        // 健康状态
        HealthStatus health = check_health();
        const char* health_str = (health == HealthStatus::HEALTHY) ? "HEALTHY" :
                                  (health == HealthStatus::WARNING) ? "WARNING" : "CRITICAL";
        LOG_INFO("Health Status: %s", health_str);
        LOG_INFO("");

        // 协程统计
        LOG_INFO("--- Coroutine Statistics ---");
        LOG_INFO("Total spawned:      %zu", total_coroutines_spawned_.load());
        LOG_INFO("Total completed:    %zu", total_coroutines_completed_.load());
        LOG_INFO("Active:             %zu", active_coroutines_.load());
        LOG_INFO("Peak active:        %zu", peak_active_coroutines_.load());
        LOG_INFO("Waiting for I/O:    %zu", coroutines_waiting_io_.load());
        LOG_INFO("");

        // CPU任务统计
        LOG_INFO("--- CPU Task Statistics ---");
        LOG_INFO("Total submitted:    %zu", total_cpu_tasks_submitted_.load());
        LOG_INFO("Total completed:    %zu", total_cpu_tasks_completed_.load());
        LOG_INFO("Active:             %zu", active_cpu_tasks_.load());
        LOG_INFO("Peak active:        %zu", peak_active_cpu_tasks_.load());
        LOG_INFO("Queue full count:   %zu", cpu_task_queue_full_count_.load());
        LOG_INFO("Avg execution time: %.2f ms", get_avg_cpu_task_time_ms());
        LOG_INFO("");

        // I/O统计
        LOG_INFO("--- I/O Statistics ---");
        LOG_INFO("Total operations:   %zu", total_io_operations_.load());
        LOG_INFO("Total bytes:        %.2f MB", total_io_bytes_.load() / (1024.0 * 1024.0));
        LOG_INFO("Total errors:       %zu", total_io_errors_.load());
        LOG_INFO("Error rate:         %.2f%%",
                 total_io_operations_ > 0 ?
                 (100.0 * total_io_errors_ / total_io_operations_) : 0.0);
        LOG_INFO("Throughput:         %.2f MB/s", get_io_throughput_mbps());
        LOG_INFO("");

        // 请求统计
        LOG_INFO("--- Request Statistics ---");
        LOG_INFO("Total requests:     %zu", total_requests_.load());
        LOG_INFO("Total completed:    %zu", total_requests_completed_.load());
        LOG_INFO("Requests/sec:       %.2f", get_requests_per_second());
        LOG_INFO("Avg latency:        %.2f ms", get_avg_request_latency_ms());
        LOG_INFO("");

        // 延迟分布
        LOG_INFO("--- Latency Distribution ---");
        LOG_INFO("0-1 ms:     %zu (%.1f%%)",
                 latency_0_1ms_.load(),
                 100.0 * latency_0_1ms_ / std::max(1UL, total_requests_completed_.load()));
        LOG_INFO("1-10 ms:    %zu (%.1f%%)",
                 latency_1_10ms_.load(),
                 100.0 * latency_1_10ms_ / std::max(1UL, total_requests_completed_.load()));
        LOG_INFO("10-100 ms:  %zu (%.1f%%)",
                 latency_10_100ms_.load(),
                 100.0 * latency_10_100ms_ / std::max(1UL, total_requests_completed_.load()));
        LOG_INFO("100+ ms:    %zu (%.1f%%)",
                 latency_100ms_plus_.load(),
                 100.0 * latency_100ms_plus_ / std::max(1UL, total_requests_completed_.load()));
        LOG_INFO("====================================================");
    }

    // ========== JSON格式输出（用于API） ==========

    std::string to_json() const {
        std::ostringstream oss;
        oss << "{"
            << "\"health\":\"" << (check_health() == HealthStatus::HEALTHY ? "healthy" :
                                   check_health() == HealthStatus::WARNING ? "warning" : "critical") << "\","
            << "\"coroutines\":{"
            << "\"spawned\":" << total_coroutines_spawned_ << ","
            << "\"completed\":" << total_coroutines_completed_ << ","
            << "\"active\":" << active_coroutines_ << ","
            << "\"peak\":" << peak_active_coroutines_ << ","
            << "\"waiting_io\":" << coroutines_waiting_io_
            << "},"
            << "\"cpu_tasks\":{"
            << "\"submitted\":" << total_cpu_tasks_submitted_ << ","
            << "\"completed\":" << total_cpu_tasks_completed_ << ","
            << "\"active\":" << active_cpu_tasks_ << ","
            << "\"peak\":" << peak_active_cpu_tasks_ << ","
            << "\"queue_full_count\":" << cpu_task_queue_full_count_ << ","
            << "\"avg_time_ms\":" << std::fixed << std::setprecision(2) << get_avg_cpu_task_time_ms()
            << "},"
            << "\"io\":{"
            << "\"operations\":" << total_io_operations_ << ","
            << "\"bytes\":" << total_io_bytes_ << ","
            << "\"errors\":" << total_io_errors_ << ","
            << "\"throughput_mbps\":" << get_io_throughput_mbps()
            << "},"
            << "\"requests\":{"
            << "\"total\":" << total_requests_ << ","
            << "\"completed\":" << total_requests_completed_ << ","
            << "\"rps\":" << get_requests_per_second() << ","
            << "\"avg_latency_ms\":" << get_avg_request_latency_ms()
            << "}"
            << "}";
        return oss.str();
    }

private:
    std::chrono::steady_clock::time_point start_time_;

    // 协程统计
    std::atomic<size_t> total_coroutines_spawned_{0};
    std::atomic<size_t> total_coroutines_completed_{0};
    std::atomic<size_t> active_coroutines_{0};
    std::atomic<size_t> peak_active_coroutines_{0};
    std::atomic<size_t> coroutines_waiting_io_{0};

    // CPU任务统计
    std::atomic<size_t> total_cpu_tasks_submitted_{0};
    std::atomic<size_t> total_cpu_tasks_completed_{0};
    std::atomic<size_t> active_cpu_tasks_{0};
    std::atomic<size_t> peak_active_cpu_tasks_{0};
    std::atomic<size_t> cpu_task_queue_full_count_{0};
    std::atomic<uint64_t> total_cpu_task_time_us_{0};

    // I/O统计
    std::atomic<size_t> total_io_operations_{0};
    std::atomic<size_t> total_io_bytes_{0};
    std::atomic<size_t> total_io_errors_{0};

    // 请求统计
    std::atomic<size_t> total_requests_{0};
    std::atomic<size_t> total_requests_completed_{0};
    std::atomic<uint64_t> total_request_latency_us_{0};

    // 延迟分布
    std::atomic<size_t> latency_0_1ms_{0};
    std::atomic<size_t> latency_1_10ms_{0};
    std::atomic<size_t> latency_10_100ms_{0};
    std::atomic<size_t> latency_100ms_plus_{0};
};

/**
 * @brief 全局监控实例（单例）
 */
inline CoroMonitor& get_global_coro_monitor() {
    static CoroMonitor monitor;
    return monitor;
}

#endif // CORO_MONITOR_H
