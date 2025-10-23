#include "file_db_manager.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

// 自动识别文件类型
std::string FileDBManager::detect_file_type(const char* extension) {
    if (!extension || *extension == '\0') {
        return "other";
    }

    std::string ext = extension;
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // PDF文档
    if (ext == ".pdf") {
        return "pdf";
    }

    // 视频文件
    if (ext == ".mp4" || ext == ".avi" || ext == ".mov" || ext == ".mkv" ||
        ext == ".wmv" || ext == ".flv" || ext == ".webm") {
        return "video";
    }

    // 图片文件
    if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".gif" ||
        ext == ".bmp" || ext == ".svg" || ext == ".webp") {
        return "image";
    }

    // 文档文件
    if (ext == ".doc" || ext == ".docx" || ext == ".xls" || ext == ".xlsx" ||
        ext == ".ppt" || ext == ".pptx" || ext == ".txt" || ext == ".odt") {
        return "document";
    }

    // 压缩文件
    if (ext == ".zip" || ext == ".rar" || ext == ".tar" || ext == ".gz" ||
        ext == ".7z" || ext == ".bz2") {
        return "archive";
    }

    // 音频文件
    if (ext == ".mp3" || ext == ".wav" || ext == ".flac" || ext == ".aac" ||
        ext == ".ogg" || ext == ".m4a") {
        return "audio";
    }

    return "other";
}

// SQL注入防护：转义字符串
std::string FileDBManager::escape_string(MYSQL* mysql, const std::string& str) {
    if (str.empty()) {
        return "";
    }

    char* escaped = new char[str.length() * 2 + 1];
    mysql_real_escape_string(mysql, escaped, str.c_str(), str.length());
    std::string result(escaped);
    delete[] escaped;

    return result;
}

// 从结果集解析文件记录
FileRecord FileDBManager::parse_file_record(MYSQL_ROW row) {
    FileRecord record;

    record.id = strtoull(row[0], nullptr, 10);
    record.filename = row[1] ? row[1] : "";
    record.file_path = row[2] ? row[2] : "";
    record.file_size = strtoull(row[3], nullptr, 10);
    record.file_hash = row[4] ? row[4] : "";
    record.file_type = row[5] ? row[5] : "";
    record.extension = row[6] ? row[6] : "";
    record.upload_time = row[7] ? strtol(row[7], nullptr, 10) : 0;

    return record;
}

// 插入文件记录
bool FileDBManager::insert_file(
    MYSQL* mysql,
    const char* filename,
    const char* file_path,
    uint64_t file_size,
    const char* file_hash,
    const char* extension
) {
    if (!mysql || !filename || !file_path) {
        LOG_ERROR("Invalid parameters for insert_file");
        return false;
    }

    // 转义字符串防止SQL注入
    std::string escaped_filename = escape_string(mysql, filename);
    std::string escaped_path = escape_string(mysql, file_path);
    std::string escaped_hash = file_hash ? escape_string(mysql, file_hash) : "";
    std::string escaped_ext = extension ? escape_string(mysql, extension) : "";

    // 自动识别文件类型
    std::string file_type = detect_file_type(extension);

    // 构建SQL语句（支持文件覆盖：如果文件路径已存在，则更新记录）
    char sql[4096];
    snprintf(sql, sizeof(sql),
        "INSERT INTO files (filename, file_path, file_size, file_hash, "
        "file_type, extension, upload_time, status) "
        "VALUES ('%s', '%s', %lu, %s, '%s', '%s', NOW(), 1) "
        "ON DUPLICATE KEY UPDATE "
        "filename = '%s', "
        "file_size = %lu, "
        "file_hash = %s, "
        "file_type = '%s', "
        "extension = '%s', "
        "upload_time = NOW(), "
        "status = 1",
        escaped_filename.c_str(),
        escaped_path.c_str(),
        file_size,
        escaped_hash.empty() ? "NULL" : ("'" + escaped_hash + "'").c_str(),
        file_type.c_str(),
        escaped_ext.c_str(),
        // ON DUPLICATE KEY UPDATE 部分
        escaped_filename.c_str(),
        file_size,
        escaped_hash.empty() ? "NULL" : ("'" + escaped_hash + "'").c_str(),
        file_type.c_str(),
        escaped_ext.c_str()
    );

    LOG_DEBUG("Executing SQL: %s", sql);

    // 执行SQL
    if (mysql_query(mysql, sql)) {
        LOG_ERROR("Failed to insert/update file record: %s", mysql_error(mysql));
        return false;
    }

    // 检查是插入还是更新
    if (mysql_affected_rows(mysql) == 1) {
        LOG_INFO("File record inserted: %s (size=%lu, type=%s)",
                 filename, file_size, file_type.c_str());
    } else {
        LOG_INFO("File record updated (overwrite): %s (size=%lu, type=%s)",
                 filename, file_size, file_type.c_str());
    }
    return true;
}

