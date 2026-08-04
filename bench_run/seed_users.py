# seed_users.py — bulk-insert bench users so bench_im.py can scale connections.
# Password is stored plaintext (matches existing bench users; see MysqlDao::CheckPwd).
# Usage: python seed_users.py <from_n> <to_n>   (inserts bench<from_n>..bench<to_n>, uid=90000+n)
import sys, pymysql

from_n = int(sys.argv[1]) if len(sys.argv) > 1 else 1201
to_n   = int(sys.argv[2]) if len(sys.argv) > 2 else 10000

c = pymysql.connect(host="127.0.0.1", port=3308, user="root",
                    password="123456.", database="llfc", charset="utf8mb4")
cur = c.cursor()
cur.execute("SELECT COUNT(*) FROM user WHERE email LIKE 'bench%@bench.local'")
print("bench users before:", cur.fetchone()[0])

rows = [(90000 + n, f"bench{n}", f"bench{n}@bench.local", "123456", "", "", 0, "")
        for n in range(from_n, to_n + 1)]
# `desc` is a MySQL reserved word -> backtick it.
cur.executemany(
    "INSERT IGNORE INTO user (uid,name,email,pwd,nick,`desc`,sex,icon) "
    "VALUES (%s,%s,%s,%s,%s,%s,%s,%s)", rows)
c.commit()

cur.execute("SELECT COUNT(*) FROM user WHERE email LIKE 'bench%@bench.local'")
print("bench users after:", cur.fetchone()[0])
cur.execute("SELECT MIN(uid),MAX(uid) FROM user WHERE email LIKE 'bench%@bench.local'")
print("uid range:", cur.fetchone())
c.close()
