#!/usr/bin/env python3
"""Execute the official SQL statements and compare result tables with their .result files.

The fixture copy/cleanup includes use SSH in the farm. Prepare those input files
locally before this runner; the SQL statements themselves are not rewritten,
except for substitution of the farm's FILE_SQL_DATA_DIR variable.
"""
import argparse
import json
import pathlib
import re
import sys
import time
import pymysql
from pymysql.constants import FIELD_TYPE


def wire_text_conversions():
    """Keep the server's temporal text, including fractional zeroes.

    PyMySQL's datetime conversion otherwise turns both DATETIME(0) and
    DATETIME(6) ending in .000000 into the same Python datetime object.
    mysqltest compares those different wire representations literally.
    """
    conversions = pymysql.converters.conversions.copy()
    for kind in (FIELD_TYPE.DATE, FIELD_TYPE.DATETIME, FIELD_TYPE.TIMESTAMP,
                 FIELD_TYPE.TIME):
        conversions[kind] = pymysql.converters.through
    return conversions


def statements(path):
    pending = []
    errors = {0}
    log_results = True
    for line in path.read_text(encoding='utf-8').splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith('#'):
            continue
        if stripped.startswith('--'):
            if stripped.startswith('--error '):
                errors = {int(value) for value in stripped[8:].split(',')}
            elif stripped == '--disable_result_log':
                log_results = False
            elif stripped == '--enable_result_log':
                log_results = True
            continue
        if stripped.startswith(('connection ', 'connect ', 'disconnect ', 'let ')):
            continue
        if stripped.startswith('eval '):
            stripped = stripped[5:]
        pending.append(stripped)
        # These suites contain no stored-routine delimiters or semicolons inside
        # multiline string literals; each statement ends on its own line.
        if stripped.endswith(';'):
            yield '\n'.join(pending), errors, log_results
            pending = []
            errors = {0}
    if pending:
        raise ValueError('unterminated SQL in ' + str(path))


def field(value):
    if value is None:
        return 'NULL'
    if isinstance(value, bytes):
        return value.decode('utf-8')
    if isinstance(value, float):
        return format(value, '.15g')
    return str(value)


def run_suite(connection, source, result, directory):
    expected = result.read_text(encoding='utf-8').splitlines()
    expected_position = 0
    outputs = []
    count = 0
    for sql, errors, log_results in statements(source):
        sql = sql.replace('$FILE_SQL_DATA_DIR', directory)
        try:
            with connection.cursor() as cursor:
                cursor.execute(sql)
                rows = cursor.fetchall() if cursor.description else ()
                columns = [item[0] for item in cursor.description] if cursor.description else []
                while cursor.nextset():
                    pass
            if 0 not in errors:
                raise AssertionError('expected SQL error ' + str(errors) + ': ' + sql)
        except pymysql.MySQLError as error:
            if error.args[0] not in errors:
                raise AssertionError(sql + '\n' + str(error)) from error
            rows, columns = (), []
        count += 1
        if columns and log_results:
            header = '\t'.join(columns)
            try:
                start = expected.index(header, expected_position)
            except ValueError as error:
                raise AssertionError('result header not found: ' + header + '\nSQL: ' + sql) from error
            actual = ['\t'.join(field(value) for value in row) for row in rows]
            wanted = expected[start + 1:start + 1 + len(actual)]
            if actual != wanted:
                raise AssertionError(sql + '\nACTUAL: ' + repr(actual) + '\nEXPECTED: ' + repr(wanted))
            expected_position = start + 1 + len(actual)
            outputs.append({'columns': columns, 'rows': [[field(value) for value in row] for row in rows]})
    return {'suite': source.name, 'statements': count, 'result_tables': outputs}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=2881)
    parser.add_argument('--unix-socket')
    parser.add_argument('--user', default='root')
    parser.add_argument('--password', default='')
    parser.add_argument('--source-root', type=pathlib.Path, required=True)
    parser.add_argument('--file-directory', required=True)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    parser.add_argument('--suite', action='append')
    arguments = parser.parse_args()
    for attempt in range(30):
        try:
            connection = pymysql.connect(host=arguments.host, port=arguments.port, unix_socket=arguments.unix_socket, user=arguments.user,
                                         password=arguments.password, charset='utf8mb4', autocommit=True,
                                         read_timeout=90, write_timeout=90, connect_timeout=2,
                                         conv=wire_text_conversions())
            break
        except pymysql.OperationalError as error:
            if error.args[0] != 2003 or attempt == 29:
                raise
            time.sleep(1)
    with connection.cursor() as cursor:
        cursor.execute('CREATE DATABASE IF NOT EXISTS test')
        cursor.execute('USE test')
        cursor.execute('SET GLOBAL secure_file_priv=%s', (arguments.file_directory,))
    results = []
    for suite, name in [('file_sql', 'file_sql_basic'), ('ai_function', 'ai_prompt'),
                        ('ai_function', 'ai_function_enhancement')]:
        if arguments.suite and name not in arguments.suite:
            continue
        root = arguments.source_root / 'tools/deploy/mysql_test/test_suite' / suite
        print('RUN', name, flush=True)
        results.append(run_suite(connection, root / 't' / (name + '.test'),
                                 root / 'r/mysql' / (name + '.result'), arguments.file_directory))
        print('PASS', name, results[-1]['statements'], 'statements', flush=True)
    arguments.output.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
    print('All official SQL result tables matched', flush=True)

if __name__ == '__main__':
    main()