// 删除文件记录（软删除）
bool FileDBManager::delete_file_by_path(MYSQL* mysql, const char* file_path) {
    if (!mysql || !file_path) {
        LOG_ERROR("Invalid parameters for delete_file_by_path");
        return false;
    }

    // 转义字符串
    std::string escaped_path = escape_string(mysql, file_path);

    // 构建SQL语句（软删除：设置status=0）
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "UPDATE files SET status = 0, modified_time = NOW() "
        "WHERE file_path = '%s' AND status = 1",
        escaped_path.c_str()
    );

    LOG_DEBUG("Executing SQL: %s", sql);

    // 执行SQL
    if (mysql_query(mysql, sql)) {
        LOG_ERROR("Failed to delete file record: %s", mysql_error(mysql));
        return false;
    }

    // 检查是否有记录被更新
    my_ulonglong affected_rows = mysql_affected_rows(mysql);
    if (affected_rows == 0) {
        LOG_WARN("No file record found for deletion: %s", file_path);
        return false;
    }

    LOG_INFO("File record soft-deleted: %s", file_path);
    return true;
}

// 查询所有文件
std::vector<FileRecord> FileDBManager::query_all_files(
    MYSQL* mysql,
    const char* order_by,
    int limit
) {
    std::vector<FileRecord> files;

    if (!mysql) {
        LOG_ERROR("Invalid mysql connection");
        return files;
    }

    // 构建SQL语句
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT id, filename, file_path, file_size, file_hash, "
        "file_type, extension, UNIX_TIMESTAMP(upload_time) as upload_time "
        "FROM files WHERE status = 1 ORDER BY %s LIMIT %d",
        order_by, limit
    );

    LOG_DEBUG("Executing SQL: %s", sql);

    // 执行查询
    if (mysql_query(mysql, sql)) {
        LOG_ERROR("Failed to query files: %s", mysql_error(mysql));
        return files;
    }

    // 获取结果集
    MYSQL_RES* result = mysql_store_result(mysql);
    if (!result) {
        LOG_ERROR("Failed to store result: %s", mysql_error(mysql));
        return files;
    }

    // 解析结果
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result))) {
        FileRecord record = parse_file_record(row);
        files.push_back(record);
    }

    mysql_free_result(result);

    LOG_INFO("Queried %zu files from database", files.size());
    return files;
}

