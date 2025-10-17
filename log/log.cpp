#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"

using namespace std;

// 全局Log实例指针（用于向后兼容）
Log* g_log_instance = nullptr;

Log::Log()
    : m_count(0),
      m_split_lines(0),
      m_log_buf_size(0),
      m_today(0),
      m_is_async(false),
      m_close_log(0),
      m_stop_thread(false)
{
}

Log::~Log()
{
    // 停止异步写线程
    if (m_write_thread && m_write_thread->joinable())
    {
        m_stop_thread = true;

        // 如果有阻塞队列，推送一个空字符串唤醒线程
        if (m_log_queue)
        {
            m_log_queue->push("");
        }

        m_write_thread->join();
    }

    // 智能指针自动释放资源：m_fp, m_buf, m_log_queue, m_write_thread
}

bool Log::init(const char *file_name, int close_log, int log_buf_size,
               int split_lines, int max_queue_size)
{
    // 如果设置了max_queue_size，则设置为异步
    if (max_queue_size >= 1)
    {
        m_is_async = true;

        // 使用智能指针创建阻塞队列
        try
        {
            m_log_queue = std::make_unique<block_queue<string>>(max_queue_size);
        }
        catch (const std::exception& e)
        {
            cerr << "Failed to create log queue: " << e.what() << endl;
            return false;
        }

        // 创建异步写线程
        m_write_thread = std::make_unique<std::thread>(&Log::async_write_log, this);
    }

    m_close_log = close_log;
    m_log_buf_size = log_buf_size;
    m_buf = std::make_unique<char[]>(m_log_buf_size);  // 智能指针管理缓冲区
    memset(m_buf.get(), '\0', m_log_buf_size);
    m_split_lines = split_lines;

    time_t t = time(NULL);
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    const char *p = strrchr(file_name, '/');
    char log_full_name[256] = {0};

    if (p == NULL)
    {
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s",
                 my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
        m_dir_name = "";
        m_log_name = file_name;
    }
    else
    {
        m_log_name = p + 1;
        m_dir_name = string(file_name, p - file_name + 1);
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s",
                 m_dir_name.c_str(), my_tm.tm_year + 1900, my_tm.tm_mon + 1,
                 my_tm.tm_mday, m_log_name.c_str());
    }

    m_today = my_tm.tm_mday;

    // 使用智能指针管理文件
    FILE* fp = fopen(log_full_name, "a");
    if (fp == NULL)
    {
        return false;
    }
    m_fp.reset(fp);  // 智能指针接管所有权

    return true;
}

void Log::write_log(int level, const char *format, ...)
{
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    // 日志级别字符串
    const char* level_str;
    switch (level)
    {
    case DEBUG:
        level_str = "[debug]:";
        break;
    case INFO:
        level_str = "[info]:";
        break;
    case WARN:
        level_str = "[warn]:";
        break;
    case ERROR:
        level_str = "[erro]:";
        break;
    default:
        level_str = "[info]:";
        break;
    }

    // 日志行数递增，检查是否需要切换文件
    {
        LockGuard lock(m_mutex);
        m_count++;

        if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0)
        {
            char new_log[256] = {0};
            fflush(m_fp.get());

            char tail[16] = {0};
            snprintf(tail, 16, "%d_%02d_%02d_",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);

            if (m_today != my_tm.tm_mday)
            {
                snprintf(new_log, 255, "%s%s%s",
                         m_dir_name.c_str(), tail, m_log_name.c_str());
                m_today = my_tm.tm_mday;
                m_count = 0;
            }
            else
            {
                snprintf(new_log, 255, "%s%s%s.%lld",
                         m_dir_name.c_str(), tail, m_log_name.c_str(),
                         m_count / m_split_lines);
            }

            // 重新打开文件
            FILE* fp = fopen(new_log, "a");
            if (fp != NULL)
            {
                m_fp.reset(fp);  // 智能指针自动关闭旧文件并接管新文件
            }
        }
    }

    // 格式化日志内容
    va_list valst;
    va_start(valst, format);

    string log_str;
    {
        LockGuard lock(m_mutex);

        // 写入时间戳和日志级别
        int n = snprintf(m_buf.get(), 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                         my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                         my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec,
                         level_str);

        int m = vsnprintf(m_buf.get() + n, m_log_buf_size - n - 1, format, valst);
        m_buf[n + m] = '\n';
        m_buf[n + m + 1] = '\0';
        log_str = m_buf.get();
    }

    va_end(valst);

    // 根据是否异步决定写入方式
    if (m_is_async && m_log_queue && !m_log_queue->full())
    {
        m_log_queue->push(log_str);
    }
    else
    {
        LockGuard lock(m_mutex);
        if (m_fp)
        {
            fputs(log_str.c_str(), m_fp.get());
        }
    }
}

void Log::flush(void)
{
    LockGuard lock(m_mutex);
    if (m_fp)
    {
        fflush(m_fp.get());
    }
}

// 异步写日志线程函数
void Log::async_write_log()
{
    string single_log;

    // 从阻塞队列中取出日志并写入文件
    while (!m_stop_thread)
    {
        if (m_log_queue->pop(single_log))
        {
            // 空字符串是停止信号
            if (single_log.empty() && m_stop_thread)
            {
                break;
            }

            LockGuard lock(m_mutex);
            if (m_fp)
            {
                fputs(single_log.c_str(), m_fp.get());
            }
        }
    }
}
