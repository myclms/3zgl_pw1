#!/usr/bin/env python3
"""Compare cardinality-only limited MATCH with the unchanged reference server.

Uses a dedicated database. Capture --output on the reference, then run with
--reference and --expect-elision on the candidate. No benchmark SQL is changed.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from decimal import Decimal
import json
from pathlib import Path

import pymysql


def capture(connection, expect_elision):
    results = {}
    with connection.cursor() as cursor:
        cursor.execute("CREATE DATABASE IF NOT EXISTS fts_cardinality_regression")
        cursor.execute("USE fts_cardinality_regression")
        for table, primary in (("hidden_ids", ""), ("primary_ids", ", PRIMARY KEY(id)")):
            cursor.execute(f"DROP TABLE IF EXISTS {table}")
            cursor.execute(f"CREATE TABLE {table}(id BIGINT, v INT, body TEXT{primary}, "
                           "FULLTEXT KEY body_idx(body) WITH PARSER beng)")
            rows = []
            for i in range(1, 81):
                body = "orbit " * (1 + i % 7) if i % 3 == 0 else "galaxy "
                if i % 5 == 0:
                    body += "nebula " * (1 + i % 4)
                if i % 17 == 0:
                    body = None
                rows.append((i, None if i % 4 == 0 else i % 9, body))
            cursor.executemany(f"INSERT INTO {table} VALUES(%s,%s,%s)", rows)
            for query in ("orbit nebula", "orbit orbit", "the with", "", None):
                score = "MATCH(body) AGAINST(%s)"
                cursor.execute(f"SELECT COUNT(*) FROM {table} WHERE {score}", (query,))
                total = cursor.fetchone()[0]
                for limit, offset in ((0, 0), (1, 0), (7, 0), (20, 3), (100, 0), (5, 100)):
                    sql = (f"SELECT COUNT(*) FROM (SELECT id,v FROM {table} WHERE {score} "
                           f"LIMIT {limit} OFFSET {offset}) t")
                    cursor.execute(sql, (query,))
                    actual = cursor.fetchone()[0]
                    assert actual == min(limit, max(0, total - offset)), (sql, query, actual, total)
                    results[f"{table}/{query}/{limit}/{offset}"] = actual

            score = "MATCH(body) AGAINST('orbit nebula')"
            child = f"SELECT id,v FROM {table} WHERE {score} LIMIT 7 OFFSET 3"
            controls = {
                "nullable_count": f"SELECT COUNT(v) FROM ({child}) t",
                "distinct_count": f"SELECT COUNT(DISTINCT v) FROM ({child}) t",
                "sum": f"SELECT SUM(id) FROM ({child}) t",
                "outer_filter": f"SELECT COUNT(*) FROM ({child}) t WHERE v IS NULL",
                "outer_group": f"SELECT v,COUNT(*) FROM ({child}) t GROUP BY v ORDER BY v",
                "outer_join": f"SELECT COUNT(*) FROM ({child}) t JOIN {table} b ON t.id=b.id WHERE b.v=1",
                "explicit_rank": f"SELECT COUNT(*) FROM (SELECT id FROM {table} WHERE {score} ORDER BY {score} DESC,id LIMIT 7) t",
                "distinct_child": f"SELECT COUNT(*) FROM (SELECT DISTINCT v FROM {table} WHERE {score} LIMIT 7) t",
                "numeric_filter": f"SELECT COUNT(*) FROM (SELECT id FROM {table} WHERE {score}>0.04 LIMIT 7) t",
                "extra_filter": f"SELECT COUNT(*) FROM (SELECT id FROM {table} WHERE {score} AND v IS NULL LIMIT 7) t",
                "ranked_ids": f"SELECT id FROM {table} WHERE {score} LIMIT 7 OFFSET 3",
                "numeric_score": f"SELECT id,{score} FROM {table} WHERE {score} ORDER BY {score} DESC,id LIMIT 7",
                "ordinary_ids": f"SELECT id FROM {table} WHERE {score} ORDER BY id",
            }
            for name, sql in controls.items():
                cursor.execute(sql)
                results[f"{table}/{name}"] = [
                    [str(value) if isinstance(value, (Decimal, float)) else value for value in row]
                    for row in cursor.fetchall()]
            if expect_elision:
                optimized = f"SELECT COUNT(*) FROM ({child}) t"
                for sql, score_needed in ((optimized, False),
                                          (controls["nullable_count"], True),
                                          (controls["distinct_count"], True),
                                          (controls["outer_filter"], True),
                                          (controls["ranked_ids"], True),
                                          (controls["explicit_rank"], True),
                                          (f"SELECT COUNT(*) FROM (SELECT id FROM {table} "
                                           f"WHERE {score} AND RAND(73)>=0 LIMIT 7) t", True),
                                          (f"SELECT COUNT(*) FROM (SELECT LENGTH(body) FROM {table} "
                                           f"WHERE {score} LIMIT 7) t", True)):
                    cursor.execute("EXPLAIN EXTENDED " + sql)
                    plan = "\n".join(str(row[0]) for row in cursor.fetchall())
                    if score_needed:
                        assert "calc_relevance=false" not in plan, (sql, plan)
                    else:
                        assert "calc_relevance=false" in plan, (sql, plan)
                        assert "SORT" not in plan, (sql, plan)

            # Parameters and cached plans must not freeze the search, count or
            # offset. Include empty results and offsets beyond all matches.
            cursor.execute(f"PREPARE count_limit FROM 'SELECT COUNT(*) FROM (SELECT id "
                           f"FROM {table} WHERE MATCH(body) AGAINST(?) LIMIT ? OFFSET ?) t'")
            try:
                for index, parameters in enumerate((("orbit", 3, 0), ("nebula", 9, 2),
                                                     (None, 2, 0), ("galaxy", 5, 100),
                                                     ("orbit nebula", 20, 3))):
                    cursor.execute("SET @question=%s,@amount=%s,@start=%s", parameters)
                    cursor.execute("EXECUTE count_limit USING @question,@amount,@start")
                    results[f"{table}/prepared/{index}"] = cursor.fetchone()[0]
            finally:
                cursor.execute("DEALLOCATE PREPARE count_limit")
            connection.begin()
            try:
                cursor.execute(f"UPDATE {table} SET body='galaxy' WHERE MOD(id,3)=0")
                cursor.execute(f"DELETE FROM {table} WHERE id<20")
                cursor.execute(f"INSERT INTO {table} VALUES(81,NULL,'orbit nebula')")
                cursor.execute(f"SELECT COUNT(*) FROM ({child}) t")
                results[f"{table}/transaction"] = cursor.fetchone()[0]
            finally:
                connection.rollback()
            cursor.execute(f"SELECT COUNT(*) FROM ({child}) t")
            results[f"{table}/rollback"] = cursor.fetchone()[0]
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--expect-elision", action="store_true")
    args = parser.parse_args()
    def connect():
        return pymysql.connect(unix_socket=args.socket, user="root", charset="utf8mb4",
                               autocommit=True, read_timeout=180)
    connection = connect()
    try:
        actual = capture(connection, args.expect_elision)
    finally:
        connection.close()
    def worker(_):
        connection = connect()
        try:
            with connection.cursor() as cursor:
                for _ in range(20):
                    cursor.execute("SELECT COUNT(*) FROM (SELECT id FROM "
                                   "fts_cardinality_regression.primary_ids WHERE MATCH(body) "
                                   "AGAINST('orbit nebula') LIMIT 7 OFFSET 3) t")
                    assert cursor.fetchone()[0] == actual["primary_ids/rollback"]
        finally:
            connection.close()
    with ThreadPoolExecutor(max_workers=8) as workers:
        list(workers.map(worker, range(8)))
    args.output.write_text(json.dumps(actual, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if args.reference:
        expected = json.loads(args.reference.read_text(encoding="utf-8"))
        assert actual == expected, [key for key in actual if actual[key] != expected.get(key)]
        print(f"All {len(actual)} limited-count and observable-ranking results agree; 160 concurrent queries passed")
    else:
        print(f"Captured {len(actual)} reference results; 160 concurrent queries passed")


if __name__ == "__main__":
    main()
