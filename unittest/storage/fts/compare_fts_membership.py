#!/usr/bin/env python3
"""Compare score-free MATCH membership with an unmodified server binary.

Run first with --output on the reference, then with --reference on the
candidate. --expect-elision additionally checks the candidate's real plans.
This creates only the dedicated fts_membership_regression test database.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path

import pymysql


def capture(connection, expect_elision):
    results = {}
    with connection.cursor() as cursor:
        cursor.execute("CREATE DATABASE IF NOT EXISTS fts_membership_regression")
        cursor.execute("USE fts_membership_regression")
        for table, primary in (("hidden_ids", ""), ("primary_ids", ", PRIMARY KEY(id)")):
            cursor.execute(f"DROP TABLE IF EXISTS {table}")
            cursor.execute(f"CREATE TABLE {table}(id BIGINT, body TEXT{primary}, "
                           "FULLTEXT KEY body_idx(body) WITH PARSER beng)")
            cursor.executemany(f"INSERT INTO {table}(id,body) VALUES(%s,%s)", [
                (1, "orbit orbit nebula telescope"), (2, "galaxy orbit cosmic"),
                (3, "quasar nebula"), (4, "the with a"), (5, ""), (6, None),
                (7, "telescope galaxy"), (8, "nebula nova"),
                (9, "ORBIT, nebula! galaxy"), (10, "stellar other"),
            ])
            match = "MATCH(body) AGAINST(%s)"
            # 320 distinct terms force the existing TAAT fallback (>256).
            many_terms = " ".join("lexeme" + chr(97 + i // 26) + chr(97 + i % 26)
                                  for i in range(320)) + " orbit nebula"
            questions = ["orbit nebula", "orbit orbit nebula", "ORBIT", "the with",
                         "notpresentword", "", None, many_terms]
            for index, question in enumerate(questions):
                for label, sql, parameters in (
                    ("ids", f"SELECT id FROM {table} WHERE {match} ORDER BY id", (question,)),
                    ("descending", f"SELECT id FROM {table} WHERE {match} ORDER BY id DESC", (question,)),
                    ("count", f"SELECT COUNT(*) FROM {table} WHERE {match}", (question,)),
                    ("compound", f"SELECT id FROM {table} WHERE {match} AND MOD(id,2)=0 ORDER BY id", (question,)),
                    ("numeric", f"SELECT id,{match} FROM {table} WHERE {match} ORDER BY id", (question, question)),
                    ("threshold", f"SELECT id FROM {table} WHERE {match}>0.03 ORDER BY id", (question,)),
                    ("ranked", f"SELECT id FROM {table} WHERE {match} ORDER BY {match} DESC,id LIMIT 5", (question, question)),
                    ("boolean", f"SELECT id FROM {table} WHERE MATCH(body) AGAINST(%s IN BOOLEAN MODE) ORDER BY id", ("+orbit -nebula",)),
                ):
                    # Boolean mode intentionally keeps its original token limit.
                    cursor.execute(sql, parameters)
                    results[f"{table}/{index}/{label}"] = [
                        [str(value) if isinstance(value, float) else value for value in row]
                        for row in cursor.fetchall()
                    ]
            score = "MATCH(body) AGAINST('orbit nebula')"
            for label, sql in (
                ("group", f"SELECT MOD(id,2),COUNT(*) FROM {table} WHERE {score} GROUP BY MOD(id,2) ORDER BY 1"),
                ("arithmetic", f"SELECT id,{score}+1 FROM {table} WHERE {score} ORDER BY id"),
                ("negative", f"SELECT id FROM {table} WHERE NOT({score}) ORDER BY id"),
                ("outer", f"SELECT COUNT(*) FROM (SELECT id FROM {table} WHERE {score} LIMIT 3) t"),
                ("join", f"SELECT a.id FROM {table} a JOIN {table} b ON a.id=b.id WHERE MATCH(a.body) AGAINST('orbit nebula') ORDER BY a.id"),
            ):
                cursor.execute(sql)
                results[f"{table}/{label}"] = [[str(v) if isinstance(v, float) else v for v in row]
                                                for row in cursor.fetchall()]
            cursor.execute(f"PREPARE membership_stmt FROM 'SELECT id FROM {table} "
                           "WHERE MATCH(body) AGAINST(?) ORDER BY id'")
            try:
                for index, question in enumerate(("orbit", "galaxy", "", None, "nebula", many_terms)):
                    cursor.execute("SET @membership_query=%s", (question,))
                    cursor.execute("EXECUTE membership_stmt USING @membership_query")
                    results[f"{table}/prepared/{index}"] = [list(row) for row in cursor.fetchall()]
            finally:
                cursor.execute("DEALLOCATE PREPARE membership_stmt")
            # Verify the exact plan property; query success alone is insufficient
            # to prove that the intended optimization is exercised.
            if expect_elision:
                for sql, expected in (
                    (f"SELECT id FROM {table} WHERE {score} ORDER BY id", "false"),
                    (f"SELECT COUNT(*) FROM {table} WHERE {score}", "false"),
                    # With no explicit order the resolver adds relevance order.
                    (f"SELECT id FROM {table} WHERE {score}", "true"),
                    (f"SELECT id FROM {table} WHERE {score} AND MOD(id,2)=0 ORDER BY id", "true"),
                    (f"SELECT id,{score} FROM {table} WHERE {score}", "true"),
                    (f"SELECT id FROM {table} WHERE {score}>0.03", "true"),
                    (f"SELECT id FROM {table} WHERE {score} ORDER BY {score} DESC", "true"),
                ):
                    cursor.execute("EXPLAIN EXTENDED " + sql)
                    plan = "\n".join(" ".join(str(v) for v in row) for row in cursor.fetchall())
                    if expected == "false":
                        assert "calc_relevance=false" in plan, (sql, plan)
                    else:
                        # A full scan evaluates MATCH by functional lookup;
                        # this path must also keep exact scores.
                        assert "calc_relevance=false" not in plan, (sql, plan)
                        assert ("calc_relevance=true" in plan
                                or "has_functional_lookup=true" in plan), (sql, plan)
            connection.begin()
            try:
                cursor.execute(f"UPDATE {table} SET body='galaxy changed' WHERE id=1")
                cursor.execute(f"DELETE FROM {table} WHERE id=3")
                cursor.execute(f"INSERT INTO {table} VALUES(11,'orbit nova')")
                cursor.execute(f"SELECT id FROM {table} WHERE {score} ORDER BY id")
                results[f"{table}/transaction"] = [list(row) for row in cursor.fetchall()]
            finally:
                connection.rollback()
            cursor.execute(f"SELECT id FROM {table} WHERE {score} ORDER BY id")
            results[f"{table}/rollback"] = [list(row) for row in cursor.fetchall()]
            assert results[f"{table}/rollback"] == results[f"{table}/0/ids"]
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--reference", type=Path)
    parser.add_argument("--expect-elision", action="store_true")
    args = parser.parse_args()
    connection = pymysql.connect(unix_socket=args.socket, user="root", charset="utf8mb4",
                                 autocommit=True, read_timeout=180)
    try:
        with connection.cursor() as cursor:
            cursor.execute("SET ob_query_timeout=120000000")
        actual = capture(connection, args.expect_elision)
    finally:
        connection.close()
    # Exercise shared plan-cache entries from separate SQL workers, especially
    # the primary-key lookup with a filter that can remove intermediate rows.
    def query_worker(_):
        worker = pymysql.connect(unix_socket=args.socket, user="root", charset="utf8mb4",
                                 database="fts_membership_regression", autocommit=True)
        try:
            with worker.cursor() as cursor:
                for _ in range(20):
                    cursor.execute("SELECT id FROM primary_ids WHERE MATCH(body) "
                                   "AGAINST('orbit nebula') AND MOD(id,2)=0 ORDER BY id")
                    assert [list(row) for row in cursor.fetchall()] == actual["primary_ids/0/compound"]
        finally:
            worker.close()
    with ThreadPoolExecutor(max_workers=8) as workers:
        list(workers.map(query_worker, range(8)))
    args.output.write_text(json.dumps(actual, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if args.reference:
        expected = json.loads(args.reference.read_text(encoding="utf-8"))
        assert actual.keys() == expected.keys()
        for key in actual:
            assert actual[key] == expected[key], f"FTS membership/score differs: {key}"
        print(f"All {len(actual)} membership, exact-score, prepared, DML and fallback results agree")
    else:
        print(f"Captured {len(actual)} reference result sets")


if __name__ == "__main__":
    main()
