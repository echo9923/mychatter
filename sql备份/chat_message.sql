/*
 Navicat Premium Data Transfer

 Source Server         : llfcmysql
 Source Server Type    : MySQL
 Source Server Version : 80027 (8.0.27)
 Source Host           : 81.68.86.146:3308
 Source Schema         : llfc

 Target Server Type    : MySQL
 Target Server Version : 80027 (8.0.27)
 File Encoding         : 65001

 Date: 24/07/2025 18:01:29
*/

SET NAMES utf8mb4;
SET FOREIGN_KEY_CHECKS = 0;

-- ----------------------------
-- Table structure for chat_message
-- ----------------------------
DROP TABLE IF EXISTS `chat_message`;
CREATE TABLE `chat_message`  (
  `message_id` bigint UNSIGNED NOT NULL AUTO_INCREMENT,
  `thread_id` bigint UNSIGNED NOT NULL,
  `sender_id` bigint UNSIGNED NOT NULL,
  `recv_id` bigint UNSIGNED NOT NULL,
  `content` text CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci NOT NULL,
  `created_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  `status` tinyint NOT NULL DEFAULT 0 COMMENT '0=未读 1=发送失败 2=已读 3=资源未上传完成',
  `msg_type` tinyint NOT NULL DEFAULT 0 COMMENT '0=文本 1=图片 2=视频 3=文件',
  `unique_id` varchar(64) CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci NULL DEFAULT NULL COMMENT '客户端去重标识，历史/系统消息为NULL',
  `content_size` bigint UNSIGNED NOT NULL DEFAULT 0 COMMENT '文本为0，图片为字节数',
  `delivery_status` tinyint NOT NULL DEFAULT 0 COMMENT '0=待投递 1=已投递(ACK)',
  PRIMARY KEY (`message_id`) USING BTREE,
  UNIQUE INDEX `uk_chat_message_sender_unique`(`sender_id` ASC, `unique_id` ASC) USING BTREE,
  INDEX `idx_thread_created`(`thread_id` ASC, `created_at` ASC) USING BTREE,
  INDEX `idx_thread_message`(`thread_id` ASC, `message_id` ASC) USING BTREE,
  INDEX `idx_chat_message_pending`(`recv_id` ASC, `delivery_status` ASC, `message_id` ASC) USING BTREE
) ENGINE = InnoDB AUTO_INCREMENT = 35 CHARACTER SET = utf8mb4 COLLATE = utf8mb4_unicode_ci ROW_FORMAT = Dynamic;

SET FOREIGN_KEY_CHECKS = 1;
