#ifndef IO_URING_MANAGER_H
#define IO_URING_MANAGER_H

#include <liburing.h>
#include <functional>
#include <unordered_map>
#include <memory>
#include <sys/types.h>

/**
 * @brief io_uring 管理器
 *
 * 封装 Linux io_uring API，提供高性能异步 I/O 操作
 *
 * 特性：
 * - 批量提交 I/O 请求（减少系统调用）
 * - 支持异步读写、sendfile
 * - 支持 registered buffers（固定内存，零拷贝）
 * - 支持 registered files（固定文件描述符，减少查表）
 * - 完成队列批量处理
 *
 * 使用示例：
 * @code
 * IoUringManager mgr(256);  // 队列深度 256
 * mgr.init();
 *
 * // 提交异步读请求
 * mgr.submit_read(fd, buffer, size, user_data);
 *
 * // 批量提交
 * mgr.submit_all();
 *
 * // 处理完成事件
 * mgr.process_completions([](struct io_uring_cqe *cqe) {
 *     // 处理完成事件
 * });
 * @endcode
 */
class IoUringManager {
public:
    /**
     * @brief 构造函数
     * @param entries SQ/CQ 队列深度（必须是 2 的幂，默认 256）
     * @param flags io_uring 初始化标志（默认 0）
     */
    explicit IoUringManager(unsigned entries = 256, unsigned flags = 0);

    /**
     * @brief 析构函数
     */
    ~IoUringManager();

    // 禁止拷贝和赋值
    IoUringManager(const IoUringManager&) = delete;
    IoUringManager& operator=(const IoUringManager&) = delete;

    /**
     * @brief 初始化 io_uring
     * @return true 成功，false 失败
     */
    bool init();

    /**
     * @brief 检查是否已初始化
     */
    bool is_initialized() const { return m_initialized; }

    /**
     * @brief 提交异步读操作
     * @param fd 文件描述符
     * @param buf 缓冲区
     * @param len 读取长度
     * @param offset 文件偏移量（-1 表示使用当前位置）
     * @param user_data 用户数据（完成时返回）
     * @return true 成功添加到 SQ，false 失败
     */
    bool submit_read(int fd, void *buf, size_t len, off_t offset, uint64_t user_data);

    /**
     * @brief 提交异步写操作
     * @param fd 文件描述符
     * @param buf 缓冲区
     * @param len 写入长度
     * @param offset 文件偏移量（-1 表示使用当前位置）
     * @param user_data 用户数据（完成时返回）
     * @return true 成功添加到 SQ，false 失败
     */
    bool submit_write(int fd, const void *buf, size_t len, off_t offset, uint64_t user_data);

    /**
     * @brief 提交异步 sendfile 操作（零拷贝）
     * @param out_fd 输出文件描述符（通常是 socket）
     * @param in_fd 输入文件描述符
     * @param offset 文件偏移量
     * @param len 发送长度
     * @param user_data 用户数据（完成时返回）
     * @return true 成功添加到 SQ，false 失败
     */
    bool submit_sendfile(int out_fd, int in_fd, off_t offset, size_t len, uint64_t user_data);

    /**
     * @brief 提交异步 accept 操作
     * @param listen_fd 监听 socket 文件描述符
     * @param addr 客户端地址结构（输出参数）
     * @param addrlen 地址长度（输出参数）
     * @param user_data 用户数据（完成时返回）
     * @return true 成功添加到 SQ，false 失败
     */
    bool submit_accept(int listen_fd, struct sockaddr *addr, socklen_t *addrlen, uint64_t user_data);

    /**
     * @brief 提交异步 close 操作
     * @param fd 文件描述符
     * @param user_data 用户数据（完成时返回）
     * @return true 成功添加到 SQ，false 失败
     */
    bool submit_close(int fd, uint64_t user_data);

    /**
     * @brief 批量提交所有待处理的 SQ 条目
     * @return 提交的条目数，-1 表示错误
     */
    int submit_all();

    /**
     * @brief 等待完成事件
     * @param wait_nr 至少等待多少个完成事件（默认 1）
     * @return 可用的完成事件数，-1 表示错误
     */
    int wait_completions(unsigned wait_nr = 1);

    /**
     * @brief 处理完成队列中的所有事件
     * @param handler 完成事件处理函数
     * @return 处理的事件数
     */
    int process_completions(std::function<void(struct io_uring_cqe*)> handler);

    /**
     * @brief 获取单个完成事件（不阻塞）
     * @param cqe_ptr 输出参数，指向完成事件
     * @return true 成功获取，false 没有事件
     */
    bool peek_completion(struct io_uring_cqe **cqe_ptr);

    /**
     * @brief 标记完成事件已处理
     * @param cqe 完成事件指针
     */
    void mark_seen(struct io_uring_cqe *cqe);

    /**
     * @brief 注册固定缓冲区（用于零拷贝 I/O）
     * @param iovecs iovec 数组
     * @param nr_iovecs iovec 数量
     * @return true 成功，false 失败
     */
    bool register_buffers(struct iovec *iovecs, unsigned nr_iovecs);

    /**
     * @brief 注销固定缓冲区
     * @return true 成功，false 失败
     */
    bool unregister_buffers();

    /**
     * @brief 注册固定文件描述符（减少查表开销）
     * @param fds 文件描述符数组
     * @param nr_fds 文件描述符数量
     * @return true 成功，false 失败
     */
    bool register_files(int *fds, unsigned nr_fds);

    /**
     * @brief 注销固定文件描述符
     * @return true 成功，false 失败
     */
    bool unregister_files();

    /**
     * @brief 获取队列深度
     */
    unsigned get_queue_depth() const { return m_entries; }

    /**
     * @brief 获取待提交的 SQ 条目数
     */
    unsigned get_sq_pending() const;

    /**
     * @brief 获取可用的 CQ 条目数
     */
    unsigned get_cq_ready() const;

private:
    struct io_uring m_ring;           ///< io_uring 实例
    unsigned m_entries;                ///< SQ/CQ 队列深度
    unsigned m_flags;                  ///< 初始化标志
    bool m_initialized;                ///< 是否已初始化

    // 高级特性标志
    bool m_buffers_registered;         ///< 是否注册了固定缓冲区
    bool m_files_registered;           ///< 是否注册了固定文件描述符

public:
    /**
     * @brief 获取 SQE（提交队列条目）
     * @return SQE 指针，NULL 表示队列满
     *
     * 注意：此方法暴露为public以支持协程-线程池桥接中的自定义操作
     */
    struct io_uring_sqe* get_sqe();
};

#endif // IO_URING_MANAGER_H
