# TinyWebServer 测试框架文档

## 📋 概述

本测试框架为基于C++20协程的TinyWebServer项目提供全面的测试支持，包括单元测试、集成测试和性能测试。

## 🏗️ 架构

```
tests/
├── unit/                   # 单元测试
│   ├── coroutine/         # 协程相关测试
│   │   ├── task_test.cpp
│   │   └── scheduler_test.cpp
│   ├── http/              # HTTP相关测试
│   │   └── file_db_manager_test.cpp
│   ├── threadpool/        # 线程池测试
│   │   └── cpu_thread_pool_test.cpp
│   └── io_uring/          # io_uring测试（待实现）
├── integration/           # 集成测试（待扩展）
├── performance/           # 性能测试（待扩展）
├── utils/                 # 测试工具库
│   ├── simple_test.h      # 简化测试框架
│   ├── coroutine_test_runner.h  # 协程测试运行器
│   ├── test_database.h    # 测试数据库管理
│   └── test_helper.h      # HTTP客户端等工具
├── fixtures/              # 测试数据
│   ├── test_files/        # 测试文件
│   └── sql/               # SQL脚本
└── Makefile.test          # 测试构建系统
```

## 🚀 快速开始

### 1. 环境准备

**必需依赖:**
- g++-10 或更高版本（支持C++20协程）
- liburing （io_uring支持）
- MySQL 客户端库
- pthread

**可选依赖:**
- Google Test（如果安装则自动使用，否则使用内置简化测试框架）

### 2. 编译测试

```bash
cd tests
make -f Makefile.test all
```

### 3. 运行测试

**运行所有单元测试:**
```bash
make -f Makefile.test test-unit
```

**运行特定测试:**
```bash
./unit/coroutine/task_test
./unit/coroutine/scheduler_test
./unit/threadpool/cpu_thread_pool_test
./unit/http/file_db_manager_test
```

**清理测试二进制:**
```bash
make -f Makefile.test clean
```

## 📚 测试工具库

### CoroTestRunner

用于在测试环境中同步运行协程的工具类。

**示例:**
```cpp
#include "utils/coroutine_test_runner.h"

TEST(MyTest, CoroTest) {
    CoroTestRunner runner;

    // 运行返回int的协程
    Task<int> task = my_async_function();
    int result = runner.run_sync(std::move(task));
    EXPECT_EQ(result, 42);

    // 运行void协程
    Task<void> void_task = my_void_async();
    runner.run_sync_void(std::move(void_task));
}
```

**API:**
- `run_sync<T>(Task<T> task, int timeout_ms)` - 同步运行协程并返回结果
- `run_sync_void(Task<void> task, int timeout_ms)` - 运行void协程
- `pump_io_uring(int timeout_ms)` - 手动泵送io_uring事件
- `get_io_uring_manager()` - 获取IoUringManager实例

### TestDatabase

自动创建和管理临时测试数据库。

**示例:**
```cpp
#include "utils/test_database.h"

TEST(MyDBTest, InsertRecord) {
    TestDatabase test_db;
    ASSERT_TRUE(test_db.init());  // 创建临时数据库

    MYSQL* conn = test_db.get_connection();
    // ... 执行数据库操作 ...
    test_db.release_connection(conn);

    // 测试结束时自动清理数据库
}
```

**API:**
- `init()` - 初始化测试数据库（创建表结构）
- `get_connection()` - 获取数据库连接
- `release_connection(MYSQL*)` - 释放连接
- `clear_all_tables()` - 清空所有表数据
- `execute_sql(string)` - 执行SQL语句
- `cleanup()` - 清理测试数据库（自动调用）

### TestHttpClient

简单的HTTP客户端，用于集成测试。

**示例:**
```cpp
#include "utils/test_helper.h"

TEST(HttpTest, GetRequest) {
    TestHttpClient client("127.0.0.1", 9006);

    std::string response = client.send_get("/index.html");
    int status_code = TestHttpClient::get_status_code(response);

    EXPECT_EQ(status_code, 200);
}
```

**API:**
- `connect()` - 连接到服务器
- `send_get(path)` - 发送GET请求
- `send_post(path, body, content_type)` - 发送POST请求
- `get_status_code(response)` - 提取状态码（静态）
- `get_response_body(response)` - 提取响应体（静态）

## 📝 编写测试

### 基本测试

```cpp
#include "utils/simple_test.h"

TEST(MyTestSuite, MyTestCase) {
    // 测试代码
    int result = 2 + 2;
    EXPECT_EQ(result, 4);
}

int main() {
    return RUN_ALL_TESTS();
}
```

