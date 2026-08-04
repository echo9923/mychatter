-- ============================================================================
-- 迁移：移除 uid 发号器表 user_id（方案A）
-- 背景：uid 不再由独立发号器生成。注册时直接使用 user 表自增主键 id 作为
--       对外 uid：INSERT 时省略 uid 列（DEFAULT 0），随后经 LAST_INSERT_ID()
--       取到新主键，再 UPDATE 回填 uid = id。
-- 影响：
--   1. 重写存储过程 reg_user，去除对 user_id 表的依赖（保持签名不变，
--      仍输出 result：>0 新uid，0 用户已存在，-1 异常）。
--   2. 删除 user_id 表。
-- 可重复执行：DROP/CREATE 均幂等，重复执行无副作用。
-- 注意：旧用户 uid（发号器分配的连续小数字）与新用户 uid（自增主键 id）
--       并存，互不冲突，无需数据迁移。
-- ============================================================================

-- ----------------------------------------------------------------------------
-- 1. 重写存储过程 reg_user：不再依赖 user_id 发号器
-- ----------------------------------------------------------------------------
DROP PROCEDURE IF EXISTS `reg_user`;
delimiter ;;
CREATE PROCEDURE `reg_user`(IN `new_name` VARCHAR(255),
    IN `new_email` VARCHAR(255),
    IN `new_pwd` VARCHAR(255),
    OUT `result` INT)
BEGIN
    -- 执行过程中遇到任何错误则回滚事务
    DECLARE EXIT HANDLER FOR SQLEXCEPTION
    BEGIN
        ROLLBACK;
        SET result = -1;
    END;

    -- 开始事务
    START TRANSACTION;

    -- 检查用户名是否已存在
    IF EXISTS (SELECT 1 FROM `user` WHERE `name` = new_name) THEN
        SET result = 0; -- 用户名已存在
        COMMIT;
    ELSE
        -- 用户名不存在，检查email是否已存在
        IF EXISTS (SELECT 1 FROM `user` WHERE `email` = new_email) THEN
            SET result = 0; -- email已存在
            COMMIT;
        ELSE
            -- email也不存在：插入用户记录（uid 列省略，走 DEFAULT 0）
            INSERT INTO `user` (`name`, `email`, `pwd`) VALUES (new_name, new_email, new_pwd);

            -- 取本次插入的自增主键 id（同一会话内 LAST_INSERT_ID() 安全）
            SELECT LAST_INSERT_ID() INTO @new_id;

            -- 回填 uid = id（方案A：uid 直接等于自增主键，不再使用发号器）
            UPDATE `user` SET `uid` = @new_id WHERE `id` = @new_id;

            -- 返回新 uid
            SET result = @new_id;
            COMMIT;
        END IF;
    END IF;
END
;;
delimiter ;

-- ----------------------------------------------------------------------------
-- 2. 删除 uid 发号器表
-- ----------------------------------------------------------------------------
DROP TABLE IF EXISTS `user_id`;
