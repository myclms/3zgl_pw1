#!/usr/bin/env python3
"""Additional SQL protocol, row alignment, ordering and file boundary regressions."""
import argparse
import hashlib
import json
import pathlib
import urllib.request
import pymysql

parser = argparse.ArgumentParser()
parser.add_argument('--socket', required=True)
parser.add_argument('--files', required=True)
parser.add_argument('--output', type=pathlib.Path, required=True)
args = parser.parse_args()
connection = pymysql.connect(unix_socket=args.socket, user='root', database='test',
                             charset='utf8mb4', autocommit=True, read_timeout=90)
checks = []
models = []

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
    checks.append(label)
    print('PASS', label, flush=True)

def fails(label, sql, parameters=None, code=None):
    try:
        execute(sql, parameters)
    except pymysql.MySQLError as error:
        check(label, code is None or error.args[0] == code)
    else:
        raise AssertionError('expected SQL error: ' + label)

def model(name, kind, path, provider='openai'):
    endpoint = name + '_ep'
    for statement in ['CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)',
                      'CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)']:
        try:
            execute(statement, (endpoint if 'ENDPOINT' in statement else name,))
        except pymysql.MySQLError as error:
            if error.args[0] not in (11112, 11118):
                raise
    execute('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
            (name, json.dumps({'type': kind, 'model_name': 'fixture-model'})))
    execute('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',
            (endpoint, json.dumps({'ai_model_name': name, 'scope': 'all',
                                   'url': 'http://127.0.0.1:8000/' + path,
                                   'access_key': '' if provider.lower() == 'ollama' else 'mock-key',
                                   'provider': provider})))
    models.append((name, endpoint))
    return name

def completion_value(result):
    try:
        parsed = json.loads(result)
        return parsed if isinstance(parsed, str) else result
    except ValueError:
        return result

