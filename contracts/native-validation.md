# Native validation — 2026-10-09

Scope: isolated source/native runtime only. No live station, APT publication,
vehicle hardware or weak-network end-to-end claim.

SDK: owning common/xrpc work tree rooted at HEAD
`b80f5c8af0621c240634dc294c7c5d1bb8724403`, including then-uncommitted C++20
HTTP/runtime-policy implementation. Tests linked the installed snapshot
`/tmp/sol6-sync-sdk-v2-prefix`, not an assumed published SDK version.

| Installed input | SHA256 |
| --- | --- |
| libxgc2_xrpc_http.so.0.1.0 | 31543fe7819821e13e7e7e4e77f51de4b9a26a191ab6ffcb4332108062fc82b8 |
| libxgc2_xrpc_policy.so.0.1.0 | cb4339995f5be8a15799537f8df9c1cf94d75605cbb90a4a4c2c1860fcb860e5 |
| http.hpp | b14983e71c9d5ee2dd17d0b44ebe3da8a6199a828efb3a4c36e381f76204382c |
| runtime_policy.hpp | 171f3cfd857ac4c59f147184d176f25da8210f09a9b628135d08716a57ec2df7 |

Toolchain: Ubuntu Noble amd64 GCC 13.3, system Boost 1.83, installed native
Noetic libraries. Noetic's CMake export referenced absent liblog4cxx-dev
headers/linker symlink. Official Noble `liblog4cxx-dev 1.1.0-1build3` was
unpacked under `/tmp/sol6-ros-dependency`; a test-only relocated rosconsole
CMake export uses those headers and the existing native `liblog4cxx.so.15`.
No system package, live ROS graph or checked-out SDK source was changed.

Native package configurations passed `-DXgcXrpc_DIR=/tmp/sol6-sync-sdk-v2-prefix/lib/cmake/XgcXrpc`,
`-Drosconsole_DIR=/tmp/sol6-ros-dependency/rosconsole-cmake`,
`-DPYTHON_EXECUTABLE=/usr/bin/python3` and `-DCATKIN_ENABLE_TESTING=ON`.
The product's deployment metadata still targets Focal; these Noble builds do
not establish a Focal GCC9/C++20 or cross-suite ABI release.

Product migration baseline: `9956af1244305e3b6a1793c4d9bec0711c357838`.

`cmake --build /tmp/sol6-sss-build -j2` passed all native SSS targets. A dedicated
rostest master loaded the actual nodelet with two manager workers; real
`test/clock_ros_native.test` passed 1 test (0 errors, 0 failures). Output:
`/tmp/sol6-sss-native-rostest.log`. It checks pause/resume/reset `/clock`
postconditions, absent old services, retained user TimeRequest data, actual
ClockUpdater XRPC registration and 32 additional timers without additional
per-timer communication threads. Installed unrelated Noetic test-nodelet XMLs
log parse warnings; the owned SSS plugin loads/unloads successfully.

`cmake -S src/sss_sim_env/test/native -B /tmp/sol6-sss-conformance
-DXgcXrpc_DIR=/tmp/sol6-sync-sdk-v2-prefix/lib/cmake/XgcXrpc`, build and
`ctest --test-dir /tmp/sol6-sss-conformance --output-on-failure` passed 1 actual
SDK/Unix socket/native domain fixture. It covers 255-client overload, fixed
listener/command burst cancellation, revisions, strict fields, reset/stale
handles, duplicate request IDs, pre-dispatch cancellation, held observation,
restart fencing and runtime-policy invalid/unsupported/ceiling/source cases.
The same fixture passed ASan+UBSan in `/tmp/sol6-sss-asan`; product domain code
is instrumented, the installed SDK libraries are not, so this is not a fresh
sanitizer assertion about SDK internals. Vehicle variant integration, client
crash leases, cold worker lifecycle preparation and deployment logging/ABI
remain open. No GUI visual check or full SSS physics experiment is claimed.
