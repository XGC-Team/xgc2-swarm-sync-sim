#include "sss_sim_env/ClockControl.hpp"
#include <boost/json.hpp>
#include <future>
#include <stdexcept>
namespace sss_utils {
namespace {
namespace j = boost::json;
using namespace xgc2::xrpc;
RuntimePolicy product_policy(RuntimePolicyOptions options) {
  options.defaults["HOST_MAX_CONNECTIONS"] = "16";
  options.defaults["HOST_MAX_IN_FLIGHT"] = "16";
  options.defaults["MAX_REQUEST_BYTES"] = "4096";
  options.defaults["MAX_RESPONSE_BYTES"] = "4096";
  options.defaults["CALL_TIMEOUT_MS"] = "5000";
  options.defaults["IDLE_TIMEOUT_MS"] = "5000";
  options.default_source = "sss-clock";
  options.ceilings["HOST_MAX_CONNECTIONS"] = 16;
  options.ceilings["HOST_MAX_IN_FLIGHT"] = 16;
  options.ceilings["MAX_REQUEST_BYTES"] = 4096;
  options.ceilings["MAX_RESPONSE_BYTES"] = 4096;
  return resolve_runtime_policy(options);
}
HttpResponse policy_response(const RuntimePolicy& policy) {
  j::array fields;
  for (const auto& f : policy.fields()) {
    j::object field{{"name", std::string(f.name)}, {"source", std::string(f.source)},
      {"source_detail", f.source_detail}, {"dynamic", f.dynamic}, {"unit", std::string(f.unit)}};
    std::visit([&](const auto& value) { field["value"] = value; }, f.value);
    field["ceiling"] = f.ceiling ? j::value(*f.ceiling) : j::value(nullptr);
    fields.push_back(std::move(field));
  }
  return {200, {{"Content-Type", "application/json"}}, j::serialize(j::object{{"revision", policy.revision()}, {"fields", std::move(fields)}})};
}

j::object object(const HttpRequest& req, std::initializer_list<const char*> allowed) {
  j::error_code ec; auto v = j::parse(req.body, ec);
  if (ec || !v.is_object()) throw std::invalid_argument("JSON object required");
  auto o = v.as_object();
  for (const auto& f : o) {
    bool known = false; for (auto* name : allowed) if (f.key() == name) known = true;
    if (!known) throw std::invalid_argument("unknown field");
  }
  return o;
}
uint64_t number(const j::object& o, const char* name) {
  auto* v = o.if_contains(name);
  if (!v || !(v->is_uint64() || (v->is_int64() && v->as_int64() >= 0)))
    throw std::invalid_argument(std::string(name) + " must be a nonnegative integer");
  return v->is_uint64() ? v->as_uint64() : uint64_t(v->as_int64());
}
}
ClockControl::ClockControl(std::string path, ClockDomain::Snapshot initial, RuntimePolicyOptions options)
    : snapshot_(initial), policy_(product_policy(std::move(options))), instance_id_(xgc2::xrpc::new_instance_id()) {
  std::promise<void> started; auto ready = started.get_future();
  io_ = std::thread([this, path = std::move(path), started = std::move(started)]() mutable {
    try {
      auto limits = http_limits(policy_);
      HttpServer server(UnixOptions{path}, [this](auto req, auto reply) { handle(std::move(req), reply); },
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
  j::object out{{"schema_version", 1}, {"instance_id", instance_id_}, {"revision", s.revision},
    {"desired_revision", s.desired_revision}, {"applied_revision", s.applied_revision},
    {"persisted_revision", nullptr}, {"time_ns", s.time_ns}, {"paused", s.paused},
    {"max_speed_ratio", s.max_speed_ratio}, {"clients", s.clients},
    {"waiting_clients", s.waiting_clients}, {"finite_clients", s.finite_clients},
    {"state", s.paused ? "paused" : ((!s.clients || s.waiting_clients) ? "waiting_for_clients" : (!s.finite_clients ? "idle" : "advancing"))}};
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
    out["receipt"] = j::object{{"operation_id", receipt->id}, {"state", code.empty() ? "succeeded" : "rejected"},
      {"desired_revision", r.snapshot.desired_revision}, {"failure_stage", code.empty() ? "" : "apply"}};
    out["client_id"] = r.client_id;
    if (!code.empty()) out["error"] = j::object{{"code", code}, {"message", "clock precondition failed"}};
  }
  return {status, {{"Content-Type", "application/json"}}, j::serialize(out)};
}
void ClockControl::handle(HttpRequest req, HttpReply reply) {
  try {
    if (req.method == "GET" && req.target == "/v1/sss-clock/describe") {
      reply.complete({200, {{"Content-Type", "application/json"}}, j::serialize(j::object{
        {"schema_version", 1}, {"instance_id", instance_id_}, {"service", "sss-clock"},
        {"clients", 255}, {"commands", 16}, {"observers", 16}, {"receipts", 64}, {"persistence", false}})}); return;
    }
    if (req.method != "POST") { reply.complete(http_error(404, "not_found", "unknown operation")); return; }
    if (req.target == "/v1/sss-clock/effective-policy") {
      object(req, {}); reply.complete(policy_response(policy_)); return;
    }
    if (req.target == "/v1/sss-clock/status") {
      object(req, {}); std::lock_guard lock(mutex_); reply.complete(response()); return;
    }
    if (req.target == "/v1/sss-clock/receipt") {
      auto o = object(req, {"operation_id"}); auto* id = o.if_contains("operation_id");
      if (!id || !id->is_string() || id->as_string().size() > 128) throw std::invalid_argument("operation_id required");
      std::lock_guard lock(mutex_);
      for (const auto& r : receipts_) if (r.id == id->as_string()) { reply.complete(response(&r)); return; }
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
      auto* proceed = o.if_contains("proceed"); auto* speed = o.if_contains("max_speed_ratio");
      if (!proceed || !proceed->is_bool() || !speed || !speed->is_number()) throw std::invalid_argument("proceed and max_speed_ratio required");
      c.proceed = proceed->as_bool(); c.speed = speed->to_number<double>();
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
