#ifndef TESTS_UTILS_TEST_DATABASE_H
#define TESTS_UTILS_TEST_DATABASE_H

#include "../../CGImysql/sql_connection_pool.h"
#include "../../log/log.h"
#include <mysql/mysql.h>
#include <string>
#include <memory>
#include <cstdlib>
#include <ctime>

/**
 * @brief 测试数据库管理器
 *
 * 为每个测试创建独立的临时数据库，测试结束后自动清理
 */
class TestDatabase {
public:
    TestDatabase(const std::string& host = "localhost",
                 const std::string& user = "root",
                 const std::string& password = "root")
        : host_(host)
        , user_(user)
        , password_(password)
        , test_db_name_("")
        , pool_(nullptr)
    {
        // 生成随机数据库名
        test_db_name_ = generate_test_db_name();
    }

    ~TestDatabase() {
        cleanup();
    }

    /**
     * @brief 初始化测试数据库
     *
     * 创建临时数据库和必要的表结构
     */
    bool init() {
        // 连接到MySQL服务器（不指定数据库）
        MYSQL* mysql = mysql_init(nullptr);
        if (!mysql) {
            fprintf(stderr, "mysql_init() failed\n");
            return false;
        }

        if (!mysql_real_connect(mysql, host_.c_str(), user_.c_str(),
                                password_.c_str(), nullptr, 0, nullptr, 0)) {
            fprintf(stderr, "mysql_real_connect() failed: %s\n", mysql_error(mysql));
            mysql_close(mysql);
            return false;
        }

        // 创建测试数据库
        std::string create_db_sql = "CREATE DATABASE IF NOT EXISTS " + test_db_name_;
        if (mysql_query(mysql, create_db_sql.c_str())) {
            fprintf(stderr, "CREATE DATABASE failed: %s\n", mysql_error(mysql));
            mysql_close(mysql);
            return false;
        }

        // 选择测试数据库
        if (mysql_select_db(mysql, test_db_name_.c_str())) {
            fprintf(stderr, "mysql_select_db() failed: %s\n", mysql_error(mysql));
            mysql_close(mysql);
            return false;
        }

        // 创建files表
        const char* create_files_table = R"(
            CREATE TABLE IF NOT EXISTS files (
                id INT AUTO_INCREMENT PRIMARY KEY,
                filename VARCHAR(255) NOT NULL,
                file_path VARCHAR(512) NOT NULL,
                file_size BIGINT NOT NULL,
                file_hash VARCHAR(64),
                file_type VARCHAR(50),
                extension VARCHAR(20),
                upload_time DATETIME NOT NULL,
                modified_time DATETIME DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
                status TINYINT DEFAULT 1,
                UNIQUE KEY uk_file_path (file_path),
                INDEX idx_filename (filename),
                INDEX idx_upload_time (upload_time),
                INDEX idx_status (status)
            ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4
        )";

        if (mysql_query(mysql, create_files_table)) {
            fprintf(stderr, "CREATE TABLE files failed: %s\n", mysql_error(mysql));
            mysql_close(mysql);
            return false;
        }

        // 创建user表
        const char* create_user_table = R"(
            CREATE TABLE IF NOT EXISTS user (
                username CHAR(50) NULL,
                passwd CHAR(50) NULL
            ) ENGINE=InnoDB
        )";

        if (mysql_query(mysql, create_user_table)) {
            fprintf(stderr, "CREATE TABLE user failed: %s\n", mysql_error(mysql));
            mysql_close(mysql);
            return false;
        }

        // 插入测试用户数据
        const char* insert_user = "INSERT INTO user(username, passwd) VALUES('test_user', 'test_pass')";
        mysql_query(mysql, insert_user);  // 忽略错误（可能已存在）

        mysql_close(mysql);

        // 创建连接池
        pool_ = connection_pool::GetInstance();
        pool_->init(host_, user_, password_, test_db_name_, 3306, 5, 0);

        return true;
    }

    /**
     * @brief 清理测试数据库
     */
    void cleanup() {
        if (test_db_name_.empty()) {
            return;
        }

        // 关闭连接池
        if (pool_) {
            pool_->DestroyPool();
        }

        // 删除测试数据库
        MYSQL* mysql = mysql_init(nullptr);
        if (mysql && mysql_real_connect(mysql, host_.c_str(), user_.c_str(),
                                        password_.c_str(), nullptr, 0, nullptr, 0)) {
            std::string drop_db_sql = "DROP DATABASE IF EXISTS " + test_db_name_;
            mysql_query(mysql, drop_db_sql.c_str());
            mysql_close(mysql);
        }

        test_db_name_.clear();
    }

    /**
     * @brief 获取测试数据库连接
     */
    MYSQL* get_connection() {
        if (!pool_) {
            return nullptr;
        }
        return pool_->GetConnection();
    }

    /**
     * @brief 释放数据库连接
     */
    void release_connection(MYSQL* conn) {
        if (pool_ && conn) {
            pool_->ReleaseConnection(conn);
        }
    }

    /**
     * @brief 获取测试数据库名
     */
    const std::string& get_db_name() const {
        return test_db_name_;
    }

    /**
     * @brief 清空所有表数据（保留表结构）
     */
    void clear_all_tables() {
        MYSQL* conn = get_connection();
        if (!conn) {
            return;
        }

        mysql_query(conn, "DELETE FROM files");
        mysql_query(conn, "DELETE FROM user");

        release_connection(conn);
    }

    /**
     * @brief 执行SQL语句
     */
    bool execute_sql(const std::string& sql) {
        MYSQL* conn = get_connection();
        if (!conn) {
            return false;
        }

        bool success = (mysql_query(conn, sql.c_str()) == 0);
        release_connection(conn);
        return success;
    }

private:
    /**
     * @brief 生成随机测试数据库名
     */
    std::string generate_test_db_name() {
        // 使用时间戳和随机数生成唯一数据库名
        static bool seeded = false;
        if (!seeded) {
            srand(time(nullptr));
            seeded = true;
        }

        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()).count();

        int random_num = rand() % 10000;
        return "test_db_" + std::to_string(timestamp) + "_" + std::to_string(random_num);
    }

    std::string host_;
    std::string user_;
    std::string password_;
    std::string test_db_name_;
    connection_pool* pool_;
};

#endif // TESTS_UTILS_TEST_DATABASE_H
