#!/usr/bin/env python3

import timeit
import sys

import lbug

query = sys.stdin.read()
print(query)

db = lbug.Database(":memory:")
con = lbug.Connection(db)
ver = con.execute("call DB_Version() return *;").get_next()[0]
db = lbug.Database(f"mydb-{ver}", read_only=True)
con = lbug.Connection(db)
for try_num in range(3):
    start = timeit.default_timer()
    results = con.execute(query.replace('\\', '\\\\'))
    end = timeit.default_timer()
    print(end - start)
