#include "work_stealing_pool.h"

// 初始化全局状态变量
namespace work_stealing_globals
{
    std::atomic<uint64_t> work_announcement_board(0);
    std::mutex g_sleep_mutex;
    std::condition_variable g_sleeper_cv;
}
