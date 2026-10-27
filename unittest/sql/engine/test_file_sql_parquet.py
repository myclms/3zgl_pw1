"""Generate independent Arrow fixtures, then verify every value using native seekdb SQL.

Arrow is only a test fixture writer; the seekdb runtime does not depend on it.
Generate: python test_file_sql_parquet.py --generate-dir /path/to/regression
Verify: python test_file_sql_parquet.py --files /path/to/regression --socket /path/to/sql.sock
"""
import argparse
import datetime
import json
from pathlib import Path


def generate(folder):
    import pyarrow as pa
    import pyarrow.parquet as pq
    folder.mkdir(parents=True, exist_ok=True)
    columns = {
        'id': pa.array(range(103), pa.int64()),
        'i32': pa.array([None if i % 7 == 0 else i - 50 for i in range(103)], pa.int32()),
        'd': pa.array([None if i % 9 == 0 else i / 4 for i in range(103)], pa.float64()),
        'f': pa.array([i / 2 for i in range(103)], pa.float32()),
        'b': pa.array([None if i % 5 == 0 else bool(i % 2) for i in range(103)], pa.bool_()),
        's': pa.array([None if i % 11 == 0 else '中文,' + str(i % 4) for i in range(103)]),
        'date': pa.array([datetime.date(2020, 1, 1) + datetime.timedelta(days=i) for i in range(103)], pa.date32()),
        'us': pa.array([datetime.datetime(2024, 1, 1) + datetime.timedelta(seconds=i, microseconds=123456) for i in range(103)], pa.timestamp('us')),
        'ms': pa.array([datetime.datetime(1969, 12, 31) + datetime.timedelta(seconds=i, milliseconds=123) for i in range(103)], pa.timestamp('ms')),
    }
    table = pa.table(columns)
    expected = {}
    for dictionary in (False, True):
        for version in ('1.0', '2.0'):
            name = 'arrow-' + str(dictionary) + '-' + version + '.parquet'
            pq.write_table(table, folder / name, compression='snappy', row_group_size=17,
                           use_dictionary=dictionary, data_page_version=version)
            assert pq.ParquetFile(folder / name).metadata.num_row_groups == 7
            expected[name] = [[normalize(row[key]) for key in columns] for row in table.to_pylist()]
    timestamps = [1704067200123456000, -123456000, 0, -1]
    nanos = pa.table({'id': list(range(4)), 'ts': pa.array(timestamps, pa.timestamp('ns'))})
    pq.write_table(nanos, folder / 'nanos.parquet', compression='snappy')
    epoch = datetime.datetime(1970, 1, 1)
    expected['nanos.parquet'] = [[i, normalize(epoch + datetime.timedelta(microseconds=value // 1000))]
                                 for i, value in enumerate(timestamps)]
    int96 = table.select(['id', 'us'])
    pq.write_table(int96, folder / 'int96.parquet', compression='snappy', row_group_size=17,
                   use_deprecated_int96_timestamps=True)
    expected['int96.parquet'] = [[row['id'], normalize(row['us'])] for row in int96.to_pylist()]
    (folder / 'expected.json').write_text(json.dumps(expected, ensure_ascii=False), encoding='utf-8')
    print('Generated 6 independent Parquet fixtures with Arrow', pa.__version__)


def normalize(value):
    if isinstance(value, (datetime.date, datetime.datetime)):
        return str(value)
    return value


def verify(folder, socket, output):
    import pymysql
    expected = json.loads((folder / 'expected.json').read_text(encoding='utf-8'))
    connection = pymysql.connect(unix_socket=socket, user='root', database='test',
                                 autocommit=True, charset='utf8mb4')
    results = []
    try:
        with connection.cursor() as cursor:
            cursor.execute('SET enable_file_sql=ON')
            for name, rows in expected.items():
                cursor.execute('SELECT * FROM read_parquet(%s) ORDER BY id', (str(folder / name),))
                actual = [[normalize(value) for value in row] for row in cursor.fetchall()]
                assert actual == rows, (name, actual[:2], rows[:2])
                # A separate projection must preserve values while avoiding unrelated pages.
                cursor.execute('SELECT id FROM read_parquet(%s) WHERE id >= 2 ORDER BY id', (str(folder / name),))
                assert [row[0] for row in cursor.fetchall()] == [row[0] for row in rows if row[0] >= 2]
                cursor.execute('SELECT COUNT(*),SUM(id) FROM read_parquet(%s)', (str(folder / name),))
                count, total = cursor.fetchone()
                assert count == len(rows) and total == sum(row[0] for row in rows)
                results.append({'file': name, 'rows': len(rows), 'status': 'passed'})
                print('PASS', name, len(rows), 'rows, full values, projection and aggregation', flush=True)
    finally:
        connection.close()
    if output:
        output.write_text(json.dumps(results, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--generate-dir', type=Path)
    parser.add_argument('--files', type=Path)
    parser.add_argument('--socket')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.generate_dir:
        generate(args.generate_dir)
    elif args.files and args.socket:
        verify(args.files, args.socket, args.output)
    else:
        parser.error('provide --generate-dir, or --files and --socket')
