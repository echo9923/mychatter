/*
 * chat_message 幂等持久化迁移（计划 §3.1）
 *
 * 目标：分离“展示状态(status/msg_type)”与“投递状态(delivery_status)”，
 * 为应用层至少一次投递、ACK 幂等去重与离线拉取建立可证明的持久化真值。
 *
 * 新增列：
 *   - unique_id      VARCHAR(64) NULL    客户端去重标识；历史/系统消息保持 NULL（禁止空串，
 *                                        否则同一 sender 的旧数据会撞唯一键）
 *   - content_size   BIGINT UNSIGNED NOT NULL DEFAULT 0
 *                                        文本为 0，图片为字节数（供离线图片元数据）
 *   - delivery_status TINYINT NOT NULL DEFAULT 1
 *                                        0=待投递 1=已投递(ACK)。新增列时默认 1，使全部
 *                                        历史行立即被标记为“已投递”，避免升级后把历史记录
 *                                        当作离线消息重推；随后把列默认值改为 0，使新写入
 *                                        的消息默认进入 pending 集合。
 *
 * 新增索引：
 *   - UNIQUE KEY uk_chat_message_sender_unique(sender_id, unique_id)
 *                                        同一 sender 下 unique_id 唯一；MySQL 允许多个
 *                                        NULL，因此历史/系统消息(unique_id=NULL)互不冲突。
 *   - INDEX idx_chat_message_pending(recv_id, delivery_status, message_id)
 *                                        支撑按 recv_uid 拉取待投递消息。
 *
 * 幂等性：脚本可由 mysql 客户端重复执行。对每条 DDL 先在 information_schema
 *         校验目标列/索引不存在才执行；已存在则跳过（假定类型一致，由执行前人工核查保证）。
 *
 * 执行前人工核查：已在 live MySQL information_schema 确认 chat_message 仅含
 *   message_id/thread_id/sender_id/recv_id/content/created_at/updated_at/status/msg_type
 *   九列与 PRIMARY/idx_thread_created/idx_thread_message 三索引，
 *   unique_id/content_size/delivery_status 三列及 uk_chat_message_sender_unique/
 *   idx_chat_message_pending 两索引均不存在，类型无冲突。
 *
 * 用法： mysql -h<host> -P<port> -u<user> -p<pass> llfc < 20260730_message_reliability.sql
 */

SET NAMES utf8mb4;

-- 切换到目标库（调用方亦可在命令行指定库）
USE `llfc`;

-- ----------------------------------------------------------------------
-- 辅助存储过程：仅在列不存在时新增
-- ----------------------------------------------------------------------
DROP PROCEDURE IF EXISTS `_mr_add_column_if_absent`;
DROP PROCEDURE IF EXISTS `_mr_modify_column`;
DROP PROCEDURE IF EXISTS `_mr_add_index_if_absent`;
DROP PROCEDURE IF EXISTS `_mr_drop_helper_procs`;

DELIMITER $$

-- 列不存在则执行 ADD COLUMN，存在则跳过
CREATE PROCEDURE `_mr_add_column_if_absent`(
  IN p_table VARCHAR(64),
  IN p_column VARCHAR(64),
  IN p_col_ddl TEXT
)
BEGIN
  DECLARE col_exists INT DEFAULT 0;
  SELECT COUNT(*) INTO col_exists
    FROM information_schema.columns
    WHERE table_schema = DATABASE()
      AND table_name   = p_table
      AND column_name  = p_column;
  IF col_exists = 0 THEN
    SET @mr_sql = CONCAT('ALTER TABLE `', p_table, '` ADD COLUMN ', p_col_ddl);
    PREPARE mr_stmt FROM @mr_sql; EXECUTE mr_stmt; DEALLOCATE PREPARE mr_stmt;
  ELSE
    SELECT CONCAT('skip: column `', p_column, '` already exists on `', p_table, '`') AS msg;
  END IF;
END$$

