#include "../../utils/simple_test.h"
#include "../../utils/test_database.h"
#include "../../../http/file_db_manager.h"
#include <cstdio>

/**
 * @file file_db_manager_test.cpp
 * @brief FileDBManager的单元测试
 */

class FileDBManagerTest {
public:
    TestDatabase* test_db_;
    MYSQL* conn_;

    void SetUp() {
        test_db_ = new TestDatabase();
        ASSERT_TRUE(test_db_->init());
        conn_ = test_db_->get_connection();
        ASSERT_TRUE(conn_ != nullptr);
    }

    void TearDown() {
        if (conn_) {
            test_db_->release_connection(conn_);
        }
        delete test_db_;
    }
};

TEST_F(FileDBManagerTest, InsertFile) {
    const char* filename = "test_file.txt";
    const char* filepath = "./test_file.txt";
    uint64_t filesize = 1024;
    const char* hash = "abc123";
    const char* ext = ".txt";

    bool success = FileDBManager::insert_file(
        conn_, filename, filepath, filesize, hash, ext
    );

    EXPECT_TRUE(success);

    // 验证插入成功
    const char* query = "SELECT * FROM files WHERE filename = 'test_file.txt'";
    ASSERT_EQ(mysql_query(conn_, query), 0);

    MYSQL_RES* result = mysql_store_result(conn_);
    ASSERT_TRUE(result != nullptr);

    MYSQL_ROW row = mysql_fetch_row(result);
    EXPECT_TRUE(row != nullptr);

    mysql_free_result(result);
}

TEST_F(FileDBManagerTest, DuplicateKeyUpdate) {
    const char* filename = "duplicate_file.txt";
    const char* filepath = "./duplicate_file.txt";

    // 第一次插入
    bool success1 = FileDBManager::insert_file(
        conn_, filename, filepath, 1000, "hash1", ".txt"
    );
    EXPECT_TRUE(success1);

    // 第二次插入相同路径（应该更新）
    bool success2 = FileDBManager::insert_file(
        conn_, filename, filepath, 2000, "hash2", ".txt"
    );
    EXPECT_TRUE(success2);

    // 验证文件大小被更新为2000
    const char* query = "SELECT file_size, file_hash FROM files WHERE file_path = './duplicate_file.txt'";
    ASSERT_EQ(mysql_query(conn_, query), 0);

    MYSQL_RES* result = mysql_store_result(conn_);
    MYSQL_ROW row = mysql_fetch_row(result);

    ASSERT_TRUE(row != nullptr);
    EXPECT_STREQ(row[0], "2000");  // file_size
    EXPECT_STREQ(row[1], "hash2"); // file_hash

    mysql_free_result(result);
}

TEST_F(FileDBManagerTest, SoftDelete) {
    const char* filepath = "./delete_test.txt";

    // 先插入文件
    FileDBManager::insert_file(
        conn_, "delete_test.txt", filepath, 500, "hash", ".txt"
    );

    // 软删除
    bool success = FileDBManager::delete_file_by_path(conn_, filepath);
    EXPECT_TRUE(success);

    // 验证status变为0
    const char* query = "SELECT status FROM files WHERE file_path = './delete_test.txt'";
    ASSERT_EQ(mysql_query(conn_, query), 0);

    MYSQL_RES* result = mysql_store_result(conn_);
    MYSQL_ROW row = mysql_fetch_row(result);

    ASSERT_TRUE(row != nullptr);
    EXPECT_STREQ(row[0], "0");  // status = 0

    mysql_free_result(result);
}

TEST_F(FileDBManagerTest, QueryFiles) {
    // 插入多个文件
    FileDBManager::insert_file(conn_, "file1.txt", "./file1.txt", 100, "hash1", ".txt");
    FileDBManager::insert_file(conn_, "file2.jpg", "./file2.jpg", 200, "hash2", ".jpg");
    FileDBManager::insert_file(conn_, "file3.mp4", "./file3.mp4", 300, "hash3", ".mp4");

    // 查询所有文件
    std::vector<FileRecord> files = FileDBManager::query_all_files(conn_, "upload_time DESC", 100);

    EXPECT_GE(files.size(), 3);
}

TEST_F(FileDBManagerTest, SearchFiles) {
    // 插入测试文件
    FileDBManager::insert_file(conn_, "test_video.mp4", "./test_video.mp4", 1000, "h1", ".mp4");
    FileDBManager::insert_file(conn_, "demo_image.jpg", "./demo_image.jpg", 2000, "h2", ".jpg");
    FileDBManager::insert_file(conn_, "test_doc.pdf", "./test_doc.pdf", 3000, "h3", ".pdf");

    // 搜索包含"test"的文件
    SearchCriteria criteria;
    criteria.keyword = "test";
    criteria.extension = ".mp4";
    criteria.sort_by = "upload_time";
    criteria.sort_order = "DESC";
    criteria.limit = 10;

    std::vector<FileRecord> files = FileDBManager::search_files(conn_, criteria, 10);

    EXPECT_GT(files.size(), 0);

    // 验证至少有一个文件名包含"test"
    bool found = false;
    for (const auto& file : files) {
        if (file.filename.find("test") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

// 主函数
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    std::cout << "Running FileDBManager unit tests...\n" << std::endl;
    std::cout << "Note: These tests require a running MySQL server\n" << std::endl;

    return RUN_ALL_TESTS();
}
