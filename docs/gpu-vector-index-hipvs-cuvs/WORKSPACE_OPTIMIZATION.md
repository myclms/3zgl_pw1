# Candidate: bounded per-index query workspace

Development baseline: `08c76418a8e1fef3c1aae83010658ff56a4faa57`, branch
`amd-oceanbase-official-contest-base`, tag `amd-cagra-contest-baseline-v1`.
This work belongs in the seekdb repository, not the PowerContext ecosystem
branch. SQL entry points and CPU VSAG implementation are preserved.

## What changes

The frozen bridge allocates and frees three device buffers and creates and
destroys a search-parameter object for every query. The candidate retains the
three buffers per live index and grows only when required. Total retained
workspace is bounded at 64 MiB; larger batches still execute but release their
workspace on return. Index destruction releases its workspace. There is no
cross-index graph cache or reuse across cold processes.

A handle owns a nonblocking GPU stream. Query transfer, CAGRA search and result
copies use the same stream, followed by a checked fence before the public call
returns. Build also completes its stream before the caller may free its input.
The distance-copy error, formerly ignored, is reported. A mutex protects shared
workspace during concurrent calls, and calls activate the correct device and
restore the calling thread's previous device. Initialization is checked and
partial state is cleaned up. Invalid shapes and byte-size overflow are rejected.

The public C ABI still calls real `cuvsCagraBuild` and `cuvsCagraSearch`. It does
not substitute CPU results, change graph degrees/build algorithm/search
parameters, quantize inputs, or change external IDs. GPU performance is not
inferred from these implementation changes: stream initialization and fencing
also have costs that must be included in the real measurements.

The build graph now builds the candidate bridge from source, rather than merely
linking the preexisting library specified by `CUVS_BRIDGE_LIB`. Its output is
local to the build directory and has a distinct SONAME, preventing a baseline
bridge from being loaded instead. CPU builds with `OB_BUILD_CUVS=OFF` do not
configure this GPU target.

## Checks already run on the Windows development machine

```powershell
python unittest/oblib/vector/cuvs_bridge/run_tests.py --evidence C:/path/to/evidence
```

The actual product bridge compiles as C11 and C++14 with warnings treated as
errors, against explicitly isolated CPU API doubles. Eight cases pass:
same-shape allocation/parameter reuse; deferred-operation completion and shape
changes; invalid input; partial initialization failure; copy/search/sync/OOM
errors and retry; two-index isolation; concurrent requests on one handle;
and releasing a workspace above the retention bound.

The doubles queue operations until stream completion to expose missing fences.
Their small exact-distance implementation is solely a test double. It is not an
AMD kernel, cannot measure CAGRA recall or performance, and is never linked into
the product. Only the standalone test command adds the stub include path.

```powershell
uv run --with cmake==3.31.6 --with ninja==1.11.1.3 python `
  unittest/oblib/vector/cuvs_bridge/check_cmake.py --evidence C:/path/to/evidence
```

The configuration-only fixture passes: the build dependency names the current
source, the candidate target is actually linked, its output belongs to the
current build directory, and its SONAME differs from the baseline. The fixture
uses fake toolchain paths and does not compile seekdb or run a GPU compiler.

Candidate-specific logs are outside the source tree. The full seekdb build,
real AMD runtime, GPU Recall and performance remain **unverified** until run on
ROCm/hipVS hardware. This is a development checkpoint, not a completed scored
submission.

## GPU verification required before submission

1. Obtain Linux AMD Radeon/ROCm/hipVS access. Build the frozen baseline in its
   own checkout and the candidate from this checkout, with the same release
   flags and no fast-math or changed index parameters.
2. Run the maintained SQL smoke runner for CPU isolation, L2-only validation,
   filters, delete/freshness fallback, one-call batch search and actual GPU
   activity. Use a fresh directory and separate ports for each version.
3. Measure the **SQL** path from index creation through scan, transfer, CAGRA
   construction, GPU completion and the first fixed legal query. Alternate three
   fresh rounds of frozen baseline/candidate. Use the same FP32 data and index
   parameters. Include the CPU `lib=vsag` comparison as a separate reference.
4. Check legal/distinct IDs, Recall@10 and Recall@100, absolute contest floors,
   no drop greater than 0.005, query throughput at least 90% of the frozen
   baseline, and memory use. The 64 MiB cache is extra retained workspace and
   must be checked against the cloud quota.
5. Use the bridge component harness below to diagnose runtime costs, then
   confirm any improvement in the complete SQL path. A component speedup cannot
   substitute for end-to-end build improvement.

```bash
python3 -m pip install numpy
LD_LIBRARY_PATH=/opt/hipvs/lib:/opt/rocm/lib python3 \
  docs/gpu-vector-index-hipvs-cuvs/validation/compare_bridge.py \
  --baseline-bridge /baseline/libseekdb_cuvs_bridge.so \
  --candidate-bridge /candidate/build/hipvs/libseekdb_cuvs_bridge_candidate.so \
  --output /tmp/amd-component-run-UNIQUE --rows 10000 --dimension 128 --queries 128
```

This generates its own FP32 corpus and exact CPU ground truth, runs each cold
sample in a new process, includes first-query completion, and records raw
timings, library digests, AMD KFD descriptors, IDs and recall checks. It refuses
to call a failed GPU build a CPU fallback success. Its report explicitly says
`component_only=true` and `official_score=null`. The official platform's fixed
test, data, build environment and score remain authoritative.

## Performance report status

| Required measurement | Current evidence |
|---|---|
| CPU/GPU SQL index-build time including first query | Pending AMD environment |
| Candidate / frozen-baseline build median, three alternating cold rounds | Pending |
| Real Recall@10 / Recall@100 and legal IDs | Pending |
| Batch QPS, P50/P95 and no-regression gate | Pending |
| GPU peak memory / full seekdb stability | Pending |
| Standalone bridge CPU-double contracts and C/C++ compile checks | Eight cases passed |
| CMake source-build dependency, linkage and SONAME fixture | Passed; configuration only |

No synthetic latency or unmeasured speedup is reported. The provided benchmark
is runnable code; its GPU results have not been invented or copied from the
reference Notebook.
