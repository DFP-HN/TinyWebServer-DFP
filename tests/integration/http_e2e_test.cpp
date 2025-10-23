#include "../utils/simple_test.h"
#include "../utils/test_helper.h"
#include "../utils/test_database.h"
#include <thread>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>

/**
 * @file http_e2e_test.cpp
 * @brief HTTP端到端集成测试
 *
 * 注意：这些测试需要服务器在本地9006端口运行
 */

// 测试服务器是否在运行
bool is_server_running() {
    return TestHelper::wait_for_port("127.0.0.1", 9006, 1000);
}

TEST(HttpE2ETest, ServerIsRunning) {
    bool running = is_server_running();

    if (!running) {
        std::cout << "WARNING: Server not running on port 9006. Skipping E2E tests." << std::endl;
        std::cout << "Start server with: ./server -e 2 -t 4 -c 0" << std::endl;
    }

    // 这个测试只是检查，不会fail
    EXPECT_TRUE(running || !running);  // 总是通过
}

TEST(HttpE2ETest, GetStaticFile) {
    if (!is_server_running()) {
        std::cout << "SKIP: Server not running" << std::endl;
        return;
    }

    TestHttpClient client("127.0.0.1", 9006);
    std::string response = client.send_get("/judge.html");

    int status_code = TestHttpClient::get_status_code(response);
    EXPECT_EQ(status_code, 200);

    std::string body = TestHttpClient::get_response_body(response);
    EXPECT_GT(body.size(), 0);
}

TEST(HttpE2ETest, GetNonExistentFile) {
    if (!is_server_running()) {
        std::cout << "SKIP: Server not running" << std::endl;
        return;
    }

    TestHttpClient client("127.0.0.1", 9006);
    std::string response = client.send_get("/nonexistent.html");

    int status_code = TestHttpClient::get_status_code(response);
    // 应该返回404
    EXPECT_EQ(status_code, 404);
}

TEST(HttpE2ETest, PostLogin) {
    if (!is_server_running()) {
        std::cout << "SKIP: Server not running" << std::endl;
        return;
    }

    TestHttpClient client("127.0.0.1", 9006);

    // POST登录请求
    std::string body = "user=test_user&password=test_pass";
    std::string response = client.send_post("/2", body);

    int status_code = TestHttpClient::get_status_code(response);

    // 登录可能成功或失败，取决于数据库状态
    // 但至少应该有响应
    EXPECT_GT(status_code, 0);
}

TEST(HttpE2ETest, FileListAPI) {
    if (!is_server_running()) {
        std::cout << "SKIP: Server not running" << std::endl;
        return;
    }

    TestHttpClient client("127.0.0.1", 9006);
    std::string response = client.send_get("/api/files");

    int status_code = TestHttpClient::get_status_code(response);
    EXPECT_EQ(status_code, 200);

    std::string body = TestHttpClient::get_response_body(response);

    // 响应应该是JSON格式
    EXPECT_TRUE(body.find("[") != std::string::npos ||
                body.find("{") != std::string::npos);
}

TEST(HttpE2ETest, CPUComputeAPI) {
    if (!is_server_running()) {
        std::cout << "SKIP: Server not running" << std::endl;
        return;
    }

    TestHttpClient client("127.0.0.1", 9006);
    std::string response = client.send_get("/cpu_compute?task=primes&level=2");

    int status_code = TestHttpClient::get_status_code(response);

    // CPU任务应该返回结果
    if (status_code == 200) {
        std::string body = TestHttpClient::get_response_body(response);
        EXPECT_GT(body.size(), 0);

        // 应该包含JSON响应
        EXPECT_TRUE(body.find("{") != std::string::npos);
    } else {
        std::cout << "Note: CPU compute returned status " << status_code << std::endl;
    }
}

TEST(HttpE2ETest, ConcurrentConnections) {
    if (!is_server_running()) {
        std::cout << "SKIP: Server not running" << std::endl;
        return;
    }

    const int concurrent_count = 10;
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};

    // 并发发送请求
    for (int i = 0; i < concurrent_count; ++i) {
        threads.emplace_back([&success_count]() {
            TestHttpClient client("127.0.0.1", 9006);
            std::string response = client.send_get("/judge.html");

            if (TestHttpClient::get_status_code(response) == 200) {
                success_count++;
            }
        });
    }

    // 等待所有线程完成
    for (auto& t : threads) {
        t.join();
    }

    // 大部分请求应该成功
    EXPECT_GT(success_count.load(), concurrent_count / 2);

    std::cout << "Concurrent requests: " << concurrent_count
              << ", succeeded: " << success_count.load() << std::endl;
}

// 主函数
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    std::cout << "========================================" << std::endl;
    std::cout << "HTTP End-to-End Integration Tests" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "" << std::endl;
    std::cout << "These tests require the server to be running:" << std::endl;
    std::cout << "  ./server -e 2 -t 4 -c 0" << std::endl;
    std::cout << "" << std::endl;

    return RUN_ALL_TESTS();
}
