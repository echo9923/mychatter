-- 20260804: PBKDF2 password-hashing migration.
--
-- 应用代码现在只向 pwd 列写入自描述哈希串（pbkdf2-sha256$i=<iter>$<salt>$<dk>），
-- 不再写入明文。存量明文行保持不变，首次成功登录时由 rehash-on-login 平滑升级。
--
-- 本迁移只是预防性加固：把 pwd 列的排序规则改为大小写敏感（utf8mb4_bin）。
-- App 代码从不执行 WHERE pwd = ? 或对 pwd 建唯一索引，所以这是可选步骤；
-- 目的是防止未来任何 SQL 直接比较 pwd 时被大小写不敏感语义坑到。
-- 保持 CHARACTER SET utf8mb4，确保存量非 ASCII 明文/哈希都能存活。
--
-- 注意：ALTER TABLE ... MODIFY 会重建表，请在低峰期执行。

ALTER TABLE `user`
  MODIFY `pwd` VARCHAR(255) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL DEFAULT '';
