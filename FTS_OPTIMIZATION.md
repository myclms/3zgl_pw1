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

This continuation was published as `ad3d2956cf066078a1a96c8028184dfc9aa3aeaa` and submitted on 2026-10-29 at 15:25. The platform completed successfully with **11.923/50**, `fts_mean_improvement=0.11923267319564404` and raw score `23.84653463912881`. This score corresponds to 11.923% mean time reduction against the platform reference; full points require 50% reduction, rather than just passing correctness checks.


## Score-free natural-language membership (2026-10-29)

The reference for this step is the evaluated `ad3d2956` binary. Preparing a text retrieval scan previously always requested BM25 relevance: a historical score-usage check in `prepare_text_retrieval_info` is commented out. Even a pure `COUNT(*) ... WHERE MATCH(...) AGAINST(...)` therefore estimated corpus/document frequencies and average document length and evaluated per-posting BM25 values.

The optimizer now inspects every semantic expression root, including SELECT and ORDER BY, before eliding exact scores. Eligibility is deliberately conservative: a basic-table SELECT, natural-language mode without boosts, exactly one WHERE predicate equal to BOOL(MATCH), one match filter, and no other scan filters or subqueries in that block. Only BOOL consumers are accepted. Numeric projections, arithmetic, numeric thresholds, explicit or implicit relevance ordering, boolean-mode searches, joins, functional lookups and queries with additional predicates retain the normal score path. There are no query-text, table-name, row-count or benchmark-specific checks.

BM25 clamps the query token weight to a positive epsilon; a matching natural-language posting has a positive contribution. A positive membership value therefore preserves BOOL(MATCH) after the optimizer proves that no consumer observes its numerical value. The generated scan uses the existing `need_calc_relevance_` contract to omit unused statistics scans and BM25 expressions. Scores exposed to SQL callers continue to use the original calculation.

The dormant no-score DAAT/TAAT paths now initialize their skip vector, avoid null aggregate/statistics references, update TAAT token ranges without an aggregate scan, and deduplicate score-free TAAT document IDs. Inverted scans retain forward ID ordering even without aggregate iterators. Empty queries and iterator reuse keep their normal termination behavior.

During development, an unguarded version failed the new primary-key/compound-filter differential test: its projected IDs did not match the filtered rows. Changing lookup order alone did not fix that mismatch and was reverted. The final optimization excludes all additional predicates, with EXPLAIN assertions that those cases retain full scoring. This is a tested fallback boundary, not a claim that the dormant compound-filter path has been repaired.

### Validation

Both complete server builds and the three normal CMake/CTest targets passed. The final candidate passed the three native SQL suites `hot_path_equivalence`, `tokenize_function` and `simple_query` on two fresh instances. Four native expected EXPLAIN lines in `simple_query.result` change `calc_relevance=true` to `false`; the query results and all other expected text remain unchanged.

`compare_fts_membership.py` captured **154 complete result sets** from the evaluated reference and compared them exactly on both final candidate instances. It covers hidden document IDs and integer primary keys; empty/NULL/stopword-only/repeated questions; 320 distinct query terms exercising TAAT; ascending and descending IDs; compound-filter fallbacks; exact scores, numeric thresholds, arithmetic and ranking; joins; prepared-statement reuse; and DML/rollback. Eight concurrent connections each repeat the primary-key compound-filter case 20 times. EXPLAIN checks prove score elision for eligible membership/count queries and full scoring for numeric, ranked, implicit relevance-order and compound-filter cases. A full scan using functional MATCH lookup is also accepted as an exact-score fallback.

Reproduce the focused comparison against two dedicated instances, using the same script on both binaries:

```sh
python unittest/storage/fts/compare_fts_membership.py \
  --socket /path/to/reference/run/sql.sock --output /tmp/membership-reference.json
python unittest/storage/fts/compare_fts_membership.py \
  --socket /path/to/candidate/run/sql.sock --output /tmp/membership-candidate.json \
  --reference /tmp/membership-reference.json --expect-elision
```

