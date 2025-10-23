#ifndef TESTS_UTILS_TEST_HELPER_H
#define TESTS_UTILS_TEST_HELPER_H

#include <string>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <vector>
#include <sstream>

/**
 * @brief 简单的HTTP测试客户端
 */
class TestHttpClient {
public:
    TestHttpClient(const std::string& host, int port)
        : host_(host), port_(port), sockfd_(-1) {
    }

    ~TestHttpClient() {
        close_connection();
    }

    /**
     * @brief 连接到服务器
     */
    bool connect() {
        sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd_ < 0) {
            return false;
        }

        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port_);

        if (inet_pton(AF_INET, host_.c_str(), &server_addr.sin_addr) <= 0) {
            close(sockfd_);
            sockfd_ = -1;
            return false;
        }

        if (::connect(sockfd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            close(sockfd_);
            sockfd_ = -1;
            return false;
        }

        return true;
    }

    /**
     * @brief 发送GET请求
     */
    std::string send_get(const std::string& path) {
        if (sockfd_ < 0 && !connect()) {
            return "";
        }

        std::ostringstream request;
        request << "GET " << path << " HTTP/1.1\r\n"
                << "Host: " << host_ << "\r\n"
                << "Connection: close\r\n"
                << "\r\n";

        std::string req_str = request.str();
        if (send(sockfd_, req_str.c_str(), req_str.size(), 0) < 0) {
            return "";
        }

        return read_response();
    }

    /**
     * @brief 发送POST请求
     */
    std::string send_post(const std::string& path, const std::string& body,
                          const std::string& content_type = "application/x-www-form-urlencoded") {
        if (sockfd_ < 0 && !connect()) {
            return "";
        }

        std::ostringstream request;
        request << "POST " << path << " HTTP/1.1\r\n"
                << "Host: " << host_ << "\r\n"
                << "Content-Type: " << content_type << "\r\n"
                << "Content-Length: " << body.size() << "\r\n"
                << "Connection: close\r\n"
                << "\r\n"
                << body;

        std::string req_str = request.str();
        if (send(sockfd_, req_str.c_str(), req_str.size(), 0) < 0) {
            return "";
        }

        return read_response();
    }

    /**
     * @brief 关闭连接
     */
    void close_connection() {
        if (sockfd_ >= 0) {
            close(sockfd_);
            sockfd_ = -1;
        }
    }

    /**
     * @brief 检查响应状态码
     */
    static int get_status_code(const std::string& response) {
        // 查找HTTP/1.1 XXX
        size_t pos = response.find("HTTP/1.1 ");
        if (pos == std::string::npos) {
            return -1;
        }

        pos += 9;  // "HTTP/1.1 "的长度
        if (pos + 3 > response.size()) {
            return -1;
        }

        try {
            return std::stoi(response.substr(pos, 3));
        } catch (...) {
            return -1;
        }
    }

    /**
     * @brief 提取响应体
     */
    static std::string get_response_body(const std::string& response) {
        // 查找\r\n\r\n（头部结束标志）
        size_t pos = response.find("\r\n\r\n");
        if (pos == std::string::npos) {
            return "";
        }

        return response.substr(pos + 4);
    }

private:
    /**
     * @brief 读取响应
     */
    std::string read_response() {
        std::string response;
        char buffer[4096];
        ssize_t n;

        while ((n = recv(sockfd_, buffer, sizeof(buffer), 0)) > 0) {
            response.append(buffer, n);
        }

        return response;
    }

    std::string host_;
    int port_;
    int sockfd_;
};

/**
 * @brief 测试辅助函数
 */
namespace TestHelper {

/**
 * @brief 生成随机字符串
 */
inline std::string random_string(size_t length) {
    static const char charset[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

    std::string result;
    result.reserve(length);

    for (size_t i = 0; i < length; ++i) {
        result += charset[rand() % (sizeof(charset) - 1)];
    }

    return result;
}

/**
 * @brief 创建临时测试文件
 */
inline bool create_test_file(const std::string& path, const std::string& content) {
    FILE* file = fopen(path.c_str(), "w");
    if (!file) {
        return false;
    }

    fwrite(content.c_str(), 1, content.size(), file);
    fclose(file);
    return true;
}

/**
 * @brief 删除测试文件
 */
inline void remove_test_file(const std::string& path) {
    unlink(path.c_str());
}

/**
 * @brief 等待端口可用
 */
inline bool wait_for_port(const std::string& host, int port, int timeout_ms = 5000) {
    auto start = std::chrono::steady_clock::now();

    while (true) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            return false;
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

        if (::connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            close(sock);
            return true;
        }

        close(sock);

        auto elapsed = std::chrono::steady_clock::now() - start;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > timeout_ms) {
            break;
        }

        usleep(100000);  // 100ms
    }

    return false;
}

} // namespace TestHelper

#endif // TESTS_UTILS_TEST_HELPER_H
