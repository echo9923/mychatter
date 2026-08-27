/*
 * Unified per-recipient message stream (destructive development migration).
 * This migration intentionally discards chat/friend-application history.
 */

SET NAMES utf8mb4;
USE `llfc`;

SET FOREIGN_KEY_CHECKS = 0;

DROP TABLE IF EXISTS `user_message_sync`;
DROP TABLE IF EXISTS `friend_apply`;
TRUNCATE TABLE `chat_message`;

ALTER TABLE `user`
    ADD COLUMN `last_recv_seq` BIGINT UNSIGNED NOT NULL DEFAULT 0
        COMMENT 'last sequence allocated to this user as message receiver';

UPDATE `user` SET `last_recv_seq` = 0;

ALTER TABLE `chat_message`
    DROP INDEX `idx_chat_message_pending`,
    DROP COLUMN `delivery_status`,
    MODIFY COLUMN `thread_id` BIGINT UNSIGNED NULL,
    ADD COLUMN `recv_seq` BIGINT UNSIGNED NULL AFTER `recv_id`,
    ADD COLUMN `business_status` TINYINT NOT NULL DEFAULT 0
        COMMENT '0=none 1=pending 2=accepted 3=rejected' AFTER `resource_status`,
    ADD COLUMN `related_message_id` BIGINT UNSIGNED NULL
        COMMENT 'friend result references its application message' AFTER `business_status`,
    ADD COLUMN `handled_at` DATETIME(3) NULL AFTER `related_message_id`,
    ADD COLUMN `requester_remark` VARCHAR(255) NULL AFTER `handled_at`,
    ADD COLUMN `pending_friend_key` VARCHAR(64)
        GENERATED ALWAYS AS (
            CASE
                WHEN `msg_type` = 10 AND `business_status` = 1
                THEN CONCAT(`sender_id`, ':', `recv_id`)
                ELSE NULL
            END
        ) STORED,
    ADD UNIQUE KEY `uk_chat_message_recv_seq` (`recv_id`, `recv_seq`),
    ADD KEY `idx_chat_message_recv_seq` (`recv_id`, `recv_seq`),
    ADD UNIQUE KEY `uk_chat_message_friend_result` (`related_message_id`),
    ADD UNIQUE KEY `uk_chat_message_pending_friend` (`pending_friend_key`),
    ADD KEY `idx_chat_message_friend_apply`
        (`recv_id`, `msg_type`, `business_status`, `created_at`),
    ADD KEY `idx_chat_message_sent_friend_apply`
        (`sender_id`, `msg_type`, `business_status`, `created_at`);

SET FOREIGN_KEY_CHECKS = 1;
