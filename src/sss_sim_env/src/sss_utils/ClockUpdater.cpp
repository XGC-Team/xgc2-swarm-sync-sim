#include "sss_sim_env/ClockUpdater.hpp"
#include <xgc2/xrpc/http.hpp>
#include "sss_sim_env/ClockJson.hpp"
#include <stdexcept>
namespace sss_utils {
namespace j = sss_utils::clock_json;
xgc2::xrpc::HttpClient& ClockUpdater::control_client() {
  // Explicit bootstrap path read once by this process; not a mutable ROS
  // control authority. A restart requires an explicit new binding/client.
  static auto client = [] {
    ros::NodeHandle nh; std::string path;
    if (!nh.getParam("/sss_clock/control_socket", path) || path.empty())
      throw std::invalid_argument("SSS control_socket bootstrap binding required");
    xgc2::xrpc::HttpClient discovery(path);
    xgc2::xrpc::HttpRequest req; req.method = "GET"; req.target = "/v1/sss-clock/describe";
    auto result = discovery.call(req, xgc2::xrpc::Clock::now() + std::chrono::seconds(2));
    if (result.status != 200) throw std::runtime_error("SSS control discovery rejected");
    auto instance = std::string(j::parse(result.body)["instance_id"].asString());
    return std::make_unique<xgc2::xrpc::HttpClient>(path, xgc2::xrpc::HttpLimits{}, instance);
  }();
  return *client;
}
std::string ClockUpdater::next_request_id() {
  static const std::string prefix = xgc2::xrpc::new_instance_id();
  static std::atomic<uint64_t> seq{0}; return prefix + ":" + std::to_string(++seq);
}
ClockUpdater::ClockUpdater() {
  ros::NodeHandle nh; nh.param<bool>("/use_sss_sim_time", use_sim_time, false);
  if (!use_sim_time) return;
  xgc2::xrpc::HttpRequest req; req.method = "POST"; req.target = "/v1/sss-clock/client/register";
  req.body = "{}"; req.request_id = next_request_id();
  auto result = control_client().call(req, xgc2::xrpc::Clock::now() + std::chrono::seconds(2));
  if (result.status != 200) throw std::runtime_error("SSS client registration rejected");
  time_client_id_ = j::parse(result.body)["client_id"].asUInt();
  UpdateClockPublisher::global(); inited_ = true;
}
ClockUpdater::~ClockUpdater() { try { unregister(); } catch (...) { /* bounded best effort on destruction */ } }
bool ClockUpdater::request_clock_update(const ros::Time& new_time) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!use_sim_time) return true;
  if (!inited_) return false;
  if (has_request_ && new_time == request_time_) return true;
  // No network/JSON/retry/wait here: only existing bounded ROS data publication.
  if (!UpdateClockPublisher::global().publish(time_client_id_, new_time)) return false;
  request_time_ = new_time; has_request_ = true; return true;
}
bool ClockUpdater::unregister() {
  uint32_t id;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!use_sim_time || !inited_) return true;
    id = time_client_id_; inited_ = false;
  }
  xgc2::xrpc::HttpRequest req; req.method = "POST"; req.target = "/v1/sss-clock/client/unregister";
  req.request_id = next_request_id(); req.body = j::serialize(j::Object{{"client_id", id}});
  auto result = control_client().call(req, xgc2::xrpc::Clock::now() + std::chrono::milliseconds(200));
  if (result.status == 200 || result.status == 404) { return true; }
  return false;
}
}
