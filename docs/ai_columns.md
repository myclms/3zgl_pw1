# Asynchronous AI columns

AI columns use registered `AI_COMPLETE` and `AI_EMBED` models in MySQL mode.
Declare the result type and the expression in the table definition:

```sql
CREATE TABLE documents (
  id BIGINT PRIMARY KEY,
  body TEXT,
  tags JSON AI COLUMN (AI_COMPLETE('text_model', body))
    WITH (sync_mode='async', max_retries=2),
  embedding VECTOR(4) AI COLUMN (AI_EMBED('embedding_model', body, 4))
);

SHOW AI COLUMNS FROM documents;
SHOW CREATE TABLE documents;
```

Register models and endpoints with `DBMS_AI_SERVICE` before declaring an AI
column. The defaults are `sync_mode='async'` and `max_retries=3`; the retry
budget accepts integers from 0 through 100. AI result
columns are nullable and cannot be assigned by business INSERT or UPDATE
statements. An expression can depend on ordinary columns, but not on another
AI column or a generated column.

Inserting a row, changing an expression input, or changing the primary key
invalidates the affected results. The NULL result, PENDING state, request ID,
definition version and durable task are written in the business transaction.
Rollback removes those changes, and uncommitted tasks are invisible to the
background workers. An unrelated column update preserves the generated value.

Four background workers claim committed request groups with expiring leases.
Model requests run outside the publication transaction. Each task records its
outcome durably, and a single transaction publishes all terminal members of a
row's request group. Successful members become READY and exhausted members
become FAILED. Publication checks the current request ID, definition version
and task owner. A deleted row is never reinserted by the worker. Native UPDATE
execution maintains ordinary and vector indexes during invalidation and
publication.

Completion expressions receive a separate system prompt for JSON and numeric
result types. The business input remains the user message. Results pass through
the engine's column conversion before they are recorded as successful. The
durable task owns the retry budget, so a background HTTP attempt does not also
consume an independent transport retry loop. Automatic retries use exponential
backoff; final task rows are removed together with publication of FAILED state.

## Readiness and retry

Normal reads return NULL for pending and failed results. A successful SQL NULL
is also a valid READY result, so NULL alone does not indicate readiness.

```sql
SET SESSION ai_column_strict_read = ON;
SELECT id, tags FROM documents;
```

Strict read checks every row of each AI column referenced by the statement.
It raises error 11120 if any of those columns contains a non-READY row, even if
the WHERE clause selects only READY rows. It applies to SELECT, subqueries,
UPDATE and DELETE. Statements accessing only ordinary columns, or other fully
READY AI columns, remain usable. The check runs on each execution, including
execution of cached plans.

```sql
SET SESSION ai_column_strict_read = OFF;
ALTER TABLE documents RETRY AI COLUMN tags
  WHERE status = 'FAILED' AND id BETWEEN 10 AND 20;
ALTER TABLE documents RETRY AI COLUMN embedding WHERE id = 42;
```

RETRY invalidates only the named AI column and matching rows. It can also
regenerate READY rows, and a predicate matching no rows creates no tasks.
Bare `status` is a retry-only pseudo column with values PENDING, READY and
FAILED. A backtick-quoted identifier such as `` `status` `` addresses the
ordinary business column. RETRY must be the only action in its ALTER TABLE
statement.

ADD supports backfilling existing rows. MODIFY of an existing AI column keeps
its hidden metadata column IDs, advances the definition version and regenerates
the result with the new expression. Metadata is available through
`oceanbase.__all_ai_column`; durable work is in `oceanbase.__all_ai_column_task`.

## Native regression coverage

The existing mysqltest suite is at
`tools/deploy/mysql_test/test_suite/ai_column`. Run all five cases with the
native mysqltest executable; the scripts start their deterministic local mocks.
The additional integration script covers successful NULL embedding results,
expired leases, cached strict reads, case-only input changes, quoted Unicode
primary keys, SQL mode literals, conversion failures, business timestamps,
ADD backfill and MODIFY:

```sh
python3 unittest/sql/engine/test_ai_column_lifecycle.py --socket /path/to/sql.sock
```

The integration script requires PyMySQL and uses local mock port 19125 by
default. It creates and removes its own database, model and endpoint.
