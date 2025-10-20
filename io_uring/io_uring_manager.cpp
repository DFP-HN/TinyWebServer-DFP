#include "io_uring_manager.h"
#include "../log/log.h"
#include <cstring>
#include <errno.h>
#include <unistd.h>

IoUringManager::IoUringManager(unsigned entries, unsigned flags)
    : m_entries(entries)
    , m_flags(flags)
    , m_initialized(false)
    , m_buffers_registered(false)
    , m_files_registered(false)
{
    memset(&m_ring, 0, sizeof(m_ring));
}

IoUringManager::~IoUringManager() {
    if (m_initialized) {
        // 注销资源
        if (m_buffers_registered) {
            unregister_buffers();
        }
        if (m_files_registered) {
            unregister_files();
        }

        // 退出 io_uring
        io_uring_queue_exit(&m_ring);
        m_initialized = false;
        LOG_INFO("IoUringManager: io_uring queue exited");
    }
}

bool IoUringManager::init() {
    if (m_initialized) {
        LOG_WARN("IoUringManager: already initialized");
        return true;
    }

    // 初始化 io_uring
    int ret = io_uring_queue_init(m_entries, &m_ring, m_flags);
    if (ret < 0) {
        LOG_ERROR("IoUringManager: io_uring_queue_init failed: %s (entries=%u, flags=%u)",
                  strerror(-ret), m_entries, m_flags);
        return false;
    }

    m_initialized = true;
    LOG_INFO("IoUringManager: initialized with entries=%u, flags=%u", m_entries, m_flags);
    return true;
}

struct io_uring_sqe* IoUringManager::get_sqe() {
    if (!m_initialized) {
        LOG_ERROR("IoUringManager: not initialized");
        return nullptr;
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&m_ring);
    if (!sqe) {
        LOG_WARN("IoUringManager: SQ is full, cannot get SQE");
    }
    return sqe;
}

bool IoUringManager::submit_read(int fd, void *buf, size_t len, off_t offset, uint64_t user_data) {
    struct io_uring_sqe *sqe = get_sqe();
    if (!sqe) {
        return false;
    }

    if (offset >= 0) {
        // 带偏移量的读（pread）
        io_uring_prep_read(sqe, fd, buf, len, offset);
    } else {
        // 无偏移量的读（read）
        io_uring_prep_read(sqe, fd, buf, len, 0);
    }

    io_uring_sqe_set_data(sqe, (void*)user_data);
    LOG_DEBUG("IoUringManager: submit_read fd=%d, len=%zu, offset=%ld, user_data=%lu",
              fd, len, offset, user_data);
    return true;
}

bool IoUringManager::submit_write(int fd, const void *buf, size_t len, off_t offset, uint64_t user_data) {
    struct io_uring_sqe *sqe = get_sqe();
    if (!sqe) {
        return false;
    }

    if (offset >= 0) {
        // 带偏移量的写（pwrite）
        io_uring_prep_write(sqe, fd, buf, len, offset);
    } else {
        // 无偏移量的写（write）
        io_uring_prep_write(sqe, fd, buf, len, 0);
    }

    io_uring_sqe_set_data(sqe, (void*)user_data);
    LOG_DEBUG("IoUringManager: submit_write fd=%d, len=%zu, offset=%ld, user_data=%lu",
              fd, len, offset, user_data);
    return true;
}

bool IoUringManager::submit_sendfile(int out_fd, int in_fd, off_t offset, size_t len, uint64_t user_data) {
    struct io_uring_sqe *sqe = get_sqe();
    if (!sqe) {
        return false;
    }

    // io_uring 的 splice 操作（用于文件 -> socket 的零拷贝）
    // 注意：io_uring 版本 >= 5.7 支持
    io_uring_prep_splice(sqe, in_fd, offset, out_fd, -1, len, 0);

    io_uring_sqe_set_data(sqe, (void*)user_data);
    LOG_DEBUG("IoUringManager: submit_sendfile out_fd=%d, in_fd=%d, offset=%ld, len=%zu, user_data=%lu",
              out_fd, in_fd, offset, len, user_data);
    return true;
}

bool IoUringManager::submit_accept(int listen_fd, struct sockaddr *addr, socklen_t *addrlen, uint64_t user_data) {
    struct io_uring_sqe *sqe = get_sqe();
    if (!sqe) {
        return false;
    }

    io_uring_prep_accept(sqe, listen_fd, addr, addrlen, 0);
    io_uring_sqe_set_data(sqe, (void*)user_data);
    LOG_DEBUG("IoUringManager: submit_accept listen_fd=%d, user_data=%lu", listen_fd, user_data);
    return true;
}

bool IoUringManager::submit_close(int fd, uint64_t user_data) {
    struct io_uring_sqe *sqe = get_sqe();
    if (!sqe) {
        return false;
    }

    io_uring_prep_close(sqe, fd);
    io_uring_sqe_set_data(sqe, (void*)user_data);
    LOG_DEBUG("IoUringManager: submit_close fd=%d, user_data=%lu", fd, user_data);
    return true;
}

