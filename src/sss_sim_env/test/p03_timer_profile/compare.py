#!/usr/bin/env python3
"""Compare sealed matched source probes; never infer robot/ROS performance."""
import argparse
import hashlib
import json
import math
from pathlib import Path

def load(path):
    data = path.read_bytes()
    seal = (path.parent/"SEALED.sha256").read_text().split()[0]
    if hashlib.sha256(data).hexdigest() != seal:
        raise ValueError("result seal mismatch: " + str(path))
    return json.loads(data)

def quantiles(rows, column):
    values = sorted(row[column] for row in rows)
    return {"n":len(values), "p50":values[math.ceil(.5*len(values))-1],
            "p95":values[math.ceil(.95*len(values))-1], "max":values[-1]}

def compare(a, b):
    if a["label"] != "baseline" or b["label"] != "candidate":
        raise ValueError("expected baseline then candidate")
    for key in ("schema", "scope", "platform", "cpu_affinity", "sizes_timers_not_robots",
                "seeds", "warmup_updates", "samples_per_case", "test_source_sha256", "sanitizers"):
        if a[key] != b[key]: raise ValueError("unmatched " + key)
    if a["sanitizers"]: raise ValueError("sanitizer runs are not timing comparisons")
    if a["header_blob"] != "57ca4920b0097ef9232d230361a4e541716556fc":
        raise ValueError("unexpected baseline")
    if b["removal_exit_code"] or a["edges_exit_code"] or b["edges_exit_code"]:
        raise ValueError("candidate removal or deterministic edge gate failed")
    ac = {(c["mode"], c["timers"], c["seed"]):c for c in a["cases"]}
    bc = {(c["mode"], c["timers"], c["seed"]):c for c in b["cases"]}
    if ac.keys() != bc.keys(): raise ValueError("unmatched cases")
    for key, x in ac.items():
        y = bc[key]
        if x["mode"] == "semantics":
            if x["exit_code"] or y["exit_code"] or x["stdout"] != y["stdout"]:
                raise ValueError("fixed-membership trace mismatch")
        elif x["metadata"]["digest"] != y["metadata"]["digest"]:
            raise ValueError("benchmark request trace mismatch")
    result = {"scope": a["scope"], "source_blobs":[a["header_blob"],b["header_blob"]],
              "quantile":a["quantile"], "units":"ns; 303 observations per timer count per variant",
              "not_measured":["robots","QP CPU","queue wait","RTF","deadline misses","per-agent RSS"],
              "timers":{}}
    for n in a["sizes_timers_not_robots"]:
        sides=[]
        for source in (a,b):
            cases = [c for c in source["cases"] if c["mode"]=="bench" and c["timers"]==n]
            rows = sum((c["SAMPLE"] for c in cases),[])
            observer = sum((c["OBSERVER"] for c in cases),[])
            sides.append({"wall_ns":quantiles(rows,1),"thread_cpu_ns":quantiles(rows,2),
                          "observer_wall_ns":quantiles(observer,1),
                          "process_ru_maxrss_kib": [c["metadata"]["max_rss_kib"] for c in cases]})
        result["timers"][n]={"baseline":sides[0],"candidate":sides[1]}
    return result

if __name__ == "__main__":
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("baseline",type=Path); p.add_argument("candidate",type=Path)
    args=p.parse_args()
    print(json.dumps(compare(load(args.baseline),load(args.candidate)),indent=2)+"\n",end="")