The existing full `compare_fts_results.py` snapshots also matched for every final candidate/control run, including all token arrays, frequencies, document lengths and exact retrieval scores. The initial control A run used the earlier 138-case fixture, followed by a separate 154-case reference capture; control B and both final candidate runs used the final 154-case fixture. No official benchmark inputs, scripts, timing or scoring files were changed. The fixed benchmark files also match the frozen competition ancestor.

### Full benchmark results

Each run loaded all 20,000 documents into a fresh instance with the unchanged original benchmark: batch 500, tokenize rounds 3,000, query rounds 200, samples 3, warmup 30, SKIP_LOAD=0. All four instances used memory 4G, cpu_count 4, datafile 512M, log disk 2G, ERROR logging and CPUs 0-3. No compilation ran during timing. A ran reference first; B ran candidate first.

| Metric | A reference | A candidate | B reference | B candidate |
| --- | ---: | ---: | ---: | ---: |
| SELECT 1, ms | 0.1206 | 0.1248 | 0.1164 | 0.1106 |
| Build IK title/content, s | 9.299 | 9.296 | 9.292 | 9.098 |
| Build IK content, s | 7.271 | 7.264 | 7.261 | 7.503 |
| Build BENG, s | 3.957 | 3.952 | 3.949 | 3.974 |
| TOKENIZE IK, ms | 0.2030 | 0.1991 | 0.2253 | 0.1955 |
| TOKENIZE BENG, ms | 0.1420 | 0.1417 | 0.1616 | 0.1352 |
| MATCH Chinese, ms | 1.5698 | 1.3336 | 1.5799 | 1.3225 |
| MATCH BENG, ms | 2.2562 | 1.9449 | 2.3636 | 1.9303 |
| MATCH mixed, ms | 1.8127 | 1.4752 | 1.8506 | 1.5071 |
| MATCH LIMIT, ms | 2.5405 | 2.5032 | 2.5342 | 2.5494 |

The four-query category improved **12.233% in A and 13.147% in B**, averaging **12.690%**. Chinese, English and mixed queries improved in both pairs; the LIMIT query retained its required relevance ordering and was effectively unchanged, including a 0.60% regression in B. All hit counts remained `8001 / 11000 / 7332 / 20`, with complete result comparisons above.

Across all three equally weighted categories the raw local reductions were 4.461% and 9.101% (mean 6.781%). Tokenizer code was not changed in this step: its larger apparent improvement in B, along with SELECT 1 variation, is machine/run variation and is not attributed to score elision. No consistent build-time improvement is claimed. The query category is the reproducible gain; this local result does not predict a platform score or establish the 50% overall reduction needed for full points.

Raw reports are `unittest/storage/fts/results/local-20261029-membership-*-report.txt`. Report Git metadata is the dirty validation checkout at `ad3d2956`; binaries were selected explicitly. The reference executable was saved from that evaluated implementation, while the candidate executable was rebuilt from the seven changed production files.

## Dictionary character lookup (2026-10-29)

The repository owner supplied the subsequent official result for `77ac24de`: **17.966/50**, mean improvement `0.17966377053918248`. The complete supplied metrics are retained in `unittest/storage/fts/results/platform-20261029-1617-77ac24de.json`. This is the evaluated control for this step. The scoring contract, remaining gap and larger optimization candidates are documented in `FTS_SCORE50_ANALYSIS.md`.

Actual TOKENIZE CPU sampling identified `ObArrayHashMap::find` and `ObFTDATReader::match_with_hit` as IK dictionary hotspots. The reader now queries with the original character view, omitting the temporary `ObFTSingleWord` copy. Dictionary code maps use a power-of-two capacity and masks for initial placement and collision probing. The existing stride of three then visits every bucket; insert and find use the same hash and probe sequence. Word bytes, assigned character codes and trie matching semantics are unchanged.

Rounding capacity upward can increase the code-map allocation to almost twice its previous size. This does not double the complete trie allocation, but the memory tradeoff matters on larger dictionaries. The complete local tests below used the same 4 GiB limit as the control. No unbounded cache or input-specific result cache is added.

