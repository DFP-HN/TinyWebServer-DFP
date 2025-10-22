-- =============================================
-- TinyWebServer - 文件信息表初始化脚本
-- =============================================
-- 用途：将文件元数据存储到MySQL数据库
-- 执行方式：mysql -u root -p yourdb < init_files_table.sql
-- =============================================

USE yourdb;

-- 创建文件信息表
CREATE TABLE IF NOT EXISTS `files` (
  `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '文件ID（主键）',
  `filename` VARCHAR(255) NOT NULL COMMENT '原始文件名（支持中文）',
  `file_path` VARCHAR(512) NOT NULL COMMENT '服务器存储路径（相对路径，如./root/uploads/xxx.pdf）',
  `file_size` BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '文件大小（字节）',
  `file_hash` CHAR(32) NULL COMMENT 'MD5哈希值（用于去重、断点续传）',
  `file_type` VARCHAR(50) NULL COMMENT '文件类型（pdf/video/image/document/archive/audio/other）',
  `extension` VARCHAR(20) NULL COMMENT '文件扩展名（含点号，如.pdf）',
  `upload_time` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP COMMENT '上传时间',
  `modified_time` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP COMMENT '修改时间',
  `status` TINYINT NOT NULL DEFAULT 1 COMMENT '状态：1=正常, 0=已删除（软删除）',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_file_path` (`file_path`),
  KEY `idx_filename` (`filename`(100)),
  KEY `idx_extension` (`extension`),
  KEY `idx_file_hash` (`file_hash`),
  KEY `idx_upload_time` (`upload_time`),
  KEY `idx_status` (`status`),
  KEY `idx_file_size` (`file_size`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='文件信息表';

-- 索引说明
-- uk_file_path: 唯一索引，防止重复路径，加速路径查询
-- idx_filename: 加速文件名模糊搜索（LIKE查询）
-- idx_extension: 加速扩展名过滤
-- idx_file_hash: 加速哈希去重查询
-- idx_upload_time: 加速时间范围查询和排序
-- idx_status: 加速软删除过滤
-- idx_file_size: 加速大小范围查询

-- 插入示例数据（可选）
-- INSERT INTO files (filename, file_path, file_size, file_hash, file_type, extension) VALUES
-- ('测试文件.pdf', './root/uploads/test.pdf', 102400, 'd41d8cd98f00b204e9800998ecf8427e', 'pdf', '.pdf'),
-- ('视频教程.mp4', './root/uploads/video.mp4', 10485760, 'a1b2c3d4e5f6g7h8i9j0k1l2m3n4o5p6', 'video', '.mp4');

-- 查询统计信息
SELECT
    COUNT(*) as total_files,
    COUNT(DISTINCT file_type) as total_types,
    SUM(file_size) as total_size_bytes,
    ROUND(SUM(file_size) / 1024 / 1024, 2) as total_size_mb
FROM files
WHERE status = 1;

-- 表结构说明
-- file_type 分类规则（根据扩展名自动识别）：
--   'pdf'      - PDF文档
--   'video'    - 视频文件（mp4, avi, mov, mkv等）
--   'image'    - 图片文件（jpg, png, gif等）
--   'document' - 文档文件（doc, docx, txt, xls等）
--   'archive'  - 压缩文件（zip, rar, tar, gz等）
--   'audio'    - 音频文件（mp3, wav, flac等）
--   'other'    - 其他类型
