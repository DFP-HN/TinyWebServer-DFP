#include "static_cache.h"
#include <sstream>
#include <iomanip>
#include <cstring>
#include <ctime>

StaticCache::StaticCache(size_t max_memory_mb)
    : m_max_memory(max_memory_mb * 1024 * 1024),
      m_current_memory(0),
      m_hit_count(0),
      m_miss_count(0),
      m_eviction_count(0)
{
}

StaticCache::~StaticCache()
{
    clear();
}

std::shared_ptr<CacheEntry> StaticCache::get(const std::string& file_path)
{
    m_lock.lock();

    auto it = m_cache_map.find(file_path);
    if (it == m_cache_map.end())
    {
        m_miss_count++;
        m_lock.unlock();
        return nullptr;
    }

    // 命中：更新LRU顺序（移到链表头部）
    m_lru_list.erase(it->second.lru_iter);
    m_lru_list.push_front(file_path);
    it->second.lru_iter = m_lru_list.begin();

    // 更新访问时间
    it->second.entry->access_time = time(NULL);

    m_hit_count++;
    std::shared_ptr<CacheEntry> result = it->second.entry;
    m_lock.unlock();

    return result;
}

bool StaticCache::put(const std::string& file_path,
                      const char* data,
                      size_t size,
                      time_t last_modified)
{
    if (size > m_max_memory)
    {
        // 文件太大，���缓存
        return false;
    }

    m_lock.lock();

    // 检查是否已存在
    auto it = m_cache_map.find(file_path);
    if (it != m_cache_map.end())
    {
        // 已存在，先移除旧的
        m_current_memory -= it->second.entry->size;
        m_lru_list.erase(it->second.lru_iter);
        m_cache_map.erase(it);
    }

    // LRU淘汰：释放空间
    while (m_current_memory + size > m_max_memory && !m_lru_list.empty())
    {
        evict_lru();
    }

    // 创建新缓存项
    std::shared_ptr<CacheEntry> entry = std::make_shared<CacheEntry>();
    entry->data = std::make_unique<char[]>(size);
    memcpy(entry->data.get(), data, size);
    entry->size = size;
    entry->last_modified = last_modified;
    entry->etag = generate_etag(file_path, last_modified, size);
    entry->access_time = time(NULL);

    // 加入LRU链表头部
    m_lru_list.push_front(file_path);

    // 加入哈希表
    CacheNode node;
    node.entry = entry;
    node.lru_iter = m_lru_list.begin();
    m_cache_map[file_path] = node;

    m_current_memory += size;

    m_lock.unlock();
    return true;
}

bool StaticCache::has_valid_cache(const std::string& file_path, time_t file_mtime)
{
    m_lock.lock();

    auto it = m_cache_map.find(file_path);
    if (it == m_cache_map.end())
    {
        m_lock.unlock();
        return false;
    }

    // 检查文件是否被修改
    bool valid = (it->second.entry->last_modified == file_mtime);
    m_lock.unlock();

    return valid;
}

void StaticCache::invalidate(const std::string& file_path)
{
    m_lock.lock();

    auto it = m_cache_map.find(file_path);
    if (it != m_cache_map.end())
    {
        m_current_memory -= it->second.entry->size;
        m_lru_list.erase(it->second.lru_iter);
        m_cache_map.erase(it);
    }

    m_lock.unlock();
}

void StaticCache::clear()
{
    m_lock.lock();

    m_cache_map.clear();
    m_lru_list.clear();
    m_current_memory = 0;

    m_lock.unlock();
}

StaticCache::Stats StaticCache::get_stats() const
{
    m_lock.lock();

    Stats stats;
    stats.hit_count = m_hit_count;
    stats.miss_count = m_miss_count;
    stats.eviction_count = m_eviction_count;
    stats.current_memory = m_current_memory;
    stats.max_memory = m_max_memory;

    m_lock.unlock();

    return stats;
}

void StaticCache::evict_lru()
{
    // 移除LRU链表尾部（最久未使用）
    if (m_lru_list.empty())
        return;

    std::string victim = m_lru_list.back();
    m_lru_list.pop_back();

    auto it = m_cache_map.find(victim);
    if (it != m_cache_map.end())
    {
        m_current_memory -= it->second.entry->size;
        m_cache_map.erase(it);
        m_eviction_count++;
    }
}

std::string StaticCache::generate_etag(const std::string& path,
                                       time_t mtime,
                                       size_t size)
{
    // ETag格式: "mtime-size" (简单但有效)
    std::ostringstream oss;
    oss << "\"" << std::hex << mtime << "-" << size << "\"";
    return oss.str();
}