### Validation

- The changed production unity translation unit compiled with the Release build's Clang 17 flags and the server linked against the existing unchanged libraries.
- All four FTS CTest targets passed, including the new `test_fts_dictionary_lookup`. It covers random keys of every supported byte length, embedded NULs, packed/unaligned storage, capacity boundaries, missing keys, deliberately colliding keys and repeated wraparound.
- The new unit also passed ASan/UBSan, with its test and dictionary sources instrumented and the existing dependency libraries uninstrumented.
- Native mysqltest passed `hot_path_equivalence`, `tokenize_function` and `simple_query` against the final candidate.
- Complete TOKENIZE arrays, spellings, frequencies, document lengths, matching IDs and exact relevance values matched the fresh control on both candidate runs. The second candidate also matched all 154 membership, numeric-score, prepared-statement, concurrent-query, DML and rollback result sets.

The first snapshot reference had been collected after multiple database rebuilds for profiling. Its BM25 values differed slightly from those on fresh instances, including BENG, whose tokenizer was not changed. A fresh run of the *unchanged control executable* reproduced the candidate values exactly and differed from that first reference. The two fresh control/candidate comparisons therefore use matching fresh-instance state and retain exact numerical comparison, without tolerances. The discarded cross-state comparison is not presented as a passing check.

### Measurements

The focused native benchmark used 2,000 three-byte UTF8 keys, identical entries and capacity for the reference/candidate algorithms, six samples with alternating order, and CPU 15. No compilation ran during timing. Median lookup time decreased from **19.778 ns to 12.824 ns (35.16%)**; all checksums were identical. These numbers isolate lookup work, not complete parser or SQL time.

Two complete official benchmark pairs used fresh instances, CPUs 0–3, memory 4G, cpu_count 4, datafile 512M, log disk 2G and ERROR logging. Rows/batch/rounds/query rounds/samples/warmup were unchanged at 20000/500/3000/200/3/30, with SKIP_LOAD=0. Both pairs ran control first; order was not counterbalanced. No compilation or CPU sampling ran during these timings.

| Metric | A control | A candidate | B control | B candidate |
| --- | ---: | ---: | ---: | ---: |
| SELECT 1, ms | 0.1330 | 0.1075 | 0.1088 | 0.1026 |
| Build IK title/content, s | 9.612 | 9.609 | 9.822 | 9.710 |
| Build IK content, s | 7.488 | 7.378 | 7.482 | 7.271 |
| Build BENG, s | 4.069 | 3.958 | 3.978 | 3.950 |
| TOKENIZE IK, ms | 0.2070 | 0.1953 | 0.1947 | 0.1923 |
| TOKENIZE BENG, ms | 0.1442 | 0.1366 | 0.1342 | 0.1361 |
| MATCH Chinese, ms | 1.3702 | 1.3559 | 1.3248 | 1.3440 |
| MATCH BENG, ms | 1.9587 | 1.9358 | 1.9610 | 1.9542 |
| MATCH mixed, ms | 1.7213 | 1.5069 | 1.5091 | 1.5771 |
| MATCH LIMIT, ms | 3.0636 | 2.5421 | 2.5548 | 2.9372 |

The build category decreased 1.41% / 1.55%, and IK TOKENIZE decreased 5.65% / 1.23%. BENG and SELECT 1 also moved, although their implementation was unchanged. Query timings moved in both directions, including a 14.97% LIMIT regression in B. Across all three categories the local mean reductions were **4.93% and -1.23%**; these runs do not establish a consistent overall SQL improvement, explain all query variation, or predict platform points. All four reports retained hit counts 8001 / 11000 / 7332 / 20.

Raw reports, the native lookup measurements and the build-profile summary are retained as `local-20261029-lookup-*` in `unittest/storage/fts/results/`. Their Git metadata comes from the dirty validation checkout at `77ac24de`; binaries were selected explicitly. The official benchmark, data generator, SQL, timer, scorer and baseline were not modified. A new platform evaluation is required to decide whether this tradeoff improves the competition score.
