#include "sss_sim_env/ClockControl.hpp"
#include "sss_sim_env/ClockJson.hpp"
#include <future>
#include <stdexcept>
namespace sss_utils {
namespace {
namespace j = clock_json;
using namespace xgc2::xrpc;
HttpLimits control_limits() {
  HttpLimits limits;
  limits.connections = 16;
  limits.inflight = 16;
  limits.request_bytes = 4096;
  limits.response_bytes = 4096;
  limits.request_timeout = std::chrono::milliseconds(5000);
  limits.idle_timeout = std::chrono::milliseconds(5000);
  return limits;
}

Json::Value object(const HttpRequest& req, std::initializer_list<const char*> allowed) {
  auto value = j::parse(req.body);
  if (!value.isObject()) throw std::invalid_argument("JSON object required");
  for (const auto& key : value.getMemberNames()) {
    bool known = false;
    for (const auto* name : allowed) if (key == name) known = true;
    if (!known) throw std::invalid_argument("unknown field");
  }
  return value;
}
uint64_t number(const Json::Value& object, const char* name) {
  const auto& value = object[name];
  if (!object.isMember(name) ||
      (value.type() != Json::uintValue && (value.type() != Json::intValue || value.asInt64() < 0)))
    throw std::invalid_argument(std::string(name) + " must be a nonnegative integer");
  return value.asUInt64();
}
}
ClockControl::ClockControl(std::string path, ClockDomain::Snapshot initial)
    : snapshot_(initial), instance_id_(xgc2::xrpc::new_instance_id()) {
  std::promise<void> started; auto ready = started.get_future();
  io_ = std::thread([this, path = std::move(path), started = std::move(started)]() mutable {
    try {
      auto limits = control_limits();
      HttpServer server(UnixOptions{path}, [this](auto req, const HttpReply& reply) { handle(std::move(req), reply); },
        limits, HttpIdentity{instance_id_, {"/v1/sss-clock/describe"}});
      server.set_wakeup_handler([this] { flush(); });
      server_.store(&server); started.set_value(); server.run(); server_.store(nullptr);
    } catch (...) { server_.store(nullptr); try { started.set_exception(std::current_exception()); } catch (...) {} }
  });
  try { ready.get(); } catch (...) { io_.join(); throw; }
}
ClockControl::~ClockControl() {
  if (auto* s = server_.load()) s->request_stop();
  io_.join();
}
xgc2::xrpc::HttpResponse ClockControl::response(const Receipt* receipt) const {
  const auto& s = snapshot_;
  auto out = j::object({{"schema_version", 1}, {"instance_id", instance_id_}, {"revision", Json::UInt64(s.revision)},
    {"desired_revision", Json::UInt64(s.desired_revision)}, {"applied_revision", Json::UInt64(s.applied_revision)},
    {"persisted_revision", Json::Value()}, {"time_ns", Json::UInt64(s.time_ns)}, {"paused", s.paused},
    {"max_speed_ratio", s.max_speed_ratio}, {"clients", s.clients},
    {"waiting_clients", s.waiting_clients}, {"finite_clients", s.finite_clients},
    {"state", s.paused ? "paused" : ((!s.clients || s.waiting_clients) ? "waiting_for_clients" : (!s.finite_clients ? "idle" : "advancing"))}});
  int status = 200;
  if (receipt) {
    const auto& r = receipt->result;
    std::string code;
    switch (r.error) {
      case ClockDomain::Error::None: break;
      case ClockDomain::Error::Conflict: status = 409; code = "conflict"; break;
      case ClockDomain::Error::Invalid: status = 400; code = "invalid_argument"; break;
      case ClockDomain::Error::NotFound: status = 404; code = "not_found"; break;
      case ClockDomain::Error::Exhausted: status = 429; code = "resource_exhausted"; break;
    }
    out["receipt"] = j::object({{"operation_id", receipt->id}, {"state", code.empty() ? "succeeded" : "rejected"},
      {"desired_revision", Json::UInt64(r.snapshot.desired_revision)}, {"failure_stage", code.empty() ? "" : "apply"}});
    out["client_id"] = r.client_id;
    if (!code.empty()) out["error"] = j::object({{"code", code}, {"message", "clock precondition failed"}});
  }
  return {status, {{"Content-Type", "application/json"}}, j::serialize(out)};
}
void ClockControl::handle(HttpRequest req, const HttpReply& reply) {
  try {
    if (req.method == "GET" && req.target == "/v1/sss-clock/describe") {
      reply.complete({200, {{"Content-Type", "application/json"}}, j::serialize(j::Object{
        {"schema_version", 1}, {"instance_id", instance_id_}, {"service", "sss-clock"},
        {"clients", 255}, {"commands", 16}, {"observers", 16}, {"receipts", 64}, {"persistence", false}})}); return;
    }
    if (req.method != "POST") { reply.complete(http_error(404, "not_found", "unknown operation")); return; }
    if (req.target == "/v1/sss-clock/status") {
      object(req, {}); std::lock_guard lock(mutex_); reply.complete(response()); return;
    }
    if (req.target == "/v1/sss-clock/receipt") {
      auto o = object(req, {"operation_id"}); const auto& id = o["operation_id"];
      if (!id.isString() || id.asString().size() > 128) throw std::invalid_argument("operation_id required");
      std::lock_guard lock(mutex_);
      for (const auto& r : receipts_) if (r.id == id.asString()) { reply.complete(response(&r)); return; }
      reply.complete(http_error(404, "not_found", "receipt expired or unknown")); return;
    }
    if (req.target == "/v1/sss-clock/observe") {
      auto o = object(req, {"after_revision"}); auto after = number(o, "after_revision");
      std::lock_guard lock(mutex_);
      if (after > snapshot_.revision) { reply.complete(http_error(409, "conflict", "future revision")); return; }
      if (after < snapshot_.revision) { reply.complete(response()); return; }
      for (auto& w : observers_) if (!w.active || w.reply.cancelled()) { w = {true, after, reply}; return; }
      reply.complete(http_error(429, "resource_exhausted", "observers full")); return;
    }
    ClockCommand c;
    if (req.target == "/v1/sss-clock/control") {
      auto o = object(req, {"expected_revision", "proceed", "max_speed_ratio"});
      c.expected_revision = number(o, "expected_revision");
      const auto& proceed = o["proceed"]; const auto& speed = o["max_speed_ratio"];
      if (!proceed.isBool() || !speed.isNumeric()) throw std::invalid_argument("proceed and max_speed_ratio required");
      c.proceed = proceed.asBool(); c.speed = speed.asDouble();
      if (!std::isfinite(c.speed) || c.speed < 0.1 || c.speed > 100) throw std::invalid_argument("max_speed_ratio must be in [0.1,100]");
    } else if (req.target == "/v1/sss-clock/reset") {
      auto o = object(req, {"expected_revision", "time_ns"}); c.kind = ClockCommand::Kind::Reset;
      c.expected_revision = number(o, "expected_revision"); c.time_ns = number(o, "time_ns");
      if (c.time_ns > ClockDomain::kMaxTime) throw std::invalid_argument("time_ns outside ROS time range");
    } else if (req.target == "/v1/sss-clock/client/register") {
      object(req, {}); c.kind = ClockCommand::Kind::Register;
    } else if (req.target == "/v1/sss-clock/client/unregister") {
      auto o = object(req, {"client_id"}); auto id = number(o, "client_id");
      if (id > UINT32_MAX || !id) throw std::invalid_argument("client_id outside range");
      c.kind = ClockCommand::Kind::Unregister; c.client_id = uint32_t(id);
    } else { reply.complete(http_error(404, "not_found", "unknown operation")); return; }
    std::lock_guard lock(mutex_);
    for (const auto& r : receipts_) if (r.id == req.request_id) {
      reply.complete(r.fingerprint == req.target + req.body ? response(&r) : http_error(409, "conflict", "request ID reused")); return;
    }
    for (const auto& s : slots_) if (s.state != Slot::Free && s.id == req.request_id) {
      reply.complete(http_error(409, "conflict", "request ID already in flight")); return;
    }
    for (auto& s : slots_) if (s.state == Slot::Free) {
      s.command = c; s.reply = reply; s.id = req.request_id; s.fingerprint = req.target + req.body;
      s.state = Slot::Queued; return;
    }
    reply.complete(http_error(429, "resource_exhausted", "native command slots full"));
  } catch (const std::invalid_argument& e) { reply.complete(http_error(400, "invalid_argument", e.what())); }
}
void ClockControl::process(const std::function<ClockDomain::Result(const ClockCommand&)>& apply) {
  // One native producer and one IO consumer per slot. Payload/result ownership
  // crosses only release/acquire state transitions; native work never waits
  // for the JSON/IO owner's mutex or destroys cold request storage.
  for (auto& slot : slots_) {
    auto queued = Slot::Queued;
    if (!slot.state.compare_exchange_strong(queued, Slot::Executing, std::memory_order_acq_rel)) continue;
    if (slot.reply.cancelled()) slot.state.store(Slot::Cancelled, std::memory_order_release);
    else {
      slot.result = apply(slot.command);
      slot.state.store(Slot::Ready, std::memory_order_release);
    }
    if (auto* s = server_.load()) s->wake();
  }
}
bool ClockControl::publish(ClockDomain::Snapshot snapshot) {
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock) return false;
  snapshot_ = snapshot; lock.unlock();
  if (auto* s = server_.load()) s->wake();
  return true;
}
void ClockControl::flush() {
  std::lock_guard lock(mutex_);
  for (auto& s : slots_) if (s.state.load(std::memory_order_acquire) == Slot::Ready) {
    auto& r = receipts_[next_receipt_++ % receipts_.size()]; r = {s.id, s.fingerprint, s.result};
    if (s.result.snapshot.revision >= snapshot_.revision) snapshot_ = s.result.snapshot;
    s.reply.complete(response(&r));
    s.reply = {}; s.id.clear(); s.fingerprint.clear();
    s.state.store(Slot::Free, std::memory_order_release);
  }
  for (auto& s : slots_) if (s.state.load(std::memory_order_acquire) == Slot::Cancelled) {
    s.reply = {}; s.id.clear(); s.fingerprint.clear();
    s.state.store(Slot::Free, std::memory_order_release);
  }
  for (auto& w : observers_) if (w.active) {
    if (w.reply.cancelled()) w = {};
    else if (snapshot_.revision > w.after) { w.reply.complete(response()); w = {}; }
  }
}
}
