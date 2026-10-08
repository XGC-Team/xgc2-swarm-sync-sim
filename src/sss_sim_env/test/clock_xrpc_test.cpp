#include "sss_sim_env/ClockControl.hpp"
#include <boost/json.hpp>
#include <atomic>
#include <cassert>
#include <filesystem>
#include <future>
#include <iostream>
#include <thread>
#include <unistd.h>
namespace j = boost::json;
using namespace xgc2::xrpc;
using namespace sss_utils;
struct Fixture {
  ClockDomain domain;
  ClockControl control;
  std::atomic<bool> stop{false}, hold{false};
  std::thread owner;
  explicit Fixture(const std::string& path, RuntimePolicyOptions options = {}) : control(path, domain.snapshot(), std::move(options)) {
    owner = std::thread([this] {
      while (!stop.load()) {
        if (!hold.load()) {
          control.process([this](const ClockCommand& c) {
            switch (c.kind) {
              case ClockCommand::Kind::Control: return domain.control(c.expected_revision, c.proceed, c.speed);
              case ClockCommand::Kind::Reset: return domain.reset(c.expected_revision, c.time_ns);
              case ClockCommand::Kind::Register: return domain.register_client();
              case ClockCommand::Kind::Unregister: return domain.unregister_client(c.client_id);
            }
            return ClockDomain::Result{};
          });
          control.publish(domain.snapshot());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  }
  ~Fixture() { stop.store(true); owner.join(); }
};
int main() {
  char tmp[] = "/tmp/sss-clock-xrpc.XXXXXX"; auto* dir = mkdtemp(tmp); assert(dir);
  const auto path = std::string(dir) + "/control.sock";
  uint64_t seq = 0;
  auto call = [&](HttpClient& client, std::string method, j::object payload, std::string id = "", int ms = 1000) {
    HttpRequest req; req.method = "POST"; req.target = "/v1/sss-clock/" + method;
    req.body = j::serialize(payload); req.request_id = id.empty() ? "test:" + std::to_string(++seq) : id;
    return client.call(req, Clock::now() + std::chrono::milliseconds(ms));
  };
  auto describe = [&](HttpClient& discovery) {
    HttpRequest req; req.method = "GET"; req.target = "/v1/sss-clock/describe";
    auto result = discovery.call(req, Clock::now() + std::chrono::seconds(1)); assert(result.status == 200);
    return std::string(j::parse(result.body).as_object().at("instance_id").as_string());
  };
  std::string first_instance;
  {
    Fixture fixture(path); HttpClient discovery(path); first_instance = describe(discovery);
    HttpClient client(path, {}, first_instance);
    auto status = j::parse(call(client, "status", {}).body).as_object();
    assert(status.at("paused").as_bool());
    auto desired = status.at("desired_revision").to_number<uint64_t>();
    assert(call(client, "control", {{"expected_revision", desired}, {"proceed", true}, {"max_speed_ratio", -1}}).status == 400);
    assert(call(client, "control", {{"expected_revision", desired + 1}, {"proceed", true}, {"max_speed_ratio", 1}}).status == 409);
    assert(call(client, "status", {{"unknown", true}}).status == 400);
    auto reg = call(client, "client/register", {}, "register-once"); assert(reg.status == 200);
    auto id = j::parse(reg.body).as_object().at("client_id").to_number<uint32_t>();
    auto replay = call(client, "client/register", {}, "register-once"); assert(replay.status == 200);
    assert(j::parse(replay.body).as_object().at("client_id").to_number<uint32_t>() == id);
    assert(j::parse(call(client, "status", {}).body).as_object().at("clients").to_number<uint32_t>() == 1);
    assert(call(client, "client/unregister", {{"client_id", id}}).status == 200);
    auto next = j::parse(call(client, "client/register", {}).body).as_object().at("client_id").to_number<uint32_t>();
    assert(next != id); assert(call(client, "client/unregister", {{"client_id", id}}).status == 404);
    for (int i = 1; i < 255; ++i) assert(call(client, "client/register", {}).status == 200);
    assert(call(client, "client/register", {}).status == 429);
    auto reset = call(client, "reset", {{"expected_revision", desired}, {"time_ns", 0}});
    assert(reset.status == 200);
    auto reset_state = j::parse(reset.body).as_object();
    assert(reset_state.at("paused").as_bool()); assert(reset_state.at("clients").to_number<int>() == 0);
    assert(call(client, "client/unregister", {{"client_id", next}}).status == 404);
    desired = reset_state.at("desired_revision").to_number<uint64_t>();
    fixture.hold.store(true); std::this_thread::sleep_for(std::chrono::milliseconds(20));
    bool expired = false;
    try { call(client, "control", {{"expected_revision", desired}, {"proceed", true}, {"max_speed_ratio", 1}}, "cancel-before-native", 20); }
    catch (const HttpCallError&) { expired = true; }
    assert(expired);
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); fixture.hold.store(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    HttpClient check(path, {}, first_instance);
    auto unchanged = j::parse(call(check, "status", {}).body).as_object();
    assert(unchanged.at("desired_revision").to_number<uint64_t>() == desired);
    assert(unchanged.at("paused").as_bool());
    // Burst beyond the fixed listener/admission budget while the native owner
    // is held. Every expired command must release without a native effect.
    fixture.hold.store(true); std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::vector<std::future<bool>> burst;
    for (int i = 0; i < 32; ++i) burst.push_back(std::async(std::launch::async, [&, i] {
      try {
        HttpClient overloaded(path, {}, first_instance);
        HttpRequest req; req.method = "POST"; req.target = "/v1/sss-clock/client/register";
        req.body = "{}"; req.request_id = "burst:" + std::to_string(i);
        const auto result = overloaded.call(req, Clock::now() + std::chrono::milliseconds(100));
        return result.status != 200;
      } catch (const HttpCallError&) { return true; }
    }));
    for (auto& item : burst) assert(item.get());
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); fixture.hold.store(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(j::parse(call(check, "status", {}).body).as_object().at("clients").to_number<int>() == 0);
    // An observation is held and released by a native state transition.
    auto rev = unchanged.at("revision").to_number<uint64_t>();
    auto observation = std::async(std::launch::async, [&] {
      HttpClient observer(path, {}, first_instance);
      HttpRequest req; req.method = "POST"; req.target = "/v1/sss-clock/observe";
      req.body = j::serialize(j::object{{"after_revision", rev}}); req.request_id = "held-observer";
      return observer.call(req, Clock::now() + std::chrono::seconds(1));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(observation.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
    auto config = call(check, "control", {{"expected_revision", desired}, {"proceed", true}, {"max_speed_ratio", 1}});
    assert(config.status == 200); assert(observation.get().status == 200);
    auto applied = j::parse(config.body).as_object();
    assert(applied.at("desired_revision") == applied.at("applied_revision"));
  }
  assert(!std::filesystem::exists(path));
  {
    Fixture restarted(path); HttpClient discovery(path); auto second = describe(discovery); assert(second != first_instance);
    HttpClient old(path, {}, first_instance);
    bool fenced = false;
    try { fenced = call(old, "client/register", {}).status == 409; }
    catch (const HttpCallError& e) { fenced = e.code == "conflict"; }
    assert(fenced);
    HttpClient fresh(path, {}, second);
    assert(j::parse(call(fresh, "status", {}).body).as_object().at("clients").to_number<int>() == 0);
  }
  for (const auto& field : std::vector<std::pair<std::string,std::string>>{
      {"XGC2_XRPC_UNKNOWN", "1"}, {"XGC2_XRPC_HOST_MAX_CONNECTIONS", "32"},
      {"XGC2_XRPC_CALL_TIMEOUT_MS", ""}, {"XGC2_XRPC_LOG_LEVEL", "debug"}}) {
    RuntimePolicyOptions options; options.environment.push_back(field); bool rejected = false;
    try { Fixture invalid(path, options); }
    catch (const RuntimePolicyError&) { rejected = true; }
    assert(rejected); assert(!std::filesystem::exists(path));
  }
  {
    RuntimePolicyOptions options; options.environment.emplace_back("XGC2_XRPC_HOST_MAX_CONNECTIONS", "8");
    Fixture configured(path, options); HttpClient discovery(path); auto instance = describe(discovery);
    HttpClient client(path, {}, instance);
    auto effective = j::parse(call(client, "effective-policy", {}).body).as_object(); bool found = false;
    for (const auto& item : effective.at("fields").as_array()) {
      const auto& field = item.as_object();
      if (field.at("name").as_string() == "HOST_MAX_CONNECTIONS") {
        found = true; assert(field.at("value").to_number<int>() == 8);
        assert(field.at("source").as_string() == "environment"); assert(field.at("ceiling").to_number<int>() == 16);
      }
    }
    assert(found);
  }
  // The real native clock cannot advance while paused or when all algorithms
  // are inactive; reset fences old data handles and backdated data is rejected.
  ClockDomain domain(1, false); auto client = domain.register_client();
  assert(domain.request(client.client_id, 10000000)); domain.tick(); assert(domain.snapshot().time_ns == 0);
  assert(domain.control(1, true, 1).error == ClockDomain::Error::None);
  domain.tick(); assert(domain.snapshot().time_ns == 10000000);
  assert(!domain.request(client.client_id, 1));
  assert(domain.request(client.client_id, ClockDomain::kMaxTime)); domain.tick();
  assert(domain.snapshot().time_ns == 10000000);
  auto revision = domain.snapshot().desired_revision;
  assert(domain.reset(revision, 0).error == ClockDomain::Error::None);
  assert(!domain.request(client.client_id, 100));
  std::filesystem::remove_all(dir);
  std::cout << "SSS native XRPC: revision/reset/generation/capacity/dedup/cancel/observe/restart passed\n";
}
