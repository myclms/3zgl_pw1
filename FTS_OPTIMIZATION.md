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

## Follow-up after platform evaluation

The platform completed commit `c5ee3180cf7986b37b91836dc1ed2bdddf48ebba` successfully but reported `fts_mean_improvement=-0.06751229347034847` and zero points. The platform's per-metric reference times were unavailable. The checked-in baseline JSON does not reproduce that score, so it must not be used to predict platform points. The follow-up comparison uses the actual submitted binary as its control.

CPU sampling of real SQL identified repeated configuration processing in TOKENIZE and per-candidate expression evaluation/array access in retrieval. This revision:

- Retains validated parser/configuration objects through TOKENIZE instead of formatting them as JSON and immediately parsing them again. Calls without additional arguments use the existing typed defaults directly. Explicit configurations still pass the original validation; dictionary loading remains per call.
- Handles the exact vectorized `BOOL(double relevance)` expression directly, using the same zero/NaN behavior as `ObExprBool`. Other expressions retain the general evaluation path. Buffered output publishes complete datum arrays and evaluation flags per batch, resetting datum pointers through the existing batch-write API.
- Accesses contiguous iterator/buffer arrays directly where indices are generated internally or have already been checked. Merge ordering, score calculation and LIMIT/OFFSET handling are unchanged.

The final Linux server built successfully and the native CTest passed. `fts_index.hot_path_equivalence`, `fts_index.tokenize_function` and `fts_index.simple_query` passed on both the submitted and follow-up binaries. The expanded snapshot comparator matched complete outputs on both versions: 2,000 original TOKENIZE results, custom/default/dynamic configurations, invalid-configuration error codes, natural-language/boolean matching, compound filters, repeated relevance expressions, ranking, pagination, empty results, DML and rollback. Relevance values are compared exactly, without rounding or tolerance.

Two further official benchmark pairs used the same parameters and server settings described above, with both binaries pinned to CPUs 0-3 and no compilation running. Pair A ran the optimized binary first; pair B ran the submitted binary first. Every run recreated and loaded the benchmark database. Dictionary state was warm for both optimized runs and the second submitted run; the first submitted run began after a restart. No index-construction improvement is claimed from these measurements.

| Metric | A submitted | A follow-up | B submitted | B follow-up |
| --- | ---: | ---: | ---: | ---: |
| SELECT 1, ms | 0.1606 | 0.1625 | 0.1303 | 0.1394 |
| Build IK title/content, s | 9.814 | 9.603 | 9.601 | 9.816 |
| Build IK content, s | 7.989 | 7.768 | 7.767 | 7.774 |
| Build BENG, s | 4.272 | 4.194 | 4.060 | 4.069 |
| TOKENIZE IK, ms | 0.3023 | 0.2515 | 0.2644 | 0.2489 |
| TOKENIZE BENG, ms | 0.1901 | 0.1693 | 0.1910 | 0.1872 |
| MATCH Chinese, ms | 2.5166 | 2.3194 | 2.2028 | 2.3121 |
| MATCH BENG, ms | 3.3923 | 2.7152 | 3.1542 | 3.0582 |
| MATCH mixed, ms | 2.5527 | 2.1344 | 2.7938 | 2.1830 |
| MATCH LIMIT, ms | 3.0320 | 3.2112 | 3.0358 | 3.1480 |

IK TOKENIZE decreased 16.8% / 5.9%, BENG TOKENIZE 10.9% / 2.0%, BENG matching 20.0% / 3.0%, and mixed matching 16.4% / 21.9%. Chinese matching moved in both directions. LIMIT latency increased 5.9% / 3.7%; SELECT 1 increased 1.2% / 7.0%. These results support improvements in the targeted paths, not a consistent improvement in every query or a predicted platform score. All four reports preserved hit counts `8001 / 11000 / 7332 / 20` and are retained as `*-round2-*-report.txt` in `unittest/storage/fts/results/`.

An additional CPU profile of 3,000 actual Chinese MATCH queries found no samples of `calc_bool_expr_for_double_type`, which appeared in the submitted profile. Batch projection also fell from 2.55% to 1.00% of sampled CPU. These percentages identify changed hotspots; they are not whole-query timing measurements.

## Local continuation on 2026-10-29

The continuation branch starts from `7bb9feb3ecbd9691995a562fe82506bd23788203` and is isolated from the AI COLUMN work. It adds the following changes without changing the official benchmark or scoring inputs:

The existing platform submission of this control commit (2026-10-28 18:57) was inspected on 2026-10-29: it completed successfully with 0.434/50 points, `fts_mean_improvement=0.004336845137197821` and raw score `0.8673690274395641`. Its measured build times were 7.156/5.528/2.822 seconds, TOKENIZE 0.0777/0.0403 ms, and query times 1.1791/1.7770/1.3525/1.8793 ms. The platform's actual reference times remain unavailable; the checked-in baseline JSON is not an appropriate predictor of platform points.

- IK reuses the UTF8 decoder's character length and code point instead of validating the first character and then decoding it again. Invalid/truncated UTF8 and all other charsets retain the previous validation/classification path, including its error behavior.
- Document-local word maps use `NoPthreadDefendMode`. All production instances are owned by one synchronous segmentation call or its caller's local document loop; shared dictionary/cache synchronization is unchanged.
- Stopword checking constructs its temporary conversion arena only for a different source charset. It still hashes/compares using the stopword collation and retains the original conversion/error path for other charsets, including binary input.
- Retrieval document IDs use direct unsigned-integer or varbinary comparisons. NULL values, non-binary collations (including `utf8mb4_bin`) and other types retain the general datum comparator.

