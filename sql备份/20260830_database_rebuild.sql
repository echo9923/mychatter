-- llfcchat current-business database rebuild
-- Destructive by design: all existing application data is discarded.

SET NAMES utf8mb4;
SET time_zone = '+00:00';
SET FOREIGN_KEY_CHECKS = 0;

DROP TABLE IF EXISTS `user_events`;
DROP TABLE IF EXISTS `friend_requests`;
DROP TABLE IF EXISTS `message_resources`;
DROP TABLE IF EXISTS `chat_messages`;
DROP TABLE IF EXISTS `private_chats`;
DROP TABLE IF EXISTS `friendships`;
DROP TABLE IF EXISTS `users`;

-- Old schema names. These are dropped only; no data is copied.
DROP TABLE IF EXISTS `user_message_sync`;
DROP TABLE IF EXISTS `chat_message`;
DROP TABLE IF EXISTS `group_chat_member`;
DROP TABLE IF EXISTS `group_chat`;
DROP TABLE IF EXISTS `private_chat`;
DROP TABLE IF EXISTS `chat_thread`;
DROP TABLE IF EXISTS `friend_apply`;
DROP TABLE IF EXISTS `friend`;
DROP TABLE IF EXISTS `user`;

CREATE TABLE `users` (
    `user_id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `username` VARCHAR(64) NOT NULL,
    `email` VARCHAR(254) NOT NULL,
    `password_hash` VARBINARY(255) NOT NULL,
    `nickname` VARCHAR(64) NOT NULL DEFAULT '',
    `profile_bio` VARCHAR(255) NOT NULL DEFAULT '',
    `gender` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `avatar_key` VARCHAR(255) NOT NULL DEFAULT '',
    `last_event_seq` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`user_id`),
    UNIQUE KEY `uk_users_username` (`username`),
    UNIQUE KEY `uk_users_email` (`email`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE `friendships` (
    `lower_user_id` BIGINT UNSIGNED NOT NULL,
    `higher_user_id` BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (`lower_user_id`, `higher_user_id`),
    KEY `idx_friendships_higher` (`higher_user_id`, `lower_user_id`),
    CONSTRAINT `fk_friendships_lower`
        FOREIGN KEY (`lower_user_id`) REFERENCES `users` (`user_id`),
    CONSTRAINT `fk_friendships_higher`
        FOREIGN KEY (`higher_user_id`) REFERENCES `users` (`user_id`),
    CONSTRAINT `chk_friendships_order` CHECK (`lower_user_id` < `higher_user_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE `private_chats` (
    `thread_id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `lower_user_id` BIGINT UNSIGNED NOT NULL,
    `higher_user_id` BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (`thread_id`),
    UNIQUE KEY `uk_private_chats_pair` (`lower_user_id`, `higher_user_id`),
    KEY `idx_private_chats_higher` (`higher_user_id`, `thread_id`),
    CONSTRAINT `fk_private_chats_lower`
        FOREIGN KEY (`lower_user_id`) REFERENCES `users` (`user_id`),
    CONSTRAINT `fk_private_chats_higher`
        FOREIGN KEY (`higher_user_id`) REFERENCES `users` (`user_id`),
    CONSTRAINT `chk_private_chats_order` CHECK (`lower_user_id` < `higher_user_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE `chat_messages` (
    `message_id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `thread_id` BIGINT UNSIGNED NOT NULL,
    `sender_user_id` BIGINT UNSIGNED NOT NULL,
    `client_message_id` VARCHAR(64) NOT NULL,
    `message_type` TINYINT UNSIGNED NOT NULL,
    `text_content` TEXT NULL,
    `status` TINYINT UNSIGNED NOT NULL,
    `created_at` DATETIME(3) NOT NULL,
    PRIMARY KEY (`message_id`),
    UNIQUE KEY `uk_chat_messages_client` (`sender_user_id`, `client_message_id`),
    KEY `idx_chat_messages_history` (`thread_id`, `status`, `message_id`),
    KEY `idx_chat_messages_pending` (`status`, `message_type`, `created_at`),
    CONSTRAINT `fk_chat_messages_thread`
        FOREIGN KEY (`thread_id`) REFERENCES `private_chats` (`thread_id`),
    CONSTRAINT `fk_chat_messages_sender`
        FOREIGN KEY (`sender_user_id`) REFERENCES `users` (`user_id`),
    CONSTRAINT `chk_chat_messages_type` CHECK (`message_type` IN (0, 1, 3)),
    CONSTRAINT `chk_chat_messages_status` CHECK (`status` IN (0, 1, 2)),
    CONSTRAINT `chk_chat_messages_content` CHECK (
        (`message_type` = 0 AND `text_content` IS NOT NULL) OR
        (`message_type` IN (1, 3) AND `text_content` IS NULL)
    )
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE `message_resources` (
    `message_id` BIGINT UNSIGNED NOT NULL,
    `original_file_name` VARCHAR(255) NOT NULL,
    `file_size_bytes` BIGINT UNSIGNED NOT NULL,
    `sha256` CHAR(64) NOT NULL,
    `mime_type` VARCHAR(128) NOT NULL,
    PRIMARY KEY (`message_id`),
    CONSTRAINT `fk_message_resources_message`
        FOREIGN KEY (`message_id`) REFERENCES `chat_messages` (`message_id`)
        ON DELETE CASCADE,
    CONSTRAINT `chk_message_resources_size` CHECK (`file_size_bytes` > 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE `friend_requests` (
    `friend_request_id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `requester_user_id` BIGINT UNSIGNED NOT NULL,
    `target_user_id` BIGINT UNSIGNED NOT NULL,
    `client_request_id` VARCHAR(64) NOT NULL,
    `request_message` VARCHAR(255) NOT NULL DEFAULT '',
    `status` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `thread_id` BIGINT UNSIGNED NULL,
    `created_at` DATETIME(3) NOT NULL,
    PRIMARY KEY (`friend_request_id`),
    UNIQUE KEY `uk_friend_requests_client` (`requester_user_id`, `client_request_id`),
    KEY `idx_friend_requests_target` (`target_user_id`, `status`, `created_at`),
    KEY `idx_friend_requests_requester` (`requester_user_id`, `status`, `created_at`),
    CONSTRAINT `fk_friend_requests_requester`
        FOREIGN KEY (`requester_user_id`) REFERENCES `users` (`user_id`),
    CONSTRAINT `fk_friend_requests_target`
        FOREIGN KEY (`target_user_id`) REFERENCES `users` (`user_id`),
    CONSTRAINT `fk_friend_requests_thread`
        FOREIGN KEY (`thread_id`) REFERENCES `private_chats` (`thread_id`),
    CONSTRAINT `chk_friend_requests_users` CHECK (`requester_user_id` <> `target_user_id`),
    CONSTRAINT `chk_friend_requests_status` CHECK (`status` IN (0, 1, 2))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

CREATE TABLE `user_events` (
    `recipient_user_id` BIGINT UNSIGNED NOT NULL,
    `event_seq` BIGINT UNSIGNED NOT NULL,
    `event_type` TINYINT UNSIGNED NOT NULL,
    `message_id` BIGINT UNSIGNED NULL,
    `friend_request_id` BIGINT UNSIGNED NULL,
    PRIMARY KEY (`recipient_user_id`, `event_seq`),
    UNIQUE KEY `uk_user_events_message`
        (`recipient_user_id`, `event_type`, `message_id`),
    UNIQUE KEY `uk_user_events_friend`
        (`recipient_user_id`, `event_type`, `friend_request_id`),
    CONSTRAINT `fk_user_events_recipient`
        FOREIGN KEY (`recipient_user_id`) REFERENCES `users` (`user_id`),
    CONSTRAINT `fk_user_events_message`
        FOREIGN KEY (`message_id`) REFERENCES `chat_messages` (`message_id`),
    CONSTRAINT `fk_user_events_friend_request`
        FOREIGN KEY (`friend_request_id`) REFERENCES `friend_requests` (`friend_request_id`),
    CONSTRAINT `chk_user_events_type`
        CHECK (`event_type` IN (0, 1, 3, 10, 11, 12)),
    CONSTRAINT `chk_user_events_reference` CHECK (
        (`event_type` IN (0, 1, 3) AND `message_id` IS NOT NULL AND `friend_request_id` IS NULL) OR
        (`event_type` IN (10, 11, 12) AND `message_id` IS NULL AND `friend_request_id` IS NOT NULL)
    )
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;

SET FOREIGN_KEY_CHECKS = 1;
