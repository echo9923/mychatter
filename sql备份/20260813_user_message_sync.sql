/*
 * user_message_sync 用户维度消息同步表迁移（增量同步改造）
 *
 * 目标：以 MySQL 为权威存储，为每个用户维护严格递增的同步序号 sync_seq，
 *       支撑客户端单账号增量同步（1051/1052），取代已删除的 Redis ZSET
 *       offline_msg 离线拉取机制。
 *
 * 表结构：
 *   - sync_seq   BIGINT UNSIGNED AUTO_INCREMENT 主键
 *                                        全局递增的同步序号；同一 uid 的可见消息按
 *                                        sync_seq 严格升序下发，客户端以十进制字符串
 *                                        形式的 after_sync_seq 游标增量拉取。
 *   - uid        BIGINT UNSIGNED         同步行归属用户（消息 sender 与 recv 各一行）
 *   - message_id BIGINT UNSIGNED         引用 chat_message.message_id
 *
 * 索引：
 *   - UNIQUE KEY uk_uid_message(uid, message_id)
 *                                        同一用户对同一消息只同步一次；写入方一律
 *                                        INSERT IGNORE，重复发送（幂等命中）天然无操作。
 *   - KEY idx_uid_seq(uid, sync_seq)
 *                                        支撑按 uid + after_sync_seq 的增量翻页查询。
 *
 * 写入点（均在写 chat_message 的同一事务内，同事务提交/回滚）：
 *   - ChatServer 文本/系统消息：UpsertChatMessage 命中 Stored/Duplicate 且 msg_type!=1；
 *     AddFriend 两条系统消息（申请描述、"We are friends now!"）。
 *   - ResourceServer 图片：UpdateUploadStatusWithSync 在上传完成（status=2）时补写，
 *     此前 UN_UPLOAD 图片对同步流不可见。
 *
 * 幂等性：CREATE TABLE IF NOT EXISTS，脚本可由 mysql 客户端重复执行。
 *
 * 用法： mysql -h<host> -P<port> -u<user> -p<pass> llfc < 20260813_user_message_sync.sql
 */

SET NAMES utf8mb4;

-- 切换到目标库（调用方亦可在命令行指定库）
USE `llfc`;

CREATE TABLE IF NOT EXISTS `user_message_sync` (
    `sync_seq`   BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '全局递增同步序号',
    `uid`        BIGINT UNSIGNED NOT NULL COMMENT '同步行归属用户',
    `message_id` BIGINT UNSIGNED NOT NULL COMMENT '引用chat_message.message_id',
    PRIMARY KEY (`sync_seq`),
    UNIQUE KEY `uk_uid_message` (`uid`, `message_id`),
    KEY `idx_uid_seq` (`uid`, `sync_seq`)
) ENGINE=InnoDB;