// 搜索文件
std::vector<FileRecord> FileDBManager::search_files(
    MYSQL* mysql,
    const SearchCriteria& criteria,
    int limit
) {
    std::vector<FileRecord> files;

    if (!mysql) {
        LOG_ERROR("Invalid mysql connection");
        return files;
    }

    // 构建SQL语句（动态添加WHERE条件）
    std::string sql =
        "SELECT id, filename, file_path, file_size, file_hash, "
        "file_type, extension, UNIX_TIMESTAMP(upload_time) as upload_time "
        "FROM files WHERE status = 1";

    // 添加关键字搜索（支持正则表达式模式）
    if (!criteria.keyword.empty()) {
        std::string escaped_keyword = escape_string(mysql, criteria.keyword);
        if (criteria.regex_mode) {
            // 正则表达式搜索（MySQL REGEXP）
            sql += " AND filename REGEXP '" + escaped_keyword + "'";
            LOG_INFO("Search using REGEXP: %s", escaped_keyword.c_str());
        } else {
            // 普通模糊搜索（LIKE）
            sql += " AND filename LIKE '%" + escaped_keyword + "%'";
        }
    }

    // 添加文件类型过滤（新增）
    if (!criteria.file_type.empty()) {
        std::string escaped_type = escape_string(mysql, criteria.file_type);
        sql += " AND file_type = '" + escaped_type + "'";
    }

    // 添加扩展名过滤
    if (!criteria.extension.empty()) {
        std::string escaped_ext = escape_string(mysql, criteria.extension);
        sql += " AND extension = '" + escaped_ext + "'";
    }

    // 添加大小范围过滤
    if (criteria.size_min > 0) {
        sql += " AND file_size >= " + std::to_string(criteria.size_min);
    }
    if (criteria.size_max < UINT64_MAX) {
        sql += " AND file_size <= " + std::to_string(criteria.size_max);
    }

    // 添加时间范围过滤
    if (criteria.date_from > 0) {
        sql += " AND UNIX_TIMESTAMP(upload_time) >= " + std::to_string(criteria.date_from);
    }
    if (criteria.date_to > 0) {
        sql += " AND UNIX_TIMESTAMP(upload_time) <= " + std::to_string(criteria.date_to);
    }

    // 动态排序（新增）
    std::string sort_field = criteria.sort_by.empty() ? "upload_time" : criteria.sort_by;
    std::string sort_direction = criteria.sort_order.empty() ? "DESC" : criteria.sort_order;

    // 白名单验证（防止SQL注入）
    bool valid_field = (sort_field == "filename" || sort_field == "file_size" ||
                       sort_field == "upload_time" || sort_field == "file_type");
    bool valid_order = (sort_direction == "ASC" || sort_direction == "DESC");

    if (valid_field && valid_order) {
        sql += " ORDER BY " + sort_field + " " + sort_direction;
    } else {
        // 默认排序
        sql += " ORDER BY upload_time DESC";
        LOG_WARN("Invalid sort parameters, using default: upload_time DESC");
    }

    // 结果数量限制（新增可配置）
    int result_limit = criteria.limit;
    if (result_limit <= 0 || result_limit > 1000) {
        result_limit = (limit > 0 && limit <= 1000) ? limit : 100;
    }
    sql += " LIMIT " + std::to_string(result_limit);

    LOG_DEBUG("Executing search SQL: %s", sql.c_str());

    // 执行查询
    if (mysql_query(mysql, sql.c_str())) {
        LOG_ERROR("Failed to search files: %s", mysql_error(mysql));
        return files;
    }

    // 获取结果集
    MYSQL_RES* result = mysql_store_result(mysql);
    if (!result) {
        LOG_ERROR("Failed to store search result: %s", mysql_error(mysql));
        return files;
    }

    // 解析结果
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result))) {
        FileRecord record = parse_file_record(row);
        files.push_back(record);
    }

    mysql_free_result(result);

    LOG_INFO("Search found %zu files (keyword='%s')",
             files.size(), criteria.keyword.c_str());
    return files;
}

// 检查文件是否存在（通过哈希）
bool FileDBManager::file_exists_by_hash(MYSQL* mysql, const char* file_hash) {
    if (!mysql || !file_hash) {
        return false;
    }

    std::string escaped_hash = escape_string(mysql, file_hash);

    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT COUNT(*) FROM files WHERE file_hash = '%s' AND status = 1",
        escaped_hash.c_str()
    );

    if (mysql_query(mysql, sql)) {
        LOG_ERROR("Failed to check file existence: %s", mysql_error(mysql));
        return false;
    }

    MYSQL_RES* result = mysql_store_result(mysql);
    if (!result) {
        return false;
    }

    MYSQL_ROW row = mysql_fetch_row(result);
    int count = row ? atoi(row[0]) : 0;
    mysql_free_result(result);

    return count > 0;
}

// 检查文件是否存在（通过路径）
bool FileDBManager::file_exists_by_path(MYSQL* mysql, const char* file_path) {
    if (!mysql || !file_path) {
        return false;
    }

    std::string escaped_path = escape_string(mysql, file_path);

    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT COUNT(*) FROM files WHERE file_path = '%s' AND status = 1",
        escaped_path.c_str()
    );

    if (mysql_query(mysql, sql)) {
        LOG_ERROR("Failed to check file existence: %s", mysql_error(mysql));
        return false;
    }

    MYSQL_RES* result = mysql_store_result(mysql);
    if (!result) {
        return false;
    }

    MYSQL_ROW row = mysql_fetch_row(result);
    int count = row ? atoi(row[0]) : 0;
    mysql_free_result(result);

    return count > 0;
}
