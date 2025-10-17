/*************************************************************
 * 重构后的日志系统 - 移除单例，使用智能指针和RAII
 *
 * 改进：
 * 1. 移除全局单例，支持依赖注入
 * 2. 使用智能指针管理资源（FILE*, buffer, queue）
 * 3. 使用 std::string 替代固定大小数组
 * 4. 使用 std::thread 替代 pthread
 * 5. RAII 锁管理，异常安全
 * 6. 添加日志级别枚举
 * 7. 可配置的刷新策略
 *************************************************************/

#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <string>
#include <memory>
#include <thread>
#include <stdarg.h>
#include "block_queue.h"

using namespace std;

class Log
{
public:
    // 日志级别枚举
    enum LogLevel
    {
        DEBUG = 0,
        INFO = 1,
        WARN = 2,
        ERROR = 3
    };

    // 构造函数（非单例）
    Log();

    // 析构函数
    ~Log();

    // 禁用拷贝
    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;

    // 初始化日志系统
    // 参数：文件名、是否关闭日志、缓冲区大小、最大行数、队列大小
    bool init(const char *file_name, int close_log,
              int log_buf_size = 8192, int split_lines = 5000000,
              int max_queue_size = 0);

    // 写日志
    void write_log(int level, const char *format, ...);

    // 刷新日志
    void flush(void);

    // 获取是否关闭日志
    int get_close_log() const { return m_close_log; }

private:
    // 异步写日志线程函数
    void async_write_log();

    // FILE* 自定义删除器
    struct FileDeleter
    {
        void operator()(FILE* fp) const
        {
            if (fp != nullptr)
            {
                fclose(fp);
            }
        }
    };

private:
    std::string m_dir_name;   // 路径名（使用 string）
    std::string m_log_name;   // log文件名（使用 string）
    int m_split_lines;        // 日志最大行数
    int m_log_buf_size;       // 日志缓冲区大小
    long long m_count;        // 日志行数记录
    int m_today;              // 记录当前时间是哪一天

    std::unique_ptr<FILE, FileDeleter> m_fp;  // 文件指针（智能指针管理）
    std::unique_ptr<char[]> m_buf;            // 缓冲区（智能指针管理）
    std::unique_ptr<block_queue<string>> m_log_queue;  // 阻塞队列（智能指针管理）
    std::unique_ptr<std::thread> m_write_thread;       // 异步写线程（智能指针管理）

    bool m_is_async;          // 是否异步标志位
    locker m_mutex;           // 互斥锁
    int m_close_log;          // 关闭日志开关
    bool m_stop_thread;       // 停止异步线程标志
};

// ====================
// 日志宏定义
// ====================

// 支持依赖注入的日志宏（推荐使用）
#define LOG_DEBUG_INST(logger, format, ...) \
    do { \
        if (logger && 0 == logger->get_close_log()) { \
            logger->write_log(Log::DEBUG, format, ##__VA_ARGS__); \
        } \
    } while(0)

#define LOG_INFO_INST(logger, format, ...) \
    do { \
        if (logger && 0 == logger->get_close_log()) { \
            logger->write_log(Log::INFO, format, ##__VA_ARGS__); \
        } \
    } while(0)

#define LOG_WARN_INST(logger, format, ...) \
    do { \
        if (logger && 0 == logger->get_close_log()) { \
            logger->write_log(Log::WARN, format, ##__VA_ARGS__); \
        } \
    } while(0)

#define LOG_ERROR_INST(logger, format, ...) \
    do { \
        if (logger && 0 == logger->get_close_log()) { \
            logger->write_log(Log::ERROR, format, ##__VA_ARGS__); \
        } \
    } while(0)

// 全局Log实例指针（用于向后兼容）
extern Log* g_log_instance;

// 向后兼容的宏（使用全局实例）
#define LOG_DEBUG(format, ...) LOG_DEBUG_INST(g_log_instance, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...) LOG_INFO_INST(g_log_instance, format, ##__VA_ARGS__)
#define LOG_WARN(format, ...) LOG_WARN_INST(g_log_instance, format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) LOG_ERROR_INST(g_log_instance, format, ##__VA_ARGS__)

#endif
