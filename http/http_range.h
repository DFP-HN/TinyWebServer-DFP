#ifndef HTTP_RANGE_H
#define HTTP_RANGE_H

#include <cstddef>
#include <cstring>
#include <cstdio>

/**
 * @brief HTTP Range 请求解析器
 *
 * 支持 HTTP/1.1 Range 请求（RFC 7233）
 *
 * 支持的格式：
 * - Range: bytes=0-1023       (从 0 到 1023)
 * - Range: bytes=1024-        (从 1024 到文件末尾)
 * - Range: bytes=-1000        (最后 1000 字节)
 *
 * 使用示例：
 * @code
 * HttpRange range;
 * if (range.parse_range_header("bytes=0-1023", file_size)) {
 *     // range.start = 0
 *     // range.end = 1023
 *     // range.is_range_request = true
 * }
 * @endcode
 */
struct HttpRange {
    size_t start;              ///< 起始字节位置（包含）
    size_t end;                ///< 结束字节位置（包含）
    bool is_range_request;     ///< 是否是 Range 请求

    /**
     * @brief 构造函数 - 默认为非 Range 请求
     */
    HttpRange()
        : start(0), end(0), is_range_request(false) {}

    /**
     * @brief 解析 Range 请求头
     *
     * @param range_header Range 请求头的值（如 "bytes=0-1023"）
     * @param file_size 文件总大小
     * @return true 解析成功，false 解析失败或格式无效
     *
     * 注意：
     * - 如果 end 超过 file_size，会自动调整为 file_size - 1
     * - 如果 start > end，返回 false
     * - 只支持单一范围，不支持多范围（bytes=0-100,200-300）
     */
    bool parse_range_header(const char* range_header, size_t file_size) {
        if (!range_header || file_size == 0) {
            return false;
        }

        // 检查前缀 "bytes="
        if (strncmp(range_header, "bytes=", 6) != 0) {
            return false;
        }

        const char* range_spec = range_header + 6;

        // 检查是否是多范围（不支持）
        if (strchr(range_spec, ',') != nullptr) {
            fprintf(stderr, "[HttpRange] Multi-range not supported: %s\n", range_header);
            return false;
        }

        // 查找 '-' 分隔符
        const char* dash = strchr(range_spec, '-');
        if (!dash) {
            return false;
        }

        is_range_request = true;

        // 解析起始位置
        if (dash == range_spec) {
            // 格式: bytes=-1000 (最后 N 字节)
            long long suffix_length = 0;
            if (sscanf(dash + 1, "%lld", &suffix_length) != 1 || suffix_length <= 0) {
                return false;
            }

            if ((size_t)suffix_length >= file_size) {
                // 请求的字节数 >= 文件大小，返回整个文件
                start = 0;
                end = file_size - 1;
            } else {
                start = file_size - suffix_length;
                end = file_size - 1;
            }
        } else {
            // 格式: bytes=1024- 或 bytes=0-1023
            long long start_val = 0;
            if (sscanf(range_spec, "%lld", &start_val) != 1 || start_val < 0) {
                return false;
            }

            start = (size_t)start_val;

            if (*(dash + 1) == '\0' || *(dash + 1) == '\r' || *(dash + 1) == '\n') {
                // 格式: bytes=1024- (从 start 到文件末尾)
                end = file_size - 1;
            } else {
                // 格式: bytes=0-1023
                long long end_val = 0;
                if (sscanf(dash + 1, "%lld", &end_val) != 1 || end_val < 0) {
                    return false;
                }
                end = (size_t)end_val;
            }
        }

        // 验证范围合法性
        if (start >= file_size) {
            fprintf(stderr, "[HttpRange] Start position %zu >= file size %zu\n", start, file_size);
            return false;  // 416 Range Not Satisfiable
        }

        if (end >= file_size) {
            end = file_size - 1;  // 调整到文件末尾
        }

        if (start > end) {
            fprintf(stderr, "[HttpRange] Invalid range: start %zu > end %zu\n", start, end);
            return false;
        }

        fprintf(stderr, "[HttpRange] Parsed successfully: bytes %zu-%zu/%zu\n", start, end, file_size);
        return true;
    }

    /**
     * @brief 获取 Content-Length（要传输的字节数）
     *
     * @param file_size 文件总大小
     * @return Content-Length 的值
     */
    size_t get_content_length(size_t file_size) const {
        if (!is_range_request) {
            return file_size;
        }
        return end - start + 1;
    }

    /**
     * @brief 生成 Content-Range 响应头
     *
     * @param file_size 文件总大小
     * @param buffer 输出缓冲区
     * @param buffer_size 缓冲区大小
     * @return 生成的字符串长度
     *
     * 格式: Content-Range: bytes 0-1023/2048
     */
    int format_content_range(size_t file_size, char* buffer, size_t buffer_size) const {
        if (!is_range_request) {
            return 0;
        }
        return snprintf(buffer, buffer_size, "Content-Range: bytes %zu-%zu/%zu\r\n",
                        start, end, file_size);
    }
};

#endif // HTTP_RANGE_H
