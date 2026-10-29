"""Read-only AMD environment checks before spending time on a GPU build.

This checks availability and toolchain versions, not CAGRA correctness or speed.
It never installs dependencies, starts a server, allocates a cloud instance,
downloads data, or reads authentication configuration.
"""

import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys


FROZEN_BASE = "634888d9b037153fa143d2c913ba4b25503e1d62"


def command(args, cwd=None):
    try:
        environment = dict(os.environ, GIT_NO_LAZY_FETCH="1", GIT_TERMINAL_PROMPT="0")
        result = subprocess.run(args, cwd=cwd, capture_output=True, text=True,
                                errors="replace", timeout=30, env=environment)
        return result.returncode, result.stdout, result.stderr
    except FileNotFoundError:
        return 127, "", "executable not found"
    except subprocess.TimeoutExpired:
        return 124, "", "command timed out"
    except OSError as error:
        return 126, "", str(error)


def inspect(args):
    checks = []

    def add(name, passed, detail):
        checks.append({"name": name, "passed": bool(passed), "detail": detail})

    add("linux_x86_64", sys.platform == "linux" and platform.machine() == "x86_64",
        {"platform": sys.platform, "machine": platform.machine()})
    code, head, _ = command(["git", "rev-parse", "HEAD"], args.source)
    add("git_checkout", code == 0, head.strip())
    code, _, _ = command(["git", "merge-base", "--is-ancestor", FROZEN_BASE, "HEAD"], args.source)
    add("v5_ancestor", code == 0, FROZEN_BASE)

    kfd = Path("/dev/kfd")
    add("amd_kfd_access", kfd.exists() and os.access(kfd, os.R_OK | os.W_OK), str(kfd))
    render_nodes = sorted(str(p) for p in Path("/dev/dri").glob("renderD*"))
    accessible = [p for p in render_nodes if os.access(p, os.R_OK | os.W_OK)]
    add("render_node_access", bool(accessible), accessible)
    rocminfo = args.rocm / "bin/rocminfo"
    executable = str(rocminfo) if rocminfo.is_file() else shutil.which("rocminfo")
    code, output, _ = command([executable]) if executable else (127, "", "")
    gpu_agents = sorted(set(re.findall(r"\bName:\s*(gfx[0-9a-z]+)\b", output)))
    add("rocm_gpu_agents", code == 0 and bool(gpu_agents),
        {"returncode": code, "architectures": gpu_agents})

    for relative in ("include/cuvs/core/c_api.h", "include/cuvs/neighbors/cagra.h",
                     "include/cuvs/cuda_runtime.h", "lib/libcuvs_c.so"):
        path = args.hipvs / relative
        add("hipvs:" + relative, path.is_file(), str(path))

    version_file = args.rocm / ".info/version"
    rocm_version = version_file.read_text(errors="replace").strip() if version_file.is_file() else ""
    add("rocm_7.2.4", bool(re.match(r"7\.2\.4(?:\D|$)", rocm_version)), rocm_version)
    hipcc = args.rocm / "bin/hipcc"
    code, output, _ = command([str(hipcc), "--version"])
    add("hipcc", code == 0, {"path": str(hipcc), "version": output.splitlines()[:3]})
    for name, invocation, expected in (
        ("gcc_13.3", ["gcc", "-dumpfullversion"], r"^13\.3(?:\D|$)"),
        ("cmake_3.31", ["cmake", "--version"], r"\b3\.31(?:\D|$)"),
        ("rust_1.97", ["rustc", "--version"], r"\b1\.97(?:\D|$)"),
    ):
        code, output, _ = command(invocation)
        first_line = output.splitlines()[0] if output else ""
        add(name, code == 0 and re.search(expected, first_line) is not None, first_line)

    # An optional completed build must link the candidate bridge, rather than
    # silently running the baseline bridge from the notebook image.
    if args.binary:
        add("candidate_binary", args.binary.is_file(), str(args.binary))
        code, output, _ = command(["readelf", "-d", str(args.binary)])
        needed = re.findall(r"\(NEEDED\).*\[([^]]+)\]", output)
        add("candidate_bridge_link", code == 0 and "libseekdb_cuvs_bridge_candidate.so" in needed,
            needed)

    passed = all(check["passed"] for check in checks)
    return {
        "recorded_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "source": str(args.source),
        "environment_ready": passed,
        "official_toolchain_versions_required": True,
        "checks": checks,
        "free_disk_bytes": shutil.disk_usage(args.source).free,
        "gpu_correctness_verified": False,
        "gpu_performance_measured": False,
        "official_score": None,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--hipvs", type=Path, default=Path("/opt/hipvs"))
    parser.add_argument("--rocm", type=Path, default=Path("/opt/rocm"))
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.source = args.source.resolve()
    if not args.source.is_dir():
        parser.error("--source must be an existing checkout")
    if args.output.exists():
        parser.error("--output already exists; choose a new evidence path")
    report = inspect(args)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    for check in report["checks"]:
        print(f"{'PASS' if check['passed'] else 'FAIL'} {check['name']}: {check['detail']}")
    print(f"Evidence: {args.output}; no GPU correctness or performance claim.")
    return 0 if report["environment_ready"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