Focused production translation units compiled with Clang 17 `RelWithDebInfo` flags on this Windows computer's Ubuntu 24.04 WSL environment. Three native differential executables passed, including ASan/UBSan instrumentation of the changed translation units (linked OBLib/datum-function dependencies were not instrumented):

- Character reading/classification: every Unicode scalar value under both UTF8 collations, every two-byte prefix, malformed/truncated/random bytes, and available UTF16 paths; existing BENG comparisons retained digest `16359905417020374183`.
- Stopwords and grouping: enabled charset conversions, accent/case-sensitive input collations, raw output spelling/order/frequencies, reuse, and eight concurrent workers with independent maps.
- Retrieval ID comparison: 800,000 comparisons against the production datum comparator across unsigned IDs, binary strings with embedded zero bytes, NULL, and PAD SPACE fallback collations.

Seven focused timing samples pinned to one CPU with this task's compilation paused gave these medians:

| Operation | Reference | Continuation | Reduction |
| --- | ---: | ---: | ---: |
| Read/classify a mixed UTF8 text | 0.764 us | 0.307 us | 59.8% |
| Group one token, including map reuse | 123.579 ns | 94.405 ns | 23.6% |
| Check one stopword | 96.392 ns | 92.542 ns | 4.0% |

These measurements cover individual operations, not index-construction time, SQL latency or a competition score.

### Full-server validation of the continuation

The control and continuation servers built and linked on this computer. All three standard CMake/CTest targets (`test_fts_hot_path`, `test_fts_word_grouping`, `test_fts_docid_compare`) passed. The production call sites in the optimizer and the two legacy retrieval paths use the common word-map alias as well; their maps are also local to synchronous calls.

Native `mysqltest` passed `fts_index.hot_path_equivalence`, `fts_index.tokenize_function` and `fts_index.simple_query` on both binaries. Four fresh dedicated instances were tested: control A, continuation A, continuation B, then control B. All captured token arrays, frequencies, document lengths, configuration results/error codes, matching user IDs and exact relevance values matched the control A snapshot. This includes natural-language and boolean queries, ordering/pagination, compound filters, DML/rollback and eight concurrent tokenization sessions. Comparison does not round relevance values or replace complete results with counts.

Both benchmark pairs used the unchanged official scripts and all fixed parameters: `ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`. Each fresh instance used `memory_limit=4G`, `cpu_count=4`, `datafile_size=512M`, `log_disk_size=2G`, ERROR logging and CPUs 0-3. No compilation ran during timing. Pair B reversed the version order to check the first pair's machine-state effect. Each run recreated and loaded all benchmark data.

| Metric | A control | A continuation | B control | B continuation |
| --- | ---: | ---: | ---: | ---: |
| SELECT 1, ms | 0.1151 | 0.1065 | 0.1252 | 0.1238 |
| Build IK title/content, s | 9.800 | 9.087 | 9.297 | 9.298 |
| Build IK content, s | 7.491 | 7.062 | 7.587 | 7.282 |
| Build BENG, s | 4.160 | 3.957 | 4.183 | 4.057 |
| TOKENIZE IK, ms | 0.2119 | 0.1968 | 0.2248 | 0.2027 |
| TOKENIZE BENG, ms | 0.1448 | 0.1361 | 0.1493 | 0.1404 |
| MATCH Chinese, ms | 1.6159 | 1.5378 | 1.6148 | 1.5604 |
| MATCH BENG, ms | 2.4455 | 2.1932 | 2.4262 | 2.2197 |
| MATCH mixed, ms | 1.9057 | 1.8108 | 1.8853 | 1.7737 |
| MATCH LIMIT, ms | 2.6426 | 2.4738 | 2.6142 | 2.4952 |

Using the stated category weighting, local mean time reduction relative to control commit `7bb9feb3` was **6.386% in A and 5.275% in B**, averaging **5.830%**. Build/tokenize/query category reductions were 5.961%/6.567%/6.629% in A and 2.340%/7.896%/5.588% in B. Negative metric improvements are retained in these averages. IK title/content construction was effectively unchanged in B (1 ms slower); a consistent improvement is not claimed for that metric. SELECT 1 also decreased, particularly in A, so the result is a local comparison, not a platform score prediction. All four runs preserved hit counts `8001 / 11000 / 7332 / 20`; full result snapshots also matched.

The four raw reports are in `unittest/storage/fts/results/local-20261029-*-report.txt`. Measurements were collected before committing: report Git metadata describes the validation checkout, while saved control/candidate executables were used explicitly. The control executable was built from a clean `7bb9feb3` checkout; candidate product-source files were compared between Windows and Linux using normalized SHA-256 hashes.

A separate baseline profile, collected with compilation paused, recorded 3,725 user CPU samples and no lost samples across interleaved Chinese, English and mixed MATCH queries. It identified heap merging, page decoding and generic binary-ID comparison/charset validation among the query costs. This supports the selected comparison hot path; sampled percentages are not latency reductions.

No official evaluation has been requested for this continuation. Its score must be measured by the platform against that platform's reference.
