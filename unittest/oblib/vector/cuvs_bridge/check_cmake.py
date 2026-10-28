"""Configuration test only. Does not compile seekdb or execute a GPU compiler."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--evidence', required=True)
args = parser.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[3]
output = Path(args.evidence).resolve()
output.mkdir(parents=True, exist_ok=True)
cc = shutil.which('gcc')
cmake = shutil.which('cmake')
if not cc or not cmake or not shutil.which('ninja'):
    raise SystemExit('gcc, cmake and ninja are required')
with tempfile.TemporaryDirectory(prefix='cmake-config-', dir=output) as temporary:
    temporary = Path(temporary).resolve()
    if not temporary.is_relative_to(output):
        raise SystemExit('Unexpected temporary directory')
    source, build = temporary/'source', temporary/'build'
    source.mkdir()
    shutil.copytree(root/'tools/hipvs_bridge', source/'tools/hipvs_bridge')
    (source/'dummy.c').write_text('int bridge_test_dummy(void){return 0;}\n')
    module = (root/'cmake/CuvsBridge.cmake').as_posix()
    (source/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.20)
project(BridgeConfiguration C)
add_library(oblib_lib STATIC dummy.c)
include("'''+module+'''")
seekdb_add_cuvs_bridge(oblib_lib)
get_target_property(location seekdb_cuvs_bridge_candidate IMPORTED_LOCATION)
get_target_property(soname seekdb_cuvs_bridge_candidate IMPORTED_SONAME)
get_target_property(libraries oblib_lib LINK_LIBRARIES)
if(NOT location STREQUAL "${CMAKE_BINARY_DIR}/hipvs/libseekdb_cuvs_bridge_candidate.so")
  message(FATAL_ERROR "Candidate library is not local to this build tree")
endif()
if(NOT soname STREQUAL "libseekdb_cuvs_bridge_candidate.so")
  message(FATAL_ERROR "Candidate can be shadowed by the baseline SONAME")
endif()
if(NOT libraries STREQUAL "seekdb_cuvs_bridge_candidate")
  message(FATAL_ERROR "Candidate target is not actually linked")
endif()
''', encoding='utf-8')
    command = [cmake, '-S', str(source), '-B', str(build), '-G', 'Ninja',
               '-DCMAKE_SYSTEM_NAME=Linux', '-DCMAKE_C_COMPILER='+cc,
               '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY',
               '-DSEEKDB_HIPCC='+cc, '-DSEEKDB_HIPVS_INCLUDE_DIR='+str(here/'stubs'),
               '-DSEEKDB_BASH='+sys.executable]
    executed = subprocess.run(command, capture_output=True, text=True, timeout=60)
    record = {'configuration_only': True, 'gpu_build_executed': False,
              'fake_toolchain_paths': True, 'command': command,
              'returncode': executed.returncode, 'stdout': executed.stdout, 'stderr': executed.stderr}
    if executed.returncode == 0:
        rules = (build/'build.ninja').read_text(encoding='utf-8')
        record['has_source_build_rule'] = 'seekdb_cuvs_bridge.c' in rules and 'CUVS_BRIDGE_SONAME=libseekdb_cuvs_bridge_candidate.so' in rules
    record['passed'] = executed.returncode == 0 and record.get('has_source_build_rule', False)
    (output/'cmake-configuration.json').write_text(json.dumps(record, indent=2)+'\n', encoding='utf-8')
    print(executed.stdout)
    if not record['passed']:
        print(executed.stderr)
        raise SystemExit(1)
    print('PASS source dependency, target linkage, per-build output and distinct SONAME; configuration only.')
