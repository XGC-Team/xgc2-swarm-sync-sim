#!/usr/bin/env python3
"""Bounded, offline production-header probe. Output is not a ROS/DMPC profile."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time

SIZES = (1, 8, 32, 100)
SEEDS = (154, 20260924, 20260925)
BASE_BLOB = "57ca4920b0097ef9232d230361a4e541716556fc"

def digest(data):
    return hashlib.sha256(data).hexdigest()

def git_blob(data):
    return hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()

def stats(values):
    ordered = sorted(values)
    return {"n": len(ordered), "p50": ordered[math.ceil(.50*len(ordered))-1],
            "p95": ordered[math.ceil(.95*len(ordered))-1], "max": ordered[-1]}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--header", type=Path, required=True, help="exact Timer.hpp to compile")
    parser.add_argument("--output", type=Path, required=True, help="NEW isolated output directory")
    parser.add_argument("--label", choices=("baseline", "candidate"), required=True)
    parser.add_argument("--cpu", type=int, required=True, help="one allowed Linux CPU")
    parser.add_argument("--sanitizers", action="store_true", help="ASan/UBSan checks, no timings")
    args = parser.parse_args()
    if not hasattr(os, "sched_setaffinity") or args.cpu not in os.sched_getaffinity(0):
        parser.error("requested CPU unavailable; do not silently change the comparison budget")
    src = Path(__file__).resolve().parent
    data = args.header.read_bytes()
    if args.label == "baseline" and git_blob(data) != BASE_BLOB:
        parser.error("baseline Git blob mismatch")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)  # never overwrite an earlier attempt
    os.sched_setaffinity(0, {args.cpu})
    os.environ.update(OMP_NUM_THREADS="1", OPENBLAS_NUM_THREADS="1")
    record = {"schema": "p03-source-microprofile-v1", "label": args.label,
              "scope": "actual TimerManagerExtra with a single-thread test boundary; NOT ROS integration",
              "header_blob": git_blob(data), "header_sha256": digest(data),
              "cpu_affinity": sorted(os.sched_getaffinity(0)), "platform": platform.platform(),
              "python": sys.version, "sizes_timers_not_robots": SIZES, "seeds": SEEDS,
              "warmup_updates": 2000, "samples_per_case": 101,
              "quantile": "nearest rank, no outlier exclusions, no observer subtraction",
              "sanitizers": args.sanitizers, "commands": [], "cases": [],
              "test_source_sha256": {p.name:digest(p.read_bytes()) for p in (src/"run.py",src/"probe.cpp",src/"boundary.hpp")}}
    for name, path in (("cpuinfo", "/proc/cpuinfo"), ("meminfo", "/proc/meminfo"),
                       ("cgroup_cpu_max", "/sys/fs/cgroup/cpu.max"), ("cgroup_memory_max", "/sys/fs/cgroup/memory.max")):
        p = Path(path)
        if p.exists(): (out/(name+".txt")).write_bytes(p.read_bytes())
    (out/"invocation.json").write_text(json.dumps(sys.argv, ensure_ascii=False, indent=2)+"\n")
    inc = out/"include"
    for path in ("ros/ros.h", "ros/callback_queue.h", "ros/callback_queue_interface.h",
                 "rosgraph_msgs/Clock.h", "std_msgs/Bool.h", "sss_sim_env/ClockUpdater.hpp"):
        p = inc/path; p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text('#include "boundary.hpp"\n')
    (inc/"sss_sim_env/Timer.hpp").write_bytes(data)
    def save():
        (out/"results.json").write_text(json.dumps(record, ensure_ascii=False, indent=2)+"\n")
    started = time.monotonic()
    def run(cmd, name):
        if time.monotonic()-started > 120:
            raise RuntimeError("attempt exceeded 120-second execution budget")
        at = time.time_ns()
        try:
            proc = subprocess.run(cmd, text=True, capture_output=True, timeout=30, cwd=out)
            code, stdout, stderr = proc.returncode, proc.stdout, proc.stderr
        except subprocess.TimeoutExpired as exc:
            code = 124
            stdout = exc.stdout or b""; stderr = exc.stderr or b""
            stdout = stdout.decode(errors="replace") if isinstance(stdout, bytes) else stdout
            stderr = stderr.decode(errors="replace") if isinstance(stderr, bytes) else stderr
            stderr += "\nTIMEOUT 30 s\n"
        (out/(name+".stdout")).write_text(stdout)
        (out/(name+".stderr")).write_text(stderr)
        record["commands"].append({"argv": [str(x) for x in cmd], "cwd": str(out), "start_unix_ns": at,
                                   "elapsed_s": (time.time_ns()-at)/1e9, "exit_code": code,
                                   "stdout": name+".stdout", "stderr": name+".stderr"})
        save()
        return code, stdout
    try:
        cxx = shutil.which("g++")
        if cxx is None: raise RuntimeError("g++ unavailable")
        run([cxx, "--version"], "compiler")
        flags = ["-O2", "-std=gnu++14", "-Wall", "-Wextra", "-pthread"]
        if args.sanitizers: flags = ["-O1", "-g", "-std=gnu++14", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-pthread"]
        exe = out/"probe"
        code, _ = run([cxx, *flags, "-I"+str(inc), "-I"+str(src), str(src/"probe.cpp"), "-o", str(exe)], "build")
        if code: raise RuntimeError("build failed; see build.stderr")
        record["executable_sha256"] = digest(exe.read_bytes())
        for n in SIZES:
            for seed in SEEDS:
                code, stdout = run([str(exe), "semantics", str(n), str(seed)], f"semantics-{n}-{seed}")
                record["cases"].append({"mode":"semantics", "timers":n,"seed":seed,"exit_code":code,"stdout":stdout})
                if code: raise RuntimeError("semantic oracle check failed")
        code, stdout = run([str(exe), "edges", "3", "154"], "edges")
        record["edges_exit_code"] = code
        record["edges_stdout"] = stdout
        if code: raise RuntimeError("deterministic edge checks failed")
        code, stdout = run([str(exe), "removal", "3", "154"], "removal")
        record["removal_exit_code"] = code
        record["removal_stdout"] = stdout
        if code and args.label != "baseline": raise RuntimeError("candidate removal regression failed")
        if not args.sanitizers:
            for n in SIZES:
                for seed in SEEDS:
                    code, stdout = run([str(exe), "bench", str(n), str(seed)], f"bench-{n}-{seed}")
                    if code: raise RuntimeError("benchmark failed")
                    rows = {"SAMPLE":[], "OBSERVER":[]}
                    meta = {}
                    for line in stdout.splitlines():
                        columns = line.split(",")
                        if columns[0] in rows:
                            rows[columns[0]].append([int(x) for x in columns[3:]])
                        elif line.startswith("BENCH_META "):
                            meta = {k:int(v) for k,v in (x.split("=") for x in line.split()[1:])}
                    require_rows = all(len(v)==101 for v in rows.values())
                    if not require_rows or not meta: raise RuntimeError("incomplete raw output")
                    record["cases"].append({"mode":"bench", "timers":n,"seed":seed,
                        "raw_columns":["sample","wall_ns","thread_cpu_ns"], **rows,"metadata":meta,
                        "sample_wall_ns":stats([x[1] for x in rows["SAMPLE"]]),
                        "sample_thread_cpu_ns":stats([x[2] for x in rows["SAMPLE"]]),
                        "observer_wall_ns":stats([x[1] for x in rows["OBSERVER"]])})
        record["result"] = "checks complete; baseline removal failure retained" if record["removal_exit_code"] and args.label=="baseline" else "checks complete"
        record["elapsed_s"] = time.monotonic()-started
        save()
        (out/"SEALED.sha256").write_text(digest((out/"results.json").read_bytes())+"  results.json\n")
        print(json.dumps({"output":str(out),"header_blob":record["header_blob"],"removal_exit_code":record["removal_exit_code"],"elapsed_s":record["elapsed_s"]}))
        return 0
    except Exception as exc:
        record["result"]="failed"; record["error"]=str(exc); save()
        print(str(exc), file=sys.stderr); return 1

if __name__ == "__main__":
    sys.exit(main())
