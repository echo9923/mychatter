/*
 * chat_message 资源消息三列迁移（图片与文件统一传输改造）
 *
 * 目标：图片与普通文件统一为“资源消息”（创建元数据 -> 查询上传位置 -> 分片上传
 *       -> 完整性校验 -> 发布消息 -> 分片下载），chat_message 需要表达资源生命周期。
 *
 * 新增列：
 *   - resource_status TINYINT NOT NULL DEFAULT 0
 *       资源生命周期单一真值：0=待上传 1=就绪可下载 2=失败/过期。
 *       仅 msg_type 1(图片)/3(文件) 有意义；文本恒为 0。
 *       status 的 3=UN_UPLOAD 语义废弃，资源状态一律读本列。
 *   - content_hash CHAR(64) NULL
 *       整文件 SHA-256（小写 hex）。资源消息必填（1035 创建时随元数据写入），
 *       服务端在收齐分片后据此做整文件校验；文本消息为 NULL。
 *   - mime_type VARCHAR(128) NULL
 *       资源 MIME 类型（如 image/png、application/pdf），仅展示用。
 *
 * 索引：
 *   - KEY idx_resource_pending(resource_status, updated_at)
 *       支撑 ResourceServer 每小时清理任务：捞取 resource_status=0 且
 *       updated_at 早于 7 天前的行，标记为 2 并补写双方同步行。
 *
 * 数据处置（本改造为破坏性变更，三端同批发布，不兼容旧协议）：
 *   - TRUNCATE chat_message / user_message_sync（清库重建，已确认）；
 *   - 旧图片按 unique_name 存于 ResourceServer bin/static/<uid>/ 下，与新方案
 *     <Output>/resource/<sender_uid>/<message_id> 布局不兼容，直接作废；
 *     开发环境可整目录清空（头像需重传）。
 *
 * 语义变更：
 *   - content 对资源消息保存“原始文件名”（UTF-8、sanitize、<=255B），仅展示用；
 *     磁盘文件统一以 message_id 命名，根除路径穿越与重名。
 *   - status 收紧为纯阅读态：0=未读 1=发送失败 2=已读（3 已废弃）。
 *
 * 幂等性：ALTER 前以 information_schema 探测列/索引是否存在，脚本可重复执行。
 *
 * 用法： mysql -h<host> -P<port> -u<user> -p<pass> llfc < 20260814_resource_unified.sql
 *        发布顺序：先执行本脚本，再发布 ResourceServer/ChatServer，最后发客户端。
 */

SET NAMES utf8mb4;

USE `llfc`;

-- 清库重建：旧图片消息与新磁盘布局/协议均不兼容，不做存量迁移
TRUNCATE TABLE `chat_message`;
TRUNCATE TABLE `user_message_sync`;

-- 加列（幂等：已存在则跳过）
SET @has_resource_status := (
    SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'chat_message'
      AND COLUMN_NAME = 'resource_status');
SET @ddl := IF(@has_resource_status = 0,
    'ALTER TABLE `chat_message` ADD COLUMN `resource_status` TINYINT NOT NULL DEFAULT 0 COMMENT ''资源状态：0=待上传 1=就绪 2=失败/过期（仅 msg_type 1/3 有意义）'' AFTER `msg_type`',
    'SELECT 1');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @has_content_hash := (
    SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'chat_message'
      AND COLUMN_NAME = 'content_hash');
SET @ddl := IF(@has_content_hash = 0,
    'ALTER TABLE `chat_message` ADD COLUMN `content_hash` CHAR(64) NULL DEFAULT NULL COMMENT ''整文件 SHA-256（小写 hex），资源消息必填'' AFTER `content_size`',
    'SELECT 1');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @has_mime_type := (
    SELECT COUNT(*) FROM information_schema.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'chat_message'
      AND COLUMN_NAME = 'mime_type');
SET @ddl := IF(@has_mime_type = 0,
    'ALTER TABLE `chat_message` ADD COLUMN `mime_type` VARCHAR(128) NULL DEFAULT NULL COMMENT ''资源 MIME 类型（如 image/png），仅展示用'' AFTER `content_hash`',
    'SELECT 1');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- 清理任务索引（幂等）
SET @has_idx := (
    SELECT COUNT(*) FROM information_schema.STATISTICS
    WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'chat_message'
      AND INDEX_NAME = 'idx_resource_pending');
SET @ddl := IF(@has_idx = 0,
    'ALTER TABLE `chat_message` ADD INDEX `idx_resource_pending`(`resource_status`, `updated_at`)',
    'SELECT 1');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- status 语义收紧：3=未上传完成 已废弃（迁移至 resource_status），重写注释
ALTER TABLE `chat_message` MODIFY COLUMN `status` TINYINT NOT NULL DEFAULT 0
    COMMENT '0=未读 1=发送失败 2=已读（3 已废弃，语义迁移至 resource_status）';
