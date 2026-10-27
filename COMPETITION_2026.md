# 2026 competition: AI functions and native file SQL

Based on `2026-competition` commit `5bce9e06d1281347ad61344e4b01fab9158c58a2`.

## Implementation

AI_PROMPT accepts TEXT and JSON with recursive and repeated placeholders. AI_COMPLETE validates JSON options and nonempty string responses. AI_EMBED validates dimensions, response elements and indexed result permutations, and batches selected non-NULL SQL rows by model/dimension (up to 128 inputs/request). AI_RERANK validates every batch response and restores global indices with stable ties. Model endpoints support case-insensitive Ollama, empty Ollama keys and native nonstreaming generate/embed requests.

File functions use the native parser, resolver and JSON_TABLE execution machinery. CSV/JSONL schema inference scans the complete file; execution then reads one bounded record at a time. Filters, sorting, aggregates and joins execute in seekdb. Parquet reads flat footer schemas and projected column pages with PLAIN, dictionary and boolean RLE encodings, uncompressed/Snappy pages, V1/V2 data pages, NULLs and multiple row groups. Timestamps support milliseconds, microseconds, nanoseconds (floored to SQL microseconds) and INT96. No external SQL engine is used.

`enable_file_sql` defaults to OFF. A nonempty directory in `secure_file_priv` is also required. Readers validate canonical paths, opened descriptors, identity/size/mtime/ctime and cached schema; changes fail the query. Records/pages are bounded to 16 MiB, schemas to 1024 columns, projected Parquet buffers to 128 MiB and glob results to 10000 files. COPY writes an exclusive temporary file, publishes without replacing existing targets, and cleans up on failure.

Remote sources, nested JSON/Parquet, multi-file union, decimal/fixed-byte Parquet and codecs other than Snappy/uncompressed are outside this implementation. Unsupported input produces an error.

## Build

Use the repository's Linux build instructions and dependencies. Configure/build Release with the repository's pinned Rust toolchain; the CMake configure step regenerates the SQL parser. The implementation was built on Debian 12 with Clang 17 and Rust 1.98.1, then run as a separate local seekdb instance with 4 GiB memory and 4 CPUs.

## Regression commands

The Python SQL runners require PyMySQL. Run the deterministic mock service on loopback port 8000:

```sh
python3 unittest/sql/engine/expr/mock_ai_service.py
```

Prepare the official `file_sql_basic` fixtures in a directory allowed by `secure_file_priv`. Its `export.csv` destination must not already exist. Run the checked-in official SQL suites and compare all expected tables/error codes:

```sh
python3 unittest/sql/engine/run_competition_sql.py \
  --source-root "$PWD" --unix-socket /path/to/run/sql.sock \
  --file-directory /path/to/fixtures --output /tmp/official-results.json
python3 unittest/sql/engine/test_competition_end_to_end.py \
  --socket /path/to/run/sql.sock --files /path/to/fixtures \
  --output /tmp/additional-results.json
```

The additional runner checks actual HTTP batching, indexed embedding alignment with NULL/filter/LIMIT, stable reranking across 20/20/5 requests, malformed provider responses, nested/large prompts, Ollama options, native cross-format joins and security gates.

Independent Parquet cross-validation uses Arrow only to generate test files (not in the seekdb runtime):

```sh
python3 unittest/sql/engine/test_file_sql_parquet.py --generate-dir /path/to/fixtures/regression
python3 unittest/sql/engine/test_file_sql_parquet.py \
  --files /path/to/fixtures/regression --socket /path/to/run/sql.sock \
  --output /tmp/parquet-results.json
```

The native reader unit `unittest/sql/engine/basic/test_file_scan_utils.cpp` is a standalone executable. Compile it with C++17, `-I src`, the dependency include directory, and ASan/UBSan. Pass a fresh writable test directory and the directory containing the official `simple.parquet` fixture. It covers CSV/JSON inference, paths, input mutation, COPY cleanup and malformed Snappy/Thrift inputs. `unittest/sql/parser/test_file_sql_parser.cpp` validates parse-tree contracts using the seekdb parser libraries.

## Local evidence (2026-10-27)

- Release seekdb target: compiled and linked successfully.
- SQL parser generation: no conflicts; focused parse-tree assertions passed.
- Official `file_sql_basic`, `ai_prompt` and `ai_function_enhancement`: all 168 statements matched expected results/error codes.
- Additional end-to-end suite: 67 checks passed, including multirow HTTP requests.
- Independent Arrow 17 fixtures: all 519 rows across six files matched, including Snappy, seven row groups, dictionary/plain, V1/V2 boolean RLE, NULLs, dates and timestamps; projection/aggregation passed.
- Native file-reader unit: ASan and UBSan passed, including 2000 malformed decoder inputs.

These are local results. Competition scores must come from the platform evaluation of the submitted commit.
