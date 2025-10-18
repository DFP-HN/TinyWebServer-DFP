#ifndef CHASE_LEV_DEQUE_H
#define CHASE_LEV_DEQUE_H

#include <atomic>
#include <vector>
#include <optional>
#include <cstdint>

/**
 * Chase-Lev 无锁工作窃取双端队列
 *
 * 基于 "Dynamic Circular Work-Stealing Deque" (Chase & Lev, 2005) 论文实现
 *
 * 特性:
 * - 所有者线程在 bottom 端操作（push/pop）
 * - 窃取者线程在 top 端操作（steal）
 * - 无锁并发，使用原子操作和内存序
 * - 动态扩容的循环数组
 */
template <typename T>
class ChaseLevDeque
{
private:
    // 内部循环数组结构
    struct CircularArray
    {
        int64_t log_size;                    // log2(capacity)
        std::vector<T> data;                 // 存储数据

        explicit CircularArray(int64_t log_sz)
            : log_size(log_sz), data(1LL << log_sz)
        {
        }

        int64_t capacity() const
        {
            return 1LL << log_size;
        }

        T get(int64_t index) const
        {
            return data[index % capacity()];
        }

        void put(int64_t index, T item)
        {
            data[index % capacity()] = item;
        }
    };

    std::atomic<int64_t> top;                // 窃取端索引（只有窃取者修改）
    std::atomic<int64_t> bottom;             // 所有者端索引（只有所有者修改）
    std::atomic<CircularArray*> array;       // 当前使用的数组

    // 扩容：当数组满时，创建一个两倍大小的新数组
    void grow()
    {
        CircularArray* old_arr = array.load(std::memory_order_relaxed);
        CircularArray* new_arr = new CircularArray(old_arr->log_size + 1);

        int64_t t = top.load(std::memory_order_relaxed);
        int64_t b = bottom.load(std::memory_order_relaxed);

        // 拷贝所有元素到新数组
        for (int64_t i = t; i < b; ++i)
        {
            new_arr->put(i, old_arr->get(i));
        }

        // 发布新数组
        array.store(new_arr, std::memory_order_release);

        // 注意: 旧数组不能立即删除，可能有窃取者还在使用
        // 在实际生产环境中需要使用延迟回收（如 hazard pointer 或 epoch-based reclamation）
        // 这里为简化暂时泄漏内存（实际使用中数组很少扩容，影响不大）
    }

public:
    ChaseLevDeque()
        : top(0), bottom(0), array(new CircularArray(10))  // 初始大小 2^10 = 1024
    {
    }

    ~ChaseLevDeque()
    {
        // 清理当前数组
        CircularArray* arr = array.load(std::memory_order_relaxed);
        delete arr;
        // 注意: 泄漏的旧数组无法清理（需要更复杂的内存管理）
    }

    /**
     * push_bottom - 所有者线程在 bottom 端压入元素
     * 只能由所有者线程调用
     */
    void push_bottom(T item)
    {
        int64_t b = bottom.load(std::memory_order_relaxed);
        int64_t t = top.load(std::memory_order_acquire);
        CircularArray* arr = array.load(std::memory_order_relaxed);

        // 检查是否需要扩容
        if (b - t >= arr->capacity())
        {
            grow();
            arr = array.load(std::memory_order_relaxed);
        }

        arr->put(b, item);
        // 确保 put 操作在 bottom 增加之前完成
        std::atomic_thread_fence(std::memory_order_release);
        bottom.store(b + 1, std::memory_order_relaxed);
    }

    /**
     * pop_bottom - 所有者线程从 bottom 端弹出元素
     * 只能由所有者线程调用
     * @return 弹出的元素，如果队列为空返回 std::nullopt
     */
    std::optional<T> pop_bottom()
    {
        int64_t b = bottom.load(std::memory_order_relaxed) - 1;
        CircularArray* arr = array.load(std::memory_order_relaxed);
        bottom.store(b, std::memory_order_relaxed);

        std::atomic_thread_fence(std::memory_order_seq_cst);

        int64_t t = top.load(std::memory_order_relaxed);

        if (t <= b)
        {
            // 队列非空
            T item = arr->get(b);

            if (t == b)
            {
                // 最后一个元素，需要与窃取者竞争
                if (!top.compare_exchange_strong(t, t + 1,
                                                   std::memory_order_seq_cst,
                                                   std::memory_order_relaxed))
                {
                    // CAS 失败，被窃取者抢走了
                    bottom.store(b + 1, std::memory_order_relaxed);
                    return std::nullopt;
                }
                bottom.store(b + 1, std::memory_order_relaxed);
                return item;
            }
            else
            {
                // 还有多个元素，直接返回
                return item;
            }
        }
        else
        {
            // 队列为空
            bottom.store(b + 1, std::memory_order_relaxed);
            return std::nullopt;
        }
    }

    /**
     * steal_top - 窃取者线程从 top 端窃取元素
     * 可以由���何窃取者线程调用
     * @return 窃取的元素，如果队列为空或竞争失败返回 std::nullopt
     */
    std::optional<T> steal_top()
    {
        int64_t t = top.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        int64_t b = bottom.load(std::memory_order_acquire);

        if (t < b)
        {
            // 队列非空
            CircularArray* arr = array.load(std::memory_order_consume);
            T item = arr->get(t);

            // 尝试原子地增加 top
            if (!top.compare_exchange_strong(t, t + 1,
                                               std::memory_order_seq_cst,
                                               std::memory_order_relaxed))
            {
                // CAS 失败，被其他窃取者抢走了
                return std::nullopt;
            }

            return item;
        }
        else
        {
            // 队列为空
            return std::nullopt;
        }
    }

    /**
     * size - 估计队列大小（非精确，仅供参考）
     * 可能被任何线程调用，但结果不保证准确
     */
    int64_t size() const
    {
        int64_t b = bottom.load(std::memory_order_relaxed);
        int64_t t = top.load(std::memory_order_relaxed);
        return b - t;
    }

    /**
     * empty - 检查队列是否为空（非精确）
     */
    bool empty() const
    {
        return size() <= 0;
    }
};

#endif // CHASE_LEV_DEQUE_H