int IoUringManager::submit_all() {
    if (!m_initialized) {
        LOG_ERROR("IoUringManager: not initialized");
        return -1;
    }

    int ret = io_uring_submit(&m_ring);
    if (ret < 0) {
        LOG_ERROR("IoUringManager: io_uring_submit failed: %s", strerror(-ret));
        return -1;
    }

    LOG_DEBUG("IoUringManager: submitted %d SQEs", ret);
    return ret;
}

int IoUringManager::wait_completions(unsigned wait_nr) {
    if (!m_initialized) {
        LOG_ERROR("IoUringManager: not initialized");
        return -1;
    }

    struct io_uring_cqe *cqe;
    int ret = io_uring_wait_cqe_nr(&m_ring, &cqe, wait_nr);
    if (ret < 0) {
        if (ret == -EINTR) {
            // 被信号中断，正常情况
            LOG_DEBUG("IoUringManager: wait interrupted by signal");
            return 0;
        }
        LOG_ERROR("IoUringManager: io_uring_wait_cqe_nr failed: %s", strerror(-ret));
        return -1;
    }

    // 返回可用的 CQE 数量
    return get_cq_ready();
}

int IoUringManager::process_completions(std::function<void(struct io_uring_cqe*)> handler) {
    if (!m_initialized) {
        LOG_ERROR("IoUringManager: not initialized");
        return -1;
    }

    int count = 0;
    struct io_uring_cqe *cqe;
    unsigned head;

    // 批量处理所有可用的 CQE
    io_uring_for_each_cqe(&m_ring, head, cqe) {
        handler(cqe);
        count++;
    }

    // 标记所有处理过的 CQE
    io_uring_cq_advance(&m_ring, count);

    if (count > 0) {
        LOG_DEBUG("IoUringManager: processed %d completions", count);
    }

    return count;
}

bool IoUringManager::peek_completion(struct io_uring_cqe **cqe_ptr) {
    if (!m_initialized) {
        LOG_ERROR("IoUringManager: not initialized");
        return false;
    }

    int ret = io_uring_peek_cqe(&m_ring, cqe_ptr);
    if (ret < 0) {
        if (ret != -EAGAIN) {
            LOG_ERROR("IoUringManager: io_uring_peek_cqe failed: %s", strerror(-ret));
        }
        return false;
    }

    return (*cqe_ptr != nullptr);
}

void IoUringManager::mark_seen(struct io_uring_cqe *cqe) {
    if (!m_initialized || !cqe) {
        return;
    }

    io_uring_cqe_seen(&m_ring, cqe);
}

bool IoUringManager::register_buffers(struct iovec *iovecs, unsigned nr_iovecs) {
    if (!m_initialized) {
        LOG_ERROR("IoUringManager: not initialized");
        return false;
    }

    if (m_buffers_registered) {
        LOG_WARN("IoUringManager: buffers already registered, unregistering first");
        unregister_buffers();
    }

    int ret = io_uring_register_buffers(&m_ring, iovecs, nr_iovecs);
    if (ret < 0) {
        LOG_ERROR("IoUringManager: io_uring_register_buffers failed: %s", strerror(-ret));
        return false;
    }

    m_buffers_registered = true;
    LOG_INFO("IoUringManager: registered %u buffers", nr_iovecs);
    return true;
}

bool IoUringManager::unregister_buffers() {
    if (!m_initialized || !m_buffers_registered) {
        return true;
    }

    int ret = io_uring_unregister_buffers(&m_ring);
    if (ret < 0) {
        LOG_ERROR("IoUringManager: io_uring_unregister_buffers failed: %s", strerror(-ret));
        return false;
    }

    m_buffers_registered = false;
    LOG_INFO("IoUringManager: unregistered buffers");
    return true;
}

bool IoUringManager::register_files(int *fds, unsigned nr_fds) {
    if (!m_initialized) {
        LOG_ERROR("IoUringManager: not initialized");
        return false;
    }

    if (m_files_registered) {
        LOG_WARN("IoUringManager: files already registered, unregistering first");
        unregister_files();
    }

    int ret = io_uring_register_files(&m_ring, fds, nr_fds);
    if (ret < 0) {
        LOG_ERROR("IoUringManager: io_uring_register_files failed: %s", strerror(-ret));
        return false;
    }

    m_files_registered = true;
    LOG_INFO("IoUringManager: registered %u files", nr_fds);
    return true;
}

bool IoUringManager::unregister_files() {
    if (!m_initialized || !m_files_registered) {
        return true;
    }

    int ret = io_uring_unregister_files(&m_ring);
    if (ret < 0) {
        LOG_ERROR("IoUringManager: io_uring_unregister_files failed: %s", strerror(-ret));
        return false;
    }

    m_files_registered = false;
    LOG_INFO("IoUringManager: unregistered files");
    return true;
}

unsigned IoUringManager::get_sq_pending() const {
    if (!m_initialized) {
        return 0;
    }
    return io_uring_sq_ready(&m_ring);
}

unsigned IoUringManager::get_cq_ready() const {
    if (!m_initialized) {
        return 0;
    }
    return io_uring_cq_ready(&m_ring);
}
