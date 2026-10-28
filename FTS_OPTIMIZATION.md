# Full-text index tuning

This revision addresses [competition problem four](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/za7cfo5cazcrihhq). The reference for local comparisons is commit `1ea12f054109a81205853f5bbd4394f2299887d4`, whose FTS implementation is unchanged from the competition base.

## Changes

- IK classifies each character after one Unicode decode, retaining predicate order and the existing decoding/error behavior. Its four processors consume the character and classification already fetched by their caller.
- The whitespace tokenizer advances ASCII bytes directly when the charset has a one-byte minimum character width. Non-ASCII and other encodings continue through the original charset routine; delimiter classification is unchanged.
- BENG embeds its fixed tokenizer/normalizer pipeline. The normalizer can write lowercase tokens directly into the caller's allocator, avoiding a second token allocation/copy while retaining token validity after subsequent reads and parser reset. Other analyzer users keep their original scratch-buffer behavior.
- Word grouping inserts or updates through one hash-table lookup and bucket lock. Duplicate updates also avoid allocating a replacement hash node. The initial frequency, duplicate frequency increments, bucket order and last equivalent key's spelling remain unchanged, including case/accent-insensitive comparisons.

The official benchmark scripts, inputs, timing, scoring and baseline files are unchanged.

## Correctness checks

Build and run the focused native test after configuring the normal Linux build:

```sh
cmake --build build_release --target test_fts_hot_path -j2
ctest --test-dir build_release -R '^test_fts_hot_path$' --output-on-failure
```

The test checks all Unicode scalar values against the original classification predicates. It compares BENG with the original English analyzer across enabled character sets, punctuation, repeated words, mixed-language input, malformed/truncated input and 1,000 deterministic random byte strings. Retained tokens are read again after advancing and resetting the parser. UTF16/UTF16LE coverage is conditional on those charsets being enabled by the build.

The baseline and optimized native runs produce the same BENG output fingerprint, `16359905417020374183`. The optimized token-stream and BENG units also passed ASan/UBSan, linked against the normal OBLib build; OBLib itself was not instrumented. The final word-grouping and IK parser units passed compilation with the production flags.

`fts_index.hot_path_equivalence` adds SQL coverage for repeated-word frequencies, empty output, consecutive documents and collation-equivalent words with different spellings.

For broader before/after comparison, first run the unchanged official benchmark on a dedicated instance, then capture its results:

```sh
python3 unittest/storage/fts/compare_fts_results.py \
  --socket /path/to/run/sql.sock --output before.json
# Build/start the optimized server and rerun the official benchmark.
python3 unittest/storage/fts/compare_fts_results.py \
  --socket /path/to/run/sql.sock --reference before.json --output after.json
```

This comparator checks complete token arrays, frequencies and document lengths for 400 benchmark fields under IK smart/max-word, BENG, space and ngram. It also compares all matching IDs and relevance scores for the four query texts before DML, inside a transaction, and after rollback. It uses only the isolated `fts_large_bench` database and does not measure performance.

## Local performance evidence

The focused executable supports `--bench`. On the same WSL Ubuntu 24.04 host, Clang 17 `RelWithDebInfo` (`-O2`), CPU 15, seven interleaved samples after warmup, with compilation paused during timing:

| Operation | Before, median | After, median | Time reduction |
| --- | ---: | ---: | ---: |
| BENG fixed text, complete parser lifecycle | 1.593 us/document | 0.935 us/document | 41.3% |
| IK character classification | 14.833 ns/character | 8.387 ns/character | 43.5% |

These are native microbenchmarks, not SQL latency, index construction measurements or a competition score.

## Full-server validation

Both binaries built and linked successfully. The checked-in CTest target passed on the optimized build. Native `mysqltest` passed both the existing `fts_index.tokenize_function` suite and the new `fts_index.hot_path_equivalence` suite on both versions. All 2,000 captured TOKENIZE results and all matching IDs/relevance scores, including DML and rollback, matched the baseline exactly.

The unchanged official benchmark ran with `ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`. Each server used `memory_limit=4G`, `cpu_count=4`, `datafile_size=512M`, `log_disk_size=2G`, and ERROR logging. Each run recreated the benchmark database. A second pair pinned both versions to CPUs 0-3 to investigate the first pair's query-latency regression. The validation checkout omitted `.git`; its reports therefore show an unknown Git head. Before building the baseline, the seven FTS source files and five official benchmark files were compared with commit `1ea12f054` and matched after line-ending normalization.

| Metric | First baseline | First optimized | Pinned baseline | Pinned optimized |
| --- | ---: | ---: | ---: | ---: |
| SELECT 1, ms | 0.1380 | 0.1514 | 0.1233 | 0.1481 |
| Build IK title/content, s | 9.603 | 9.910 | 10.120 | 10.314 |
| Build IK content, s | 7.961 | 7.766 | 7.865 | 7.863 |
| Build BENG, s | 4.563 | 4.048 | 4.257 | 3.944 |
| TOKENIZE IK, ms | 0.2753 | 0.2663 | 0.2407 | 0.2989 |
| TOKENIZE BENG, ms | 0.2011 | 0.1816 | 0.1667 | 0.2023 |
| MATCH Chinese, ms | 2.1986 | 2.2953 | 2.4988 | 2.3242 |
| MATCH BENG, ms | 3.1699 | 3.4487 | 3.7453 | 3.2805 |
| MATCH mixed, ms | 2.4613 | 2.5988 | 3.2023 | 2.7293 |
| MATCH LIMIT, ms | 2.7596 | 2.9502 | 3.2652 | 3.0708 |

All four reports retained hit counts `8001 / 11000 / 7332 / 20`. The raw reports are in `unittest/storage/fts/results/`.

BENG index construction took 11.3% and 7.4% less time in the two pairs. SQL latency moved in both directions, including the SELECT 1 control, so these runs do not establish a stable whole-server latency improvement. Regressions are retained above. The platform must determine the competition score on its own hardware.
