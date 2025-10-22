#ifndef FILE_DB_MANAGER_H
#define FILE_DB_MANAGER_H

#include <mysql/mysql.h>
#include <string>
#include <vector>
#include <cstring>
#include <ctime>
#include "../log/log.h"

/**
 * @brief 文件记录结构
 */
struct FileRecord {
    uint64_t id;
    std::string filename;
    std::string file_path;
    uint64_t file_size;
    std::string file_hash;
    std::string file_type;
    std::string extension;
    time_t upload_time;
    int status;

    FileRecord() : id(0), file_size(0), upload_time(0), status(1) {}
};

/**
 * @brief 搜索条件结构
 */
struct SearchCriteria {
    std::string keyword;        // 文件名关键字
    std::string extension;      // 扩展名过滤
    uint64_t size_min;          // 最小文件大小
    uint64_t size_max;          // 最大文件大小
    time_t date_from;           // 起始时间
    time_t date_to;             // 结束时间

    SearchCriteria()
        : size_min(0),
          size_max(UINT64_MAX),
          date_from(0),
          date_to(0) {}
};

/**
 * @brief 文件数据库管理器
 *
 * 提供文件元数据的数据库操作接口
 */
class FileDBManager {
public:
    /**
     * @brief 插入文件记录
     */
    static bool insert_file(
        MYSQL* mysql,
        const char* filename,
        const char* file_path,
        uint64_t file_size,
        const char* file_hash,
        const char* extension
    );

    /**
     * @brief 删除文件记录（软删除）
     */
    static bool delete_file_by_path(MYSQL* mysql, const char* file_path);

    /**
     * @brief 查询所有文件
     */
    static std::vector<FileRecord> query_all_files(
        MYSQL* mysql,
        const char* order_by = "upload_time DESC",
        int limit = 1000
    );

    /**
     * @brief 搜索文件
     */
    static std::vector<FileRecord> search_files(
        MYSQL* mysql,
        const SearchCriteria& criteria,
        int limit = 100
    );

    /**
     * @brief 检查文件是否存在（通过哈希）
     */
    static bool file_exists_by_hash(MYSQL* mysql, const char* file_hash);

    /**
     * @brief 检查文件是否存在（通过路径）
     */
    static bool file_exists_by_path(MYSQL* mysql, const char* file_path);

private:
    /**
     * @brief 自动识别文件类型
     */
    static std::string detect_file_type(const char* extension);

    /**
     * @brief SQL注入防护：转义字符串
     */
    static std::string escape_string(MYSQL* mysql, const std::string& str);

    /**
     * @brief 从结果集解析文件记录
     */
    static FileRecord parse_file_record(MYSQL_ROW row);
};

#endif // FILE_DB_MANAGER_H