-- 无条件 MODIFY COLUMN（用于调整列默认值/注释，幂等）
CREATE PROCEDURE `_mr_modify_column`(
  IN p_table VARCHAR(64),
  IN p_column VARCHAR(64),
  IN p_col_ddl TEXT
)
BEGIN
  SET @mr_sql = CONCAT('ALTER TABLE `', p_table, '` MODIFY COLUMN `', p_column, '` ', p_col_ddl);
  PREPARE mr_stmt FROM @mr_sql; EXECUTE mr_stmt; DEALLOCATE PREPARE mr_stmt;
END$$

-- 索引不存在则新增，存在则跳过
CREATE PROCEDURE `_mr_add_index_if_absent`(
  IN p_table VARCHAR(64),
  IN p_index VARCHAR(64),
  IN p_idx_ddl TEXT
)
BEGIN
  DECLARE idx_exists INT DEFAULT 0;
  SELECT COUNT(*) INTO idx_exists
    FROM information_schema.statistics
    WHERE table_schema = DATABASE()
      AND table_name   = p_table
      AND index_name   = p_index;
  IF idx_exists = 0 THEN
    SET @mr_sql = CONCAT('ALTER TABLE `', p_table, '` ADD ', p_idx_ddl);
    PREPARE mr_stmt FROM @mr_sql; EXECUTE mr_stmt; DEALLOCATE PREPARE mr_stmt;
  ELSE
    SELECT CONCAT('skip: index `', p_index, '` already exists on `', p_table, '`') AS msg;
  END IF;
END$$

DELIMITER ;

-- ----------------------------------------------------------------------
-- 1. 新增列（必须先于索引，因 idx_chat_message_pending 依赖 delivery_status）
-- ----------------------------------------------------------------------
-- unique_id：历史/系统消息保持 NULL，禁止空串
CALL `_mr_add_column_if_absent`('chat_message', 'unique_id',
  '`unique_id` VARCHAR(64) CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci NULL DEFAULT NULL COMMENT ''客户端去重标识，历史/系统消息为NULL''');

-- content_size：文本为 0，图片为字节数
CALL `_mr_add_column_if_absent`('chat_message', 'content_size',
  '`content_size` BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT ''文本为0，图片为字节数''');

-- delivery_status：新增时默认 1，使历史行立即被标记为已投递，避免升级后重推
CALL `_mr_add_column_if_absent`('chat_message', 'delivery_status',
  '`delivery_status` TINYINT NOT NULL DEFAULT 1 COMMENT ''0=待投递 1=已投递(ACK)''');

-- ----------------------------------------------------------------------
-- 2. 历史行已因 DEFAULT 1 被标记为已投递；新行默认改为待投递(0)
--    （MODIFY 仅改列默认值，不改动已有行的值）
-- ----------------------------------------------------------------------
CALL `_mr_modify_column`('chat_message', 'delivery_status',
  'TINYINT NOT NULL DEFAULT 0 COMMENT ''0=待投递 1=已投递(ACK)''');

-- ----------------------------------------------------------------------
-- 3. 唯一键与待投递查询索引
-- ----------------------------------------------------------------------
-- 同一 sender 下 unique_id 唯一；多个 NULL 互不冲突，故历史/系统消息安全
CALL `_mr_add_index_if_absent`('chat_message', 'uk_chat_message_sender_unique',
  'UNIQUE KEY `uk_chat_message_sender_unique`(`sender_id`, `unique_id`)');

-- 支撑按 recv_uid 分页拉取待投递消息
CALL `_mr_add_index_if_absent`('chat_message', 'idx_chat_message_pending',
  'INDEX `idx_chat_message_pending`(`recv_id`, `delivery_status`, `message_id`)');

-- ----------------------------------------------------------------------
-- 4. 清理辅助存储过程
-- ----------------------------------------------------------------------
DROP PROCEDURE IF EXISTS `_mr_add_column_if_absent`;
DROP PROCEDURE IF EXISTS `_mr_modify_column`;
DROP PROCEDURE IF EXISTS `_mr_add_index_if_absent`;