### Fixture测试

```cpp
class MyFixture {
protected:
    void SetUp() {
        // 测试前准备
    }

    void TearDown() {
        // 测试后清理
    }

    // 共享数据
    int shared_data;
};

TEST_F(MyFixture, TestWithFixture) {
    shared_data = 10;
    EXPECT_GT(shared_data, 0);
}
```

### 协程测试

```cpp
#include "utils/coroutine_test_runner.h"

Task<int> my_coroutine() {
    co_return 42;
}

TEST(CoroTest, BasicTest) {
    CoroTestRunner runner;
    int result = runner.run_sync(my_coroutine());
    EXPECT_EQ(result, 42);
}
```

## 🧪 断言宏

### 基本断言
- `EXPECT_TRUE(condition)` - 期望为真
- `EXPECT_FALSE(condition)` - 期望为假
- `EXPECT_EQ(val1, val2)` - 期望相等
- `EXPECT_NE(val1, val2)` - 期望不等
- `EXPECT_LT(val1, val2)` - 期望小于
- `EXPECT_GT(val1, val2)` - 期望大于
- `EXPECT_STREQ(str1, str2)` - 期望字符串相等
- `EXPECT_NEAR(val1, val2, error)` - 期望接近

### ASSERT vs EXPECT
- `EXPECT_*` - 失败后继续执行
- `ASSERT_*` - 失败后立即退出测试

## 📊 测试覆盖

### 当前覆盖范围

| 模块 | 测试文件 | 覆盖内容 |
|------|---------|---------|
| 协程Task | task_test.cpp | 基本返回、参数传递、异常传播、链式调用 |
| 协程调度器 | scheduler_test.cpp | spawn、优先级、限制、统计、关闭 |
| CPU线程池 | cpu_thread_pool_test.cpp | 任务入队、并发、队列限制、统计 |
| 数据库管理 | file_db_manager_test.cpp | 插入、更新、软删除、查询、搜索 |

### 待扩展测试
- [ ] io_uring awaiter测试
- [ ] HTTP解析测试
- [ ] 文件上传/下载集成测试
- [ ] 高并发场景测试
- [ ] 性能基准测试

## 🔧 故障排查

### 编译错误

**错误**: `fatal error: gtest/gtest.h: No such file or directory`

**解决**: 这是正常的，测试框架会自动降级到简化版本。如果需要使用Google Test：
```bash
sudo apt-get install libgtest-dev
cd /usr/src/gtest
sudo cmake .
sudo make
sudo cp lib/*.a /usr/lib
```

### 运行时错误

**错误**: `Failed to initialize IoUringManager`

**解决**: 检查liburing是否正确安装，内核是否支持io_uring（需要5.1+）

**错误**: `mysql_real_connect() failed`

**解决**: 确保MySQL服务运行，检查连接参数（host/user/password）

## 📈 性能测试

### 运行性能测试

```bash
make -f Makefile.test test-performance
```

### 自定义基准

```cpp
TEST(PerformanceTest, Throughput) {
    auto start = std::chrono::steady_clock::now();

    // 执行操作
    for (int i = 0; i < 10000; ++i) {
        // ...
    }

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "Throughput: " << (10000 * 1000 / duration.count()) << " ops/sec" << std::endl;
}
```

## 🤝 贡献指南

### 添加新测试

1. 在合适的目录下创建 `*_test.cpp` 文件
2. 包含 `utils/simple_test.h`
3. 编写TEST宏定义的测试用例
4. 添加main()函数调用RUN_ALL_TESTS()
5. 更新 Makefile.test 的 TEST_SRCS

### 测试命名规范

- 测试文件: `<module>_test.cpp`
- 测试套件: `<Module>Test`
- 测试用例: 描述性名称，如 `BasicReturn`, `HandleEdgeCase`

## 📞 获取帮助

如有问题，请：
1. 查看测试输出的详细错误信息
2. 运行 `make -f Makefile.test help` 查看可用命令
3. 查看现有测试代码作为参考

## 🎯 未来计划

- [ ] 集成到CI/CD流程
- [ ] 生成代码覆盖率报告（gcov/lcov）
- [ ] 添加更多集成测试
- [ ] 性能回归检测
- [ ] 内存泄漏检测（Valgrind集成）
- [ ] 模糊测试（Fuzz Testing）

---

**最后更新**: 2025-10-23
**测试框架版本**: 1.0.0
