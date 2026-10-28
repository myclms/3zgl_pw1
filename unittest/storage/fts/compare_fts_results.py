#!/usr/bin/env python3
"""Compare complete FTS outputs on an isolated fts_large_bench instance.

Run the unchanged official benchmark first, then capture a baseline with
--output before.json. After rebuilding and rerunning the benchmark, use
--reference before.json --output after.json. Timing is not measured here.
"""
import argparse
import json
from pathlib import Path

import pymysql


def snapshot(connection):
    results = {}
    with connection.cursor() as cursor:
        cursor.execute("SELECT COUNT(*) FROM docs")
        if cursor.fetchone()[0] != 20000:
            raise AssertionError("Run the official 20000-row benchmark on an isolated instance first")
        cursor.execute("SELECT title, content, title_en, content_en FROM docs ORDER BY id LIMIT 100")
        documents = cursor.fetchall()
        for parser in ("ik", "beng", "space", "ngram"):
            configurations = ['[{"output":"all"}]']
            if parser == "ik":
                configurations.append('[{"output":"all"},{"additional_args":[{"ik_mode":"max_word"}]}]')
            for configuration in configurations:
                tokens = []
                for row in documents:
                    for document in row:
                        cursor.execute("SELECT TOKENIZE(%s, %s, %s) AS tokens", (document, parser, configuration))
                        tokens.append(json.loads(cursor.fetchone()[0]))
                # Array order, token spellings, frequencies and doc_len all matter.
                results[parser + configuration] = tokens

        # Configuration values must be decoded afresh for each row/call.
        configurations = [
            ("space", [{"min_token_size": 1}, {"max_token_size": 10}]),
            ("beng", [{"min_token_size": 3}, {"max_token_size": 12}]),
            ("ngram", [{"ngram_token_size": 1}]),
            ("ngram", [{"ngram_token_size": 3}]),
            ("ngram2", [{"min_ngram_size": 1}, {"max_ngram_size": 3}]),
            ("ik", [{"ik_mode": "smart"}]),
            ("ik", [{"ik_mode": "max_word"}]),
        ]
        results["configuration"] = []
        for parser, properties in configurations:
            options = json.dumps([{"output": "all"}, {"additional_args": properties}])
            cursor.execute("SELECT TOKENIZE(%s, %s, %s) AS tokens",
                           ("数据库 database AUDIT audit a café", parser, options))
            results["configuration"].append(json.loads(cursor.fetchone()[0]))
        results["invalid_configuration"] = []
        for parser, properties in (
            ("space", [{"min_token_size": 0}]),
            ("ngram", [{"ngram_token_size": 11}]),
            ("ik", [{"ik_mode": "invalid"}]),
            ("beng", [{"unsupported": 1}]),
        ):
            try:
                cursor.execute("SELECT TOKENIZE(%s, %s, %s) AS tokens",
                               ("database", parser, json.dumps([{"additional_args": properties}])))
            except pymysql.MySQLError as error:
                results["invalid_configuration"].append(error.args[0])
            else:
                raise AssertionError("Invalid parser configuration was accepted")
        results["defaults"] = []
        for parser in ("ik", "beng", "space", "ngram", "ngram2"):
            for document in (None, "", "!!! --", "数据库 database AUDIT audit café"):
                cursor.execute("SELECT TOKENIZE(CAST(%s AS CHAR CHARACTER SET utf8mb4), %s) AS tokens",
                               (document, parser))
                results["defaults"].append(json.loads(cursor.fetchone()[0]))
        cursor.execute("SELECT TOKENIZE(title, CASE WHEN MOD(id,2)=0 THEN 'beng' ELSE 'ik' END, "
                       "CASE WHEN MOD(id,3)=0 THEN '[{\"output\":\"all\"}]' ELSE '[{}]' END) AS tokens "
                       "FROM docs ORDER BY id LIMIT 100")
        results["dynamic_configuration"] = [json.loads(row[0]) for row in cursor.fetchall()]

        queries = {
            "cn": ("title, content", "全文索引 分词器"),
            "beng": ("title_en, content_en", "database performance audit"),
            "mixed": ("title, content", "OceanBase 稳定 database"),
            "content": ("content", "倒排索引 tokenizer"),
        }

        def matches(label):
            for name, (columns, text) in queries.items():
                cursor.execute(
                    f"SELECT id, MATCH({columns}) AGAINST(%s) AS relevance FROM docs "
                    f"WHERE MATCH({columns}) AGAINST(%s) ORDER BY id", (text, text))
                results[label + name] = [[row[0], str(row[1])] for row in cursor.fetchall()]
            score = "MATCH(title, content) AGAINST('全文索引 分词器')"
            for name, condition in {
                "score_filter": f"{score} > 0.1",
                "compound": f"{score} AND MOD(id,3)=0",
                "repeated": f"{score} AND ({score} > 0)",
                "empty": "MATCH(content) AGAINST('nonexistentuniqueterm')",
            }.items():
                cursor.execute(f"SELECT id, {score} AS relevance FROM docs WHERE {condition} ORDER BY id")
                results[label + name] = [[row[0], str(row[1])] for row in cursor.fetchall()]
            cursor.execute(f"SELECT id, {score} AS relevance FROM docs WHERE {score} "
                           "ORDER BY relevance DESC, id LIMIT 75 OFFSET 15")
            results[label + "ranked"] = [[row[0], str(row[1])] for row in cursor.fetchall()]
            cursor.execute(f"SELECT id, {score} AS relevance FROM docs WHERE {score} LIMIT 20")
            results[label + "limited"] = [[row[0], str(row[1])] for row in cursor.fetchall()]
            cursor.execute("SELECT id, MATCH(title_en, content_en) "
                           "AGAINST('+database -audit' IN BOOLEAN MODE) AS relevance FROM docs "
                           "WHERE MATCH(title_en, content_en) AGAINST('+database -audit' IN BOOLEAN MODE) "
                           "ORDER BY id")
            results[label + "boolean"] = [[row[0], str(row[1])] for row in cursor.fetchall()]

        matches("initial_")
        connection.begin()
        try:
            cursor.execute("UPDATE docs SET content=%s, content_en=%s WHERE id=1",
                           ("OceanBase 全文索引 分词器 database", "database performance audit audit"))
            cursor.execute("DELETE FROM docs WHERE id=2")
            cursor.execute("INSERT INTO docs(id,category,title,content,title_en,content_en) "
                           "VALUES(30001,1,'全文索引','倒排索引 tokenizer','Database audit','database performance')")
            matches("transaction_")
        finally:
            connection.rollback()
        matches("rollback_")
        for key in results:
            if key.startswith("initial_"):
                assert results[key] == results[key.replace("initial_", "rollback_", 1)]
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--reference", type=Path)
    args = parser.parse_args()
    connection = pymysql.connect(unix_socket=args.socket, user="root", database="fts_large_bench",
                                 charset="utf8mb4", autocommit=True, read_timeout=180)
    try:
        with connection.cursor() as cursor:
            cursor.execute("SET ob_query_timeout=120000000")
        actual = snapshot(connection)
    finally:
        connection.close()
    args.output.write_text(json.dumps(actual, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if args.reference:
        expected = json.loads(args.reference.read_text(encoding="utf-8"))
        assert expected.keys() == actual.keys(), "Snapshot sections changed"
        for key in actual:
            assert expected[key] == actual[key], f"FTS output differs: {key}"
        print("All token arrays, word frequencies, document lengths, matching IDs and scores agree")
    else:
        print("Baseline FTS outputs captured")


if __name__ == "__main__":
    main()
