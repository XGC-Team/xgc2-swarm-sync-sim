#pragma once
#include "ClockDomain.hpp"
#include <xgc2/xrpc/http.hpp>
#include <xgc2/xrpc/runtime_policy.hpp>
#include <atomic>
#include <array>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace sss_utils {
struct ClockCommand {
  enum class Kind { Control, Reset, Register, Unregister } kind = Kind::Control;
  uint64_t expected_revision = 0, time_ns = 0;
  uint32_t client_id = 0;
  bool proceed = false;
  double speed = 100;
};
class ClockControl {
public:
  explicit ClockControl(std::string path, ClockDomain::Snapshot initial, xgc2::xrpc::RuntimePolicyOptions options = {});
  ~ClockControl();
  void process(const std::function<ClockDomain::Result(const ClockCommand&)>& apply);
  bool publish(ClockDomain::Snapshot snapshot);
private:
  struct Slot {
    enum State { Free, Queued, Executing, Ready, Cancelled };
    std::atomic<State> state{Free};
    ClockCommand command;
    ClockDomain::Result result;
    xgc2::xrpc::HttpReply reply;
    std::string id, fingerprint;
  };
  struct Receipt { std::string id, fingerprint; ClockDomain::Result result; };
  struct Observer { bool active = false; uint64_t after = 0; xgc2::xrpc::HttpReply reply; };
  void handle(xgc2::xrpc::HttpRequest req, xgc2::xrpc::HttpReply reply);
  void flush();
  xgc2::xrpc::HttpResponse response(const Receipt* receipt = nullptr) const;
  std::array<Slot, 16> slots_{};
  std::array<Receipt, 64> receipts_{};
  std::array<Observer, 16> observers_{};
  size_t next_receipt_ = 0;
  ClockDomain::Snapshot snapshot_;
  std::mutex mutex_;
  std::thread io_;
  std::atomic<xgc2::xrpc::HttpServer*> server_{nullptr};
  const xgc2::xrpc::RuntimePolicy policy_;
  const std::string instance_id_;
};
}
