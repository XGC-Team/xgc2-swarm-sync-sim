# SSS Clock v1

`sss_sim_env/sim_clock_nodelet` owns one clock domain and XRPC endpoint for the
nodelet lifetime. A fixed native callback owner serializes time requests,
speed regulation and management commands. The HTTP SDK has one separate IO
owner; JSON parsing and encoding stay there. There is no per-client listener
or spinner. Unload stops timers/subscription, joins the native owner, then
stops/joins HTTP and releases the owned socket. ROS `/clock` and user algorithm
`TimeRequest` data stay ROS; ROS clock-control/register/unregister services and
their generated .srv interfaces have been removed.

Build: C++20/Clang 10 in the published Focal full builder, distribution JsonCpp,
`XgcXrpc::http`, native ROS1/rosgraph_msgs/nodelet. The public ClockUpdater
facade does not expose SDK headers into user algorithm hot code.

The process owner grants an existing private runtime directory and supplies
`control_socket` to `sim_clock.launch`. The immutable bootstrap path is passed
through `/sss_clock/control_socket` to algorithm processes; each process reads
it once and discovers a fresh instance via `GET /v1/sss-clock/describe`.
Subsequent calls use an explicitly instance-bound shared C++ SDK client. UI
uses the official Python Runtime/Client. No call auto-starts the provider.

All operations below are POST JSON with SDK identity/deadline. Unknown fields
fail. All native mutation responses carry a receipt with the request ID as
operation ID; native completion is required before `succeeded`.

| Method under `/v1/sss-clock/` | Exact request |
| --- | --- |
| `status` | `{}` |
| `effective-policy` | `{}` |
| `control` | `{"expected_revision":1,"proceed":true,"max_speed_ratio":1.0}` |
| `reset` | `{"expected_revision":2,"time_ns":0}` |
| `client/register` | `{}` |
| `client/unregister` | `{"client_id":257}` |
| `receipt` | `{"operation_id":"control-request-1"}` |
| `observe` | `{"after_revision":12}` |

Speed is finite and in [0.1,100]. Zero never means retain/unlimited. Revisions
are unsigned integers; time is a nonnegative integer in native ROS time range
[0,4294967295999999999] ns. A successful control sets native pause/speed state
and updates the regulator timer before completion. Reset pauses, sets native
clock time, clears all registered client records and fences their handles,
then publishes the new `/clock` value. It does not claim to reset vehicle
physics; only the clock domain is owned by this component.

Example actual control result:

```json
{"schema_version":1,"instance_id":"fresh-instance","revision":2,
 "desired_revision":2,"applied_revision":2,"persisted_revision":null,
 "time_ns":0,"paused":false,"max_speed_ratio":1.0,"clients":0,
 "state":"waiting_for_clients","waiting_clients":0,"finite_clients":0,
 "receipt":{"operation_id":"control-request-1","state":"succeeded",
 "desired_revision":2,"failure_stage":""},"client_id":0}
```

`state` distinguishes manual pause, waiting for missing client requests,
all-inactive idle and finite-request advancement. Status/observe use maintained
native snapshots, no ROS graph probe. `observe` holds until native revision
advances, has a finite SDK deadline and rejects future revisions.

Client storage is 256 contiguous fixed slots including the reserved regulator;
at most 255 algorithm clients can register. Public uint32 handles encode a
24-bit generation and an 8-bit slot. Removed/reset/reused-slot handles cannot
update or remove a replacement client; generation exhaustion retires the slot.
Time requests never rewind native time and cannot advance while paused or when
all clients request inactive/infinite time. The ROS data queue is bounded to
1024 requests. The native hot path performs no HTTP, JSON or transport retry.
Management crosses 16 preallocated command/result slots through acquire/release
state transitions; native publication uses try-lock and retries on its existing
control tick instead of waiting for the JSON owner.

There are 16 connections, 16 in-flight calls, 16 native commands, 16 held
observations, 4 KiB requests/responses and 64 in-memory mutation receipts.
Queue/connection/receipt work remains owned until native completion or observed
pre-dispatch cancellation. Same ID plus same route/body returns the retained
result, changed content or concurrent reuse conflicts. Old terminal receipts
expire after 64 newer results; process restart clears receipts and changes
instance identity. Durable replay/exactly-once is not provided.

`ClockUpdater` uses one process-shared SDK connection and no retry loops.
Its destructor attempts bounded (200 ms) unregister; native thread-local
updaters are released at thread exit instead of accumulating thread IDs.
TimerManagerExtra owns one process-wide data spinner; adding timers does not
add spinners. Timer records have a 255-record cap, synchronized removal and
finite startup data-plane handshake. Algorithm time publication does not wait
for XRPC. Cold creation/registration of an updater still occurs when a native
thread first uses the facade; hard-realtime users must explicitly warm that
lifecycle before their real-time loop. This remains a migration gate for
legacy lazy worker initialization, not a promise of hard-real-time support.

A crashed algorithm that did not unregister can leave a missing clock client;
status reports waiting and the native reset releases/fences all clients.
Automatic client leases/heartbeat recovery are not implemented. The cache does
not claim crash-atomic registration. Weak-network/crash lease recovery and all
simulator vehicle variants are separate remaining product gates.

No desired configuration, receipts or health are saved to disk. Each world
instance has fresh handles/revisions. Socket/SDK lease lock use only the granted
runtime allocation; the process supervisor cleans that allocation after stop
and owns bounded stderr/ROS logs. Production ROS_HOME/ROS_LOG_DIR allocations,
shipped SDK/ABI packages remain deployment gates. The composition root snapshots XGC2_XRPC_ inputs once and uses the formal C++ RuntimePolicy resolver; `effective-policy` exposes values/sources/ceilings. Unknown, empty, unsupported and over-ceiling settings reject startup. This HTTP host does not claim diagnostics, client-registry or gRPC policy enforcement. Tests grant fresh transient paths and never touch the live station.

Evidence: `test/native` is a ROS-independent real SDK/socket fixture over the
same native ClockDomain and command host. It checks revisions, strict fields,
255-client exhaustion, deduplication, stale generations/reset, pre-dispatch
cancellation, held observations, restart fencing and clock pause/inactivity.
`test/clock_ros_native.test` loads the real native nodelet and verifies the old
ROS control services are absent, XRPC config/reset has actual `/clock`
postconditions, TimeRequest remains usable, the real ClockUpdater consumer
registers/unregisters, and 32 extra timers do not add communication threads.
