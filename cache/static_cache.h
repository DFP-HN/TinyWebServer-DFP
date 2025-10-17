#ifndef STATIC_CACHE_H
#define STATIC_CACHE_H

#include <string>
#include <unordered_map>
#include <list>
#include <memory>
#include <sys/stat.h>
#include "../lock/locker.h"

// 静态文件缓存项
struct CacheEntry {
    std::unique_ptr<char[]> data;     // 文件内容
    size_t size;                       // 文件大小
    time_t last_modified;              // 文件最后修改时间
    std::string etag;                  // ETag标识
    time_t access_time;                // 最后访问时间

    CacheEntry() : size(0), last_modified(0), access_time(0) {}
};

// LRU缓存管理器（线程安全）
class StaticCache {
public:
    StaticCache(size_t max_memory_mb = 256);  // 默认256MB缓存
    ~StaticCache();

    // 获取缓存内容（返回nullptr表示未命中）
    // 如果命中，会更新LRU顺序
    std::shared_ptr<CacheEntry> get(const std::string& file_path);

    // 将文件加入缓存
    bool put(const std::string& file_path,
             const char* data,
             size_t size,
             time_t last_modified);

    // 检查文件是否在缓存中且未过期
    bool has_valid_cache(const std::string& file_path, time_t file_mtime);

    // 使缓存项失效
    void invalidate(const std::string& file_path);

    // 清空缓存
    void clear();

    // 获取统计信息
    struct Stats {
        size_t hit_count;
        size_t miss_count;
        size_t eviction_count;
        size_t current_memory;
        size_t max_memory;
        double hit_rate() const {
            size_t total = hit_count + miss_count;
            return total > 0 ? (double)hit_count / total * 100.0 : 0.0;
        }
    };
    Stats get_stats() const;

private:
    // LRU双向链表节点（文件路径）
    typedef std::list<std::string> LRUList;
    typedef LRUList::iterator LRUIterator;

    // 哈希表：文件路径 -> {缓存项, LRU位置}
    struct CacheNode {
        std::shared_ptr<CacheEntry> entry;
        LRUIterator lru_iter;
    };
    std::unordered_map<std::string, CacheNode> m_cache_map;

    LRUList m_lru_list;                // LRU链表（头部=最近使用）
    size_t m_max_memory;                // 最大内存限制（字节）
    size_t m_current_memory;            // 当前使用内存

    // 统计信息
    mutable locker m_lock;              // 互斥锁
    size_t m_hit_count;
    size_t m_miss_count;
    size_t m_eviction_count;

    // LRU淘汰策略
    void evict_lru();

    // 生成ETag（基于文件修改时间和大小）
    std::string generate_etag(const std::string& path,
                              time_t mtime,
                              size_t size);
};

#endif
