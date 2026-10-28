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

        matches("initial_")
        connection.begin()
        try:
            cursor.execute("UPDATE docs SET content=%s, content_en=%s WHERE id=1",
                           ("OceanBase 全文索引 分词器 database", "database performance audit audit"))
            cursor.execute("DELETE FROM docs WHERE id=2")
            cursor.execute("INSERT INTO docs(category,title,content,title_en,content_en) "
                           "VALUES(1,'全文索引','倒排索引 tokenizer','Database audit','database performance')")
            matches("transaction_")
        finally:
            connection.rollback()
        matches("rollback_")
        for name in queries:
            assert results["initial_" + name] == results["rollback_" + name]
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
