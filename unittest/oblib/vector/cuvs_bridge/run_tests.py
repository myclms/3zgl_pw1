"""Compile the actual bridge against CPU API doubles; never a GPU benchmark."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--cc', default='gcc')
parser.add_argument('--cxx', default='g++')
parser.add_argument('--evidence', required=True)
parser.add_argument('--baseline', action='store_true', help='Require the frozen bridge to fail the reuse expectation')
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
if args.baseline and os.name == 'nt':
    raise SystemExit('Run the frozen-baseline diagnostic on LP64 Linux; its original DLPack shapes use long, which is 32-bit on Windows.')
here = Path(__file__).resolve().parent
root = here.parents[3]
output = Path(args.evidence).resolve()
output.mkdir(parents=True, exist_ok=True)
compiler = shutil.which(args.cc)
cxx = shutil.which(args.cxx)
if not compiler or not cxx:
    raise SystemExit('C and C++ compilers are required')
record = {'mock_only': True, 'real_gpu': False, 'gpu_performance_measured': False, 'commands': []}
with tempfile.TemporaryDirectory(prefix='bridge-test-', dir=output) as tmp:
    tmp = Path(tmp)
    if not tmp.resolve().is_relative_to(output):
        raise SystemExit('Unexpected temporary directory')
    source = root / 'tools/hipvs_bridge/seekdb_cuvs_bridge.c'
    if args.baseline:
        source = tmp / 'baseline.c'
        source.write_bytes(subprocess.check_output(['git', 'show', '634888d9b037153fa143d2c913ba4b25503e1d62:tools/hipvs_bridge/seekdb_cuvs_bridge.c'], cwd=root))
    flags = ['-O2', '-Wall', '-Wextra', '-Werror', '-pthread', '-D__HIP_PLATFORM_AMD__', '-I'+str(here/'stubs'), '-I'+str(root/'tools/hipvs_bridge')]
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    exe = tmp / ('test_bridge.exe' if os.name == 'nt' else 'test_bridge')
    command = [compiler, '-std=c11', *flags, str(source), str(here/'mock_gpu.c'), str(here/'test_bridge.c'), '-o', str(exe)]
    compile_result = subprocess.run(command, capture_output=True, text=True)
    record['commands'].append({'command': command, 'returncode': compile_result.returncode, 'stderr': compile_result.stderr})
    if compile_result.returncode:
        raise SystemExit(compile_result.stderr)
    cpp_command = [cxx, '-std=c++14', *flags, '-Werror', '-x', 'c++', '-c', str(source), '-o', str(tmp/'bridge.o')]
    cpp_result = subprocess.run(cpp_command, capture_output=True, text=True)
    record['commands'].append({'command': cpp_command, 'returncode': cpp_result.returncode, 'stderr': cpp_result.stderr})
    if cpp_result.returncode:
        raise SystemExit(cpp_result.stderr)
    executed = subprocess.run([str(exe), *(['reuse-only'] if args.baseline else [])], cwd=tmp, capture_output=True, text=True, timeout=120)
    record.update(returncode=executed.returncode, stdout=executed.stdout, stderr=executed.stderr,
                  expectation='baseline_reuse_test_must_fail' if args.baseline else 'all_mock_cases_must_pass')
    record['passed'] = executed.returncode != 0 if args.baseline else executed.returncode == 0
    (output / ('baseline-reuse.json' if args.baseline else 'candidate-mock.json')).write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
    print(executed.stdout, end='')
    if not record['passed']:
        print(executed.stderr)
        raise SystemExit(1)
    print('Expected result verified; no GPU performance claim.')
