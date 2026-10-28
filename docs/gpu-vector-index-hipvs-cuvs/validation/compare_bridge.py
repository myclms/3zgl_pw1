"""Real AMD component benchmark, NOT the official SQL end-to-end score.

Requires Linux, AMD ROCm, hipVS and numpy. Every cold sample is a new process.
No precomputed vectors, answers, graph files or contest datasets are used.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time


def gpu_fds():
    targets = []
    for entry in Path('/proc/self/fd').iterdir():
        try:
            target = os.readlink(entry)
        except OSError:
            continue
        if target.startswith('/dev/dri/') or target == '/dev/kfd':
            targets.append(target)
    return sorted(set(targets))


def check(ids, distances, count):
    import numpy as np
    if np.any(ids >= count) or not np.all(np.isfinite(distances)):
        raise RuntimeError('Invalid result ID or non-finite distance')
    if any(len(set(row.tolist())) != len(row) for row in ids):
        raise RuntimeError('Duplicate neighbors in a query result')


def recall(ids, truth, k):
    return sum(len(set(row[:k].tolist()) & set(gold[:k].tolist()))
               for row, gold in zip(ids, truth)) / (len(ids) * k)


def child(args):
    import numpy as np
    base = np.ascontiguousarray(np.load(args.data/'base.npy'), dtype=np.float32)
    queries = np.ascontiguousarray(np.load(args.data/'queries.npy'), dtype=np.float32)
    truth = np.load(args.data/'truth.npy')
    lib = ctypes.CDLL(str(args.library.resolve()))
    fp, up = ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_uint)
    lib.seekdb_cuvs_build.argtypes = [fp, ctypes.c_long, ctypes.c_long]
    lib.seekdb_cuvs_build.restype = ctypes.c_void_p
    lib.seekdb_cuvs_search.argtypes = [ctypes.c_void_p, fp, ctypes.c_long, ctypes.c_long, up, fp]
    lib.seekdb_cuvs_search.restype = ctypes.c_int
    lib.seekdb_cuvs_free.argtypes = [ctypes.c_void_p]
    lib.seekdb_cuvs_free.restype = None
    lib.seekdb_cuvs_serialize.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.seekdb_cuvs_serialize.restype = ctypes.c_int
    lib.seekdb_cuvs_deserialize.argtypes = [ctypes.c_char_p, ctypes.c_long, ctypes.c_long]
    lib.seekdb_cuvs_deserialize.restype = ctypes.c_void_p
    ids = np.empty((1, 10), dtype=np.uint32)
    distances = np.empty_like(ids, dtype=np.float32)
    started = time.perf_counter()
    handle = lib.seekdb_cuvs_build(base.ctypes.data_as(fp), len(base), base.shape[1])
    if not handle:
        raise RuntimeError('Real GPU CAGRA build failed; CPU fallback is not accepted')
    try:
        snapshot = os.fsencode(args.child.with_suffix('.cagra'))
        if lib.seekdb_cuvs_serialize(handle, snapshot):
            raise RuntimeError('V5 GPU snapshot write failed')
        lib.seekdb_cuvs_free(handle)
        handle = None
        handle = lib.seekdb_cuvs_deserialize(snapshot, len(base), base.shape[1])
        if not handle:
            raise RuntimeError('V5 GPU snapshot reload failed')
        rc = lib.seekdb_cuvs_search(handle, queries.ctypes.data_as(fp), 1, 10,
                                   ids.ctypes.data_as(up), distances.ctypes.data_as(fp))
        first_ms = (time.perf_counter() - started) * 1000
        if rc:
            raise RuntimeError(f'First query failed: {rc}')
        check(ids, distances, len(base))
        devices = gpu_fds()
        if '/dev/kfd' not in devices:
            raise RuntimeError('No AMD KFD device opened by this process')
        report = {'real_amd_gpu': True, 'component_only': True,
                  'includes_sql_scan_and_cpu_vsag': False,
                  'includes_first_query_and_gpu_completion': True,
                  'includes_bridge_snapshot_write_and_reload': True,
                  'build_and_first_query_ms': first_ms, 'gpu_fds': devices,
                  'library_sha256': hashlib.sha256(args.library.read_bytes()).hexdigest(),
                  'rows': len(base), 'dimension': base.shape[1], 'queries': len(queries),
                  'samples': []}
        for k in (10, 100):
            ids = np.empty((len(queries), k), dtype=np.uint32)
            distances = np.empty_like(ids, dtype=np.float32)
            for repeat in range(3):
                started = time.perf_counter()
                rc = lib.seekdb_cuvs_search(handle, queries.ctypes.data_as(fp), len(queries), k,
                                           ids.ctypes.data_as(up), distances.ctypes.data_as(fp))
                ms = (time.perf_counter() - started) * 1000
                if rc:
                    raise RuntimeError(f'Batch query failed: {rc}')
                check(ids, distances, len(base))
                report['samples'].append({'k': k, 'repeat': repeat, 'ms': ms,
                    'qps': len(queries)/(ms/1000), 'recall': recall(ids, truth, k)})
        args.child.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    finally:
        if handle:
            lib.seekdb_cuvs_free(handle)


def main(args):
    import numpy as np
    if sys.platform != 'linux':
        raise SystemExit('Real AMD benchmark requires Linux ROCm; Windows mock tests are separate')
    if args.rows < 256 or args.dimension < 1 or args.queries < 1:
        raise SystemExit('Use at least 256 rows, a positive dimension and query count')
    if not args.baseline_bridge.is_file() or not args.candidate_bridge.is_file():
        raise SystemExit('Both source-built bridge libraries are required')
    args.output.mkdir(parents=True, exist_ok=False)
    data = args.output/'data'
    data.mkdir()
    rng = np.random.default_rng(20261028)
    base = rng.normal(size=(args.rows, args.dimension)).astype(np.float32)
    base /= np.maximum(np.linalg.norm(base, axis=1, keepdims=True), np.float32(1e-12))
    queries = base[rng.integers(0, len(base), size=args.queries)].copy()
    queries += rng.normal(0, 0.005, size=queries.shape).astype(np.float32)
    queries /= np.maximum(np.linalg.norm(queries, axis=1, keepdims=True), np.float32(1e-12))
    truth = np.empty((len(queries), 100), dtype=np.uint32)
    for i, query in enumerate(queries):
        distance = np.sum((base-query)**2, axis=1)
        best = np.argpartition(distance, 99)[:100]
        truth[i] = best[np.argsort(distance[best])]
    for name, value in [('base', base), ('queries', queries), ('truth', truth)]:
        np.save(data/(name+'.npy'), value)
    records = {'baseline': [], 'candidate': []}
    for repeat in range(3):
        for name in (['baseline', 'candidate'] if repeat%2 == 0 else ['candidate', 'baseline']):
            library = getattr(args, name+'_bridge').resolve()
            output = args.output/f'{name}-{repeat}.json'
            env = dict(os.environ)
            env['LD_LIBRARY_PATH'] = str(library.parent)+os.pathsep+env.get('LD_LIBRARY_PATH', '')
            command = [sys.executable, str(Path(__file__).resolve()), '--child', str(output),
                       '--data', str(data), '--library', str(library)]
            with (args.output/f'{name}-{repeat}.log').open('w') as log:
                subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                               check=True, timeout=1800)
            records[name].append(json.loads(output.read_text()))
    summary = {'component_only': True, 'official_score': None, 'records': records}
    for name, runs in records.items():
        summary[name] = {'build_and_first_query_ms': statistics.median(r['build_and_first_query_ms'] for r in runs),
                         'qps': statistics.median(s['qps'] for r in runs for s in r['samples']),
                         'qps_by_k': {str(k): statistics.median(s['qps'] for r in runs for s in r['samples'] if s['k']==k) for k in (10, 100)}}
    summary['build_speedup'] = summary['baseline']['build_and_first_query_ms']/summary['candidate']['build_and_first_query_ms']
    summary['query_speedup'] = summary['candidate']['qps']/summary['baseline']['qps']
    summary['quality_check'] = {str(k): all(
        statistics.median(s['recall'] for s in candidate['samples'] if s['k']==k)
        >= statistics.median(s['recall'] for s in baseline['samples'] if s['k']==k)-0.005
        for baseline, candidate in zip(records['baseline'], records['candidate'])) for k in (10, 100)}
    summary['query_speedup_by_k'] = {str(k): summary['candidate']['qps_by_k'][str(k)]/summary['baseline']['qps_by_k'][str(k)] for k in (10, 100)}
    summary['query_regression_check'] = {k: value >= 0.90 for k, value in summary['query_speedup_by_k'].items()}
    (args.output/'summary.json').write_text(json.dumps(summary, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({key:value for key,value in summary.items() if key != 'records'}, indent=2))


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline-bridge', type=Path)
parser.add_argument('--candidate-bridge', type=Path)
parser.add_argument('--output', type=Path)
parser.add_argument('--rows', type=int, default=10000)
parser.add_argument('--dimension', type=int, default=128)
parser.add_argument('--queries', type=int, default=128)
parser.add_argument('--child', type=Path, help=argparse.SUPPRESS)
parser.add_argument('--data', type=Path, help=argparse.SUPPRESS)
parser.add_argument('--library', type=Path, help=argparse.SUPPRESS)
arguments = parser.parse_args()
if arguments.child:
    child(arguments)
else:
    if not arguments.baseline_bridge or not arguments.candidate_bridge or not arguments.output:
        parser.error('--baseline-bridge, --candidate-bridge and --output are required')
    main(arguments)
