# Native clock validation

The clock domain and bounded XRPC host are in the existing simulation nodelet.
Control uses one HTTP IO owner, 16 command slots, 16 observation slots and 64
receipts. Algorithms publish time requests through the existing ROS data plane;
ClockUpdater shares one native client per process. Completed/cancelled replies
are released when their native work has ended.

Current build uses the published Focal full 1.0.8 image, Clang 10, distribution
JsonCpp and the XRPC C++ HTTP SDK. Python UI uses the formal wheel on Python
>=3.8 with PyQt/ROS modules in the same interpreter. No Boost.JSON, alternate
Python installation or gRPC component is required by this clock product.

Existing native CTest passed against the current SDK in the controlled Focal
image. The existing ROS native test also passed there, including the library
consumer, pause/resume/reset, stale data handles and bounded timer threading.
The test supplies its own private 0700 socket directory. Product launch still
requires an explicit owner-provided `control_socket`.

These results cover native source and its private ROS graph. They do not claim
central APT publication, station installation or an Experiment lifecycle pass.