try:
    execute('SET enable_file_sql=ON')
    csv = args.files + '/basic.csv'
    ndjson = args.files + '/basic.jsonl'
    rows = execute('SELECT c.id,c.name,j.score FROM read_csv(%s) c LEFT JOIN read_json(%s) j ON c.id=j.id ORDER BY c.id', (csv,ndjson))
    check('native cross-format LEFT JOIN', [(r[0],r[1]) for r in rows] == [(1,'Alice'),(2,'Bob'),(3,'Chen')] and rows[1][2] is None)
    check('native cross-format INNER JOIN and LIMIT', execute('SELECT c.id FROM read_csv(%s) c JOIN read_ndjson(%s) j ON c.id=j.id WHERE c.id>=2 ORDER BY c.id DESC LIMIT 1', (csv,ndjson)) == ((3,),))
    check('two unaliased file functions', execute('SELECT COUNT(*) FROM read_csv(%s),read_json(%s)', (csv,ndjson)) == ((9,),))
    check('native GROUP BY and HAVING', execute('SELECT active,COUNT(*),SUM(id) FROM read_csv(%s) GROUP BY active HAVING COUNT(*)>1', (csv,)) == ((1,2,4),))
    fails('path outside secure_file_priv is rejected', "SELECT * FROM read_csv('/etc/passwd')")
    execute('SET enable_file_sql=OFF')
    fails('feature gate applies to cached plans', 'SELECT COUNT(*) FROM read_csv(%s)', (csv,),1235)
    execute('SET enable_file_sql=ON')

    embed = model('validation_embed','dense_embedding','batch-check/v1/embeddings')
    execute('DROP TABLE IF EXISTS validation_embedding_rows')
    execute('CREATE TABLE validation_embedding_rows(id INT, content TEXT)')
    with connection.cursor() as cursor:
        cursor.executemany('INSERT INTO validation_embedding_rows VALUES(%s,%s)',
                           [(i,None if i % 7 == 0 else 'text-' + str(i)) for i in range(1,66)])
    rows = execute('SELECT id,AI_EMBED(%s,content,8) FROM validation_embedding_rows WHERE id>=3 ORDER BY id LIMIT 41', (embed,))
    check('embedding filter/LIMIT row count', len(rows) == 41 and rows[0][0] == 3 and rows[-1][0] == 43)
    for index, encoded in rows:
        if index % 7 == 0:
            check('embedding NULL row ' + str(index), encoded is None)
        else:
            vector = json.loads(encoded)
            digest = hashlib.sha256(('text-' + str(index)).encode()).digest()
            check('reordered embedding maps to row ' + str(index), len(vector) == 8 and abs(vector[0] - round(digest[0]/255,6)) < 1e-9)
    fails('zero dimension', 'SELECT AI_EMBED(%s,%s,0)', (embed,'text'))
    fails('negative dimension', 'SELECT AI_EMBED(%s,%s,-1)', (embed,'text'))
    fails('noninteger dimension', 'SELECT AI_EMBED(%s,%s,1.5)', (embed,'text'))
    bad = model('validation_bad_dim','dense_embedding','wrong-dimension/v1/embeddings')
    fails('response dimension mismatch', 'SELECT AI_EMBED(%s,%s,8)', (bad,'text'))
    bad = model('validation_bad_element','dense_embedding','invalid-element/v1/embeddings')
    fails('response null vector element', 'SELECT AI_EMBED(%s,%s,8)', (bad,'text'))
    bad = model('validation_bad_index','dense_embedding','duplicate-index/v1/embeddings')
    fails('response duplicate batch index', 'SELECT AI_EMBED(%s,content,8) FROM validation_embedding_rows WHERE id<5', (bad,))

    rerank = model('validation_rerank','rerank','v1/rerank','siliconflow')
    documents = ['document-' + str(i) for i in range(45)]
    rows = execute('SELECT AI_RERANK(%s,%s,%s,NULL)', (rerank,'unmatched-query',json.dumps(documents)))
    ranked = json.loads(rows[0][0])
    check('rerank stable ties across three HTTP batches', [r['index'] for r in ranked] == list(range(45)))
    objects = [{'id':'original-' + str(i),'body':text} for i,text in enumerate(documents)]
    ranked = json.loads(execute('SELECT AI_RERANK(%s,%s,%s,%s)', (rerank,'unmatched-query',json.dumps(objects),'body'))[0][0])
    check('object rerank preserves original IDs', [r['id'] for r in ranked] == [r['id'] for r in objects])
    bad = model('validation_bad_rank','rerank','invalid-index/v1/rerank','siliconflow')
    fails('rerank out-of-range index', 'SELECT AI_RERANK(%s,%s,%s,NULL)', (bad,'query',json.dumps(documents[:2])))

    complete = model('validation_complete','completion','v1/chat/completions')
    prompt = execute("SELECT AI_COMPLETE(%s,AI_PROMPT('{0}/{0}/{1}/{2}',JSON_ARRAY(1,TRUE),JSON_OBJECT('k',2),AI_PROMPT('inner({0})','X')),NULL)", (complete,))[0][0]
    prompt = completion_value(prompt)
    check('JSON and nested Prompt rendering', prompt.count('[1, true]') == 2 and '{"k": 2}' in prompt and prompt.endswith('inner(X)'))
    execute('DROP TABLE IF EXISTS validation_prompt_lob')
    execute('CREATE TABLE validation_prompt_lob(t LONGTEXT,a LONGTEXT)')
    large = '汉' * 35000
    execute('INSERT INTO validation_prompt_lob VALUES(%s,%s)', ('{0}|{0}',large))
    actual = completion_value(execute('SELECT AI_COMPLETE(%s,AI_PROMPT(t,a)) FROM validation_prompt_lob',(complete,))[0][0])
    check('LONGTEXT repeated replacement beyond 64 KiB', actual == large + '|' + large)
    fails('placeholder index overflow', "SELECT AI_COMPLETE(%s,AI_PROMPT('{999999999999999999999999999999999999}','x'))", (complete,))
    fails('placeholder index out of range', "SELECT AI_COMPLETE(%s,AI_PROMPT('{2}','x'))", (complete,))
    bad = model('validation_empty_complete','completion','empty/v1/chat/completions')
    fails('empty completion response', 'SELECT AI_COMPLETE(%s,%s)', (bad,'hello'))
    bad = model('validation_wrong_complete','completion','wrong-type/v1/chat/completions')
    fails('nonstring completion response', 'SELECT AI_COMPLETE(%s,%s)', (bad,'hello'))
    ollama = model('validation_ollama','completion','api/generate','OlLaMa')
    check('Ollama native completion and config', completion_value(execute('SELECT AI_COMPLETE(%s,%s,%s)',(ollama,'native',json.dumps({'temperature':0.25,'max_tokens':9})))[0][0]) == 'native')

    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    traffic = json.load(opener.open('http://127.0.0.1:8000/', timeout=5))
    batches = [r['body']['input'] for r in traffic if '/batch-check/' in r['path']]
    check('one embedding request carries multiple rows', any(len(batch) > 1 for batch in batches))
    check('NULL inputs excluded from embedding requests', all(None not in batch for batch in batches))
    native = [r['body'] for r in traffic if r['path'].endswith('/api/generate')][-1]
    check('Ollama stream=false and options mapping', native['stream'] is False and native['options']['temperature'] == 0.25 and native['options']['num_predict'] == 9)
    execute('DROP TABLE validation_embedding_rows')
    execute('DROP TABLE validation_prompt_lob')
    args.output.write_text(json.dumps({'checks':checks,'embedding_request_sizes':[len(b) for b in batches]},ensure_ascii=False,indent=2),encoding='utf-8')
    print('All additional end-to-end checks passed:',len(checks),flush=True)
finally:
    for name,endpoint in models:
        execute('CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT(%s)',(endpoint,))
        execute('CALL DBMS_AI_SERVICE.DROP_AI_MODEL(%s)',(name,))
    connection.close()
