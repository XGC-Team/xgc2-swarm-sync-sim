# P03: next-time selection source probe

Tracks XGC-Team/xgc2-harness#154. This is an offline **source microbenchmark** and
single-thread semantic probe of the real `sss_sim_env/Timer.hpp`. It is not a ROS
build, a flight simulation, a QP profile, or evidence that 100 robots meet deadlines.
The N axis is timers in **one** TimerManagerExtra/process, not robots. Separate
agent processes may each have very few timers; do not extrapolate the large-N gain.

The candidate replaces only the full sort used to select the next request with
`min_element`; the sorting/invalidation in `cb_clock` is unchanged. No physics,
periods, solver options, precision, or scientific logging configuration changes.
For fixed membership, selection emits the same minimum timestamp under ties,
zero, infinity, incomplete readiness, and modeled boundary failure. The preexisting
sequential `remove_timer_info` bug is intentionally a negative baseline test:
with A=1, B=2, C=3, removing A sorts A to the end but erases the iterator's old
position (B). Updating B then returns the old minimum 2 instead of 3. The candidate
avoids that reordering. **Concurrent removal/addition/clock delivery remains Open**:
remove_timer_info's unlocked find/erase is not made thread-safe by this patch.

## Replay (Linux, Python 3, g++)

Run from the SSS repo root. Select one allowed CPU; keep it identical for all runs.
Use a new output directory for every attempt. Each subprocess has a 30 s timeout;
each runner attempt has a 120 s elapsed-budget guard. Output is never overwritten.

```sh
TEST=src/sss_sim_env/test/p03_timer_profile
CPU=$(python3 -c 'import os; print(min(os.sched_getaffinity(0)))')
WORK=$(mktemp -d)
git show cd1aa7a5be0b28d8bb4b9c70bc2119a141eeef41:src/sss_sim_env/include/sss_sim_env/Timer.hpp > "$WORK/baseline.hpp"
python3 "$TEST/run.py" --header "$WORK/baseline.hpp" --output "$WORK/baseline" --label baseline --cpu "$CPU"
python3 "$TEST/run.py" --header src/sss_sim_env/include/sss_sim_env/Timer.hpp --output "$WORK/candidate" --label candidate --cpu "$CPU"
python3 "$TEST/compare.py" "$WORK/baseline/results.json" "$WORK/candidate/results.json" > "$WORK/comparison.json"
python3 "$TEST/run.py" --header src/sss_sim_env/include/sss_sim_env/Timer.hpp --output "$WORK/asan" --label candidate --cpu "$CPU" --sanitizers
```

The baseline header's Git blob must be `57ca4920b0097ef9232d230361a4e541716556fc`.
When git transport is unavailable, `--header` may point to an API-materialized
file with that exact blob. This does not establish a full repository checkout.
Do not install the generated include wrappers or boundary.hpp into a ROS workspace.
The spy models call observation and a controllable return value, not transport,
ClockUpdater deduplication, real ROS thread scheduling, or service registration.

Seeds are 154, 20260924, 20260925; N=1,8,32,100. Each semantic case checks 20,001
operations against a map-based specification oracle. Deterministic edges cover
empty/unknown handles, partial readiness, infinity/zero/ties, failures and retries.
The removal test's baseline exit 2 is retained in results; it is **not** a passing
baseline regression. Each timing case has 2,000 warmups and 101 measured updates
on a ready manager, using precomputed seeded inputs. This is a selection-heavy
workload; it does not estimate the fraction of wall time spent ready in real ROS.
A second pair in reverse execution order is useful to expose order effects.

Wall timing uses CLOCK_MONOTONIC; CPU uses CLOCK_THREAD_CPUTIME_ID. CPU observer
calls are inside the wall interval. The identical empty observer is also reported;
no subtraction or outlier removal occurs. Quantiles use nearest rank. At small N,
observer overhead dominates and a small gain or regression is not a robust claim.
Process `ru_maxrss` is KiB on this Linux probe and can include launcher high-water
accounting inherited across exec; it is not incremental algorithm/per-agent RSS.
The artifact records compiler flags, header/test/binary hashes, affinity, host and
cgroup limits, every command/exit code, raw stdout/stderr, samples, and a result seal.
Sanitizer runs are not benchmark data. Tests here are same-context self-checks;
an independent reviewer and a real ROS/SSS integration run remain required.

Paper-specific raw observations, rejected routes, dependencies and open gates are
kept in paper-dmpc at `research/tro-26-0979-v1/P03/20260924-timer-selection/`.
