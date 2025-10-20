#ifndef ZERO_COPY_H
#define ZERO_COPY_H

#include <sys/sendfile.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "http_conn.h"

// 前向声明
class http_conn;

/**
 * @brief 零拷贝管理器
 *
 * 根据文件大小和缓存状态智能选择传输方式：
 * 1. 小文件 (< 64KB): 使用内存缓存
 * 2. 中等文件 (64KB - 4MB): 使用 sendfile 零拷贝
 * 3. 大文件 (> 4MB): 使用 mmap + 分块传输
 */
class ZeroCopyManager {
public:
    // 文件大小阈值
    static const size_t SMALL_FILE_THRESHOLD = 64 * 1024;         // 64KB
    static const size_t LARGE_FILE_THRESHOLD = 4 * 1024 * 1024;   // 4MB

    // 传输方式选择
    enum TransferMethod {
        USE_CACHE,      // 使用内存缓存（小文件）
        USE_SENDFILE,   // 使用 sendfile 零拷贝（中等文件）
        USE_MMAP,       // 使用 mmap 内存映射（大文件）
        USE_WRITEV      // 使用 writev 分散写（头 + 体）
    };

    /**
     * @brief 根据文件大小和缓存状态选择传输方式
     * @param file_size 文件大小
     * @param is_cached 是否已缓存
     * @return 推荐的传输方式
     */
    static TransferMethod choose_method(size_t file_size, bool is_cached);

    /**
     * @brief 使用 sendfile 零拷贝发送文件
     * @param sockfd 套接字文件描述符
     * @param filefd 文件描述符
     * @param offset 文件偏移量（会被修改）
     * @param count 要发送的字节数
     * @return 实际发送的字节数，-1 表示错误
     */
    static ssize_t send_file(int sockfd, int filefd, off_t *offset, size_t count);

    /**
     * @brief 使用 splice 进行管道传输（零拷贝）
     * @param in_fd 输入文件描述符
     * @param out_fd 输出文件描述符
     * @param len 传输长度
     * @return 实际传输的字节数，-1 表示错误
     */
    static ssize_t splice_transfer(int in_fd, int out_fd, size_t len);

    /**
     * @brief 发送 HTTP 响应（自动选择最佳传输方式）
     * @param conn HTTP 连接对象
     * @return true 成功，false 失败
     */
    static bool send_response(http_conn *conn);

private:
    // 管道文件描述符（用于 splice）
    static int s_pipefd[2];
    static bool s_pipe_initialized;

    /**
     * @brief 初始化管道（用于 splice）
     * @return true 成功，false 失败
     */
    static bool init_pipe();

    /**
     * @brief 清理管道
     */
    static void cleanup_pipe();
};

#endif // ZERO_COPY_H
