#!/usr/bin/env python3
"""Native SQL regressions beyond the competition's five AI-column cases.

Requires a running local seekdb and PyMySQL. The mock is the same deterministic
service used by the mysqltest suite; no external model or credentials are used.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time

def main():
    import pymysql

    parser = argparse.ArgumentParser()
    parser.add_argument('--socket', required=True)
    parser.add_argument('--mock-port', type=int, default=19125)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    mock = repo / 'tools/deploy/mysql_test/test_suite/ai_column/include/ai_column_mock.py'
    database = 'ai_column_lifecycle_regression'
    model = 'ai_column_lifecycle_model'
    endpoint = model + '_ep'
    embedding_model = model + '_embed'
    embedding_endpoint = embedding_model + '_ep'
    connection = pymysql.connect(unix_socket=args.socket, user='root', database='oceanbase',
                                 charset='utf8mb4', autocommit=True, read_timeout=30)


    def execute(sql, parameters=None):
        with connection.cursor() as cursor:
            cursor.execute(sql, parameters)
            rows = cursor.fetchall() if cursor.description else ()
            while cursor.nextset():
                pass
            return rows


    def check(label, condition):
        if not condition:
            raise AssertionError(label)
        print('PASS', label, flush=True)


    def fails(label, sql, code):
        try:
            execute(sql)
        except pymysql.MySQLError as error:
            check(label, error.args[0] == code)
        else:
            raise AssertionError('expected SQL error: ' + label)


    def wait_for(label, query, expected, seconds=20):
        deadline = time.monotonic() + seconds
        actual = None
        while time.monotonic() < deadline:
            actual = execute(query)
            if actual == expected:
                check(label, True)
                return
            time.sleep(0.05)
        raise AssertionError(f'{label}: expected {expected!r}, got {actual!r}')


    def drop_models():
        for statement, name in [('DROP_AI_MODEL_ENDPOINT', endpoint),
                                ('DROP_AI_MODEL_ENDPOINT', embedding_endpoint),
                                ('DROP_AI_MODEL', model), ('DROP_AI_MODEL', embedding_model)]:
            try:
                execute(f'CALL DBMS_AI_SERVICE.{statement}(%s)', (name,))
            except pymysql.MySQLError as error:
                if error.args[0] not in (11112, 11118):
                    raise


    def task_count(table):
        return ("SELECT count(*) FROM oceanbase.__all_ai_column_task q "
                "JOIN oceanbase.__all_table t ON t.table_id=q.table_id "
                "JOIN oceanbase.__all_database d ON d.database_id=t.database_id "
                f"WHERE d.database_name='{database}' AND t.table_name='{table}'")


    try:
        subprocess.run([sys.executable, str(mock), 'start', str(args.mock_port)], check=True)
        execute(f'DROP DATABASE IF EXISTS {database}')
        drop_models()
        execute(f'CREATE DATABASE {database}')
        execute(f'USE {database}')
        execute('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
                (model, json.dumps({'type': 'completion', 'model_name': 'fixture-model'})))
        execute('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',
                (endpoint, json.dumps({'ai_model_name': model, 'provider': 'openai',
                    'url': f'http://127.0.0.1:{args.mock_port}/v1/chat/completions',
                    'access_key': 'fixture-key', 'request_model_name': 'fixture-model'})))
        execute('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
                (embedding_model, json.dumps({'type': 'dense_embedding', 'model_name': 'fixture-embed'})))
        execute('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',
                (embedding_endpoint, json.dumps({'ai_model_name': embedding_model, 'provider': 'openai',
                    'url': f'http://127.0.0.1:{args.mock_port}/v1/embeddings',
                    'access_key': 'fixture-key', 'request_model_name': 'fixture-embed'})))

        # AI_EMBED accepts a NULL input and returns a successful SQL NULL.
        execute(f"CREATE TABLE null_results(id BIGINT PRIMARY KEY,content VARCHAR(100),"
                f"result VECTOR(4) AI COLUMN(AI_EMBED('{embedding_model}',content)))")
        execute('INSERT INTO null_results(id,content) VALUES(1,NULL)')
        wait_for('successful NULL tasks finish', task_count('null_results'), ((0,),))
        execute('SET SESSION ai_column_strict_read=ON')
        check('successful NULL results are READY',
              execute('SELECT result IS NULL FROM null_results WHERE id=1') == ((1,),))
        execute('SET SESSION ai_column_strict_read=OFF')

        # The default connection collation adds implicit casts around model names.
        execute("SET collation_connection='utf8mb4_general_ci'")
        execute(f"CREATE TABLE docs(id BIGINT PRIMARY KEY,content VARCHAR(100),"
                "changed TIMESTAMP(6) DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),"
                f"result JSON AI COLUMN(AI_COMPLETE('{model}',content)),"
                f"label VARCHAR(200) AI COLUMN(AI_COMPLETE('{model}',content)))")
        execute("INSERT INTO docs(id,content,changed) VALUES(1,'first','2000-01-01 00:00:00')")
        wait_for('timestamp fixture is published', task_count('docs'), ((0,),))
        check('publication preserves business timestamp',
              execute("SELECT changed='2000-01-01 00:00:00' FROM docs WHERE id=1") == ((1,),))

        # Recreate the durable state left by an interrupted worker before commit.
        execute('START TRANSACTION')
        execute("INSERT INTO docs(id,content) VALUES(3,'recovered')")
        execute("UPDATE oceanbase.__all_ai_column_task SET state=1,owner='expired-worker',"
                "lease_until=DATE_SUB(NOW(6),INTERVAL 1 SECOND) WHERE table_id=("
                "SELECT t.table_id FROM oceanbase.__all_table t "
                "JOIN oceanbase.__all_database d ON d.database_id=t.database_id "
                f"WHERE d.database_name='{database}' AND t.table_name='docs') "
                "AND rowkey_predicate='`id` = 3'")
        execute('COMMIT')
        wait_for('expired leases recover the complete request group',
                 "SELECT json_unquote(json_extract(result,'$.input')),label FROM docs WHERE id=3",
                 (('recovered', 'mock:recovered'),))

        execute("INSERT INTO docs(id,content) VALUES(2,'Alpha')")
        wait_for('ordinary input is generated',
                 "SELECT json_unquote(json_extract(result,'$.input')) FROM docs WHERE id=2", (('Alpha',),))
        execute('START TRANSACTION')
        execute("UPDATE docs SET content='alpha' WHERE id=2")
        check('case-only input change invalidates both results',
              execute('SELECT result IS NULL AND label IS NULL FROM docs WHERE id=2') == ((1,),))
        check('case-only input change queues a group', execute(task_count('docs')) == ((2,),))
        execute('ROLLBACK')
        check('rollback restores generated value',
              execute("SELECT json_unquote(json_extract(result,'$.input')) FROM docs WHERE id=2") == (('Alpha',),))
        wait_for('rollback removes both tasks', task_count('docs'), ((0,),))

        execute('SET SESSION ai_column_strict_read=ON')
        check('strict ready query warms the plan cache',
              execute('SELECT label FROM docs WHERE id=3') == (('mock:recovered',),))
        execute('START TRANSACTION')
        execute("UPDATE docs SET content='pending-cache-check' WHERE id=2")
        fails('cached strict reads see uncommitted invalidation in another row',
              'SELECT label FROM docs WHERE id=3', 11120)
        execute('ROLLBACK')
        check('cached strict reads recover after rollback',
              execute('SELECT label FROM docs WHERE id=3') == (('mock:recovered',),))
        execute('SET SESSION ai_column_strict_read=OFF')

        # A byte-string primary key must survive quotes, backslashes and Unicode.
        execute(f"CREATE TABLE string_keys(id VARCHAR(80) PRIMARY KEY,content VARCHAR(100),"
                f"result VARCHAR(200) AI COLUMN(AI_COMPLETE('{model}',content)))")
        key = "quoted'\\中文"
        execute('INSERT INTO string_keys(id,content) VALUES(%s,%s)', (key, 'first'))
        wait_for('quoted string primary key is published',
                 'SELECT result FROM string_keys', (('mock:first',),))
        execute('START TRANSACTION')
        execute('UPDATE string_keys SET id=%s,content=%s WHERE id=%s', (key + '2', 'second', key))
        check('string primary key update invalidates result',
              execute('SELECT result IS NULL FROM string_keys') == ((1,),))
        execute('ROLLBACK')
        check('string primary key rollback restores row', execute('SELECT id,result FROM string_keys') == ((key, 'mock:first'),))

        execute(f"CREATE TABLE heap_docs(content VARCHAR(100),"
                f"result VARCHAR(200) AI COLUMN(AI_COMPLETE('{model}',content)))")
        execute("INSERT INTO heap_docs(content) VALUES('heap')")
        wait_for('hidden primary keys support publication',
                 'SELECT result FROM heap_docs', (('mock:heap',),))

        execute("SET SESSION sql_mode='ANSI_QUOTES,NO_BACKSLASH_ESCAPES'")
        execute(f"CREATE TABLE mode_literals(id BIGINT PRIMARY KEY,content VARCHAR(100),"
                f"result VARCHAR(200) AI COLUMN(AI_COMPLETE('{model}',CONCAT(\"content\",'\\\\'))))")
        execute("INSERT INTO mode_literals(id,content) VALUES(1,'mode')")
        wait_for('worker preserves definition SQL mode literals and identifiers',
                 'SELECT result FROM mode_literals', (('mock:mode\\\\',),))

        # Length validation uses native column conversion, independent of a lax
        # business session's SQL mode, and must reach FAILED with a finite budget.
        execute("SET SESSION sql_mode=''")
        execute(f"CREATE TABLE too_short(id BIGINT PRIMARY KEY,content VARCHAR(100),"
                f"result VARCHAR(2) AI COLUMN(AI_COMPLETE('{model}',content)) WITH(max_retries=0))")
        execute("INSERT INTO too_short(id,content) VALUES(1,'cannot-fit')")
        wait_for('conversion failure exhausts and removes its task', task_count('too_short'), ((0,),))
        check('failed conversion remains NULL', execute('SELECT result IS NULL FROM too_short') == ((1,),))
        execute('SET SESSION ai_column_strict_read=ON')
        fails('strict read rejects final conversion failure', 'SELECT result FROM too_short', 11120)
        fails('strict checks survive optimizer predicate pruning',
              'SELECT id FROM too_short WHERE result IS NULL AND 1=0', 11120)
        fails('strict checks include correlated AI references',
              'SELECT id FROM too_short t WHERE EXISTS(SELECT 1 WHERE t.result IS NULL)', 11120)
        execute('UPDATE too_short SET content=content WHERE id=1')
        check('ordinary UPDATE remains usable in strict mode', True)
        execute('SET SESSION ai_column_strict_read=OFF')
        execute('CREATE VIEW short_view AS SELECT id,result FROM too_short')
        execute('SET SESSION ai_column_strict_read=ON')
        check('ordinary view projections remain usable in strict mode',
              execute('SELECT id FROM short_view') == ((1,),))
        execute('SET SESSION ai_column_strict_read=OFF')

        # Metadata and hidden IDs survive MODIFY; existing rows are regenerated.
        execute(f"ALTER TABLE docs ADD COLUMN extra VARCHAR(200) AI COLUMN(AI_COMPLETE('{model}',content))")
        wait_for('ADD on a populated table is backfilled', 'SELECT extra FROM docs WHERE id=2', (('mock:Alpha',),))
        execute(f"ALTER TABLE docs MODIFY COLUMN extra VARCHAR(200) AI COLUMN(AI_COMPLETE('{model}',concat('new:',content)))")
        wait_for('MODIFY regenerates with the new definition', 'SELECT extra FROM docs WHERE id=2', (('mock:new:Alpha',),))
        check('SHOW AI COLUMNS exposes definitions', len(execute('SHOW AI COLUMNS FROM docs')) == 3)
        execute('CREATE VIEW document_results AS SELECT id,extra FROM docs')
        fails('updatable views preserve AI result protection',
              "UPDATE document_results SET extra='forged' WHERE id=2", 1235)
        print('AI-column lifecycle regressions passed', flush=True)
    finally:
        try:
            execute('ROLLBACK')
            execute('SET SESSION ai_column_strict_read=OFF')
            execute(f'DROP DATABASE IF EXISTS {database}')
            drop_models()
        finally:
            connection.close()
            subprocess.run([sys.executable, str(mock), 'stop', str(args.mock_port)], check=False)


if __name__ == '__main__':
    main()
