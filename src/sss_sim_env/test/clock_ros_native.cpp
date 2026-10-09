#include <gtest/gtest.h>
#include <ros/ros.h>
#include <rosgraph_msgs/Clock.h>
#include <xgc2/xrpc/http.hpp>
#include "sss_sim_env/ClockJson.hpp"
#include "sss_sim_env/TimeRequest.h"
#include "sss_sim_env/ClockUpdater.hpp"
#include "sss_sim_env/Timer.hpp"
#include <fstream>
#include <filesystem>
#include <functional>
#include <sstream>
namespace j = sss_utils::clock_json;
using namespace xgc2::xrpc;
uint64_t observed_time = UINT64_MAX;
void clock_received(const rosgraph_msgs::Clock::ConstPtr& msg) { observed_time = msg->clock.toNSec(); }
bool wait(const std::function<bool()>& predicate, double seconds = 2) {
  const auto until = ros::WallTime::now() + ros::WallDuration(seconds);
  while (ros::ok() && ros::WallTime::now() < until) {
    ros::spinOnce(); if (predicate()) return true; ros::WallDuration(0.005).sleep();
  }
  return false;
}
int threads() {
  std::ifstream stat("/proc/self/status"); std::string line;
  while (std::getline(stat, line)) if (line.starts_with("Threads:")) return std::stoi(line.substr(8));
  return -1;
}
TEST(SssNativeClock, ControlResetAndUserTimeData) {
  ros::NodeHandle nh, pnh("~"); std::string path; ASSERT_TRUE(pnh.getParam("control_socket", path));
  ASSERT_TRUE(wait([&] { return std::filesystem::exists(path); }));
  HttpClient discovery(path); HttpRequest descriptor; descriptor.method = "GET"; descriptor.target = "/v1/sss-clock/describe";
  auto desc = discovery.call(descriptor, Clock::now() + std::chrono::seconds(2)); ASSERT_EQ(200, desc.status);
  auto instance = std::string(j::parse(desc.body)["instance_id"].asString());
  HttpClient client(path, {}, instance); uint64_t seq = 0;
  auto call = [&](std::string method, j::Object body) {
    HttpRequest req; req.method = "POST"; req.target = "/v1/sss-clock/" + method;
    req.body = j::serialize(body); req.request_id = "ros-test:" + std::to_string(++seq);
    auto response = client.call(req, Clock::now() + std::chrono::seconds(2));
    EXPECT_EQ(200, response.status); return j::parse(response.body);
  };
  EXPECT_FALSE(ros::service::exists("/sss_clock_control", false));
  EXPECT_FALSE(ros::service::exists("/sss_timeclient_register", false));
  EXPECT_FALSE(ros::service::exists("/sss_timeclient_unregister", false));
  auto clock_sub = nh.subscribe("/clock", 4, clock_received);
  auto data = nh.advertise<sss_sim_env::TimeRequest>("/sss_time_client/update_clock_request", 4, false);
  ASSERT_TRUE(wait([&] { return data.getNumSubscribers() > 0 && observed_time == 0; }));
  auto registered = call("client/register", {}); auto id = registered["client_id"].asUInt();
  auto state = call("status", {}); auto desired = state["desired_revision"].asLargestUInt();
  state = call("control", {{"expected_revision", desired}, {"proceed", true}, {"max_speed_ratio", 1}});
  sss_sim_env::TimeRequest request; request.time_client_id = id; request.request_time.fromNSec(20000000);
  data.publish(request); ASSERT_TRUE(wait([&] { return observed_time == 20000000; }));
  desired = state["desired_revision"].asLargestUInt();
  state = call("control", {{"expected_revision", desired}, {"proceed", false}, {"max_speed_ratio", 1}});
  request.request_time.fromNSec(40000000); data.publish(request); ros::WallDuration(0.05).sleep(); ros::spinOnce();
  EXPECT_EQ(20000000, observed_time);
  desired = state["desired_revision"].asLargestUInt();
  state = call("control", {{"expected_revision", desired}, {"proceed", true}, {"max_speed_ratio", 1}});
  ASSERT_TRUE(wait([&] { return observed_time == 40000000; }));
  desired = state["desired_revision"].asLargestUInt();
  state = call("reset", {{"expected_revision", desired}, {"time_ns", 0}});
  ASSERT_TRUE(wait([&] { return observed_time == 0; }));
  EXPECT_TRUE(state["paused"].asBool()); EXPECT_EQ(0, state["clients"].asInt());
  data.publish(request); ros::WallDuration(0.05).sleep(); ros::spinOnce(); EXPECT_EQ(0, observed_time);
  auto second = call("client/register", {}); EXPECT_NE(id, second["client_id"].asUInt());
  call("client/unregister", {{"client_id", second["client_id"]}});
  // Real library consumer registers through the same native XRPC boundary.
  { sss_utils::ClockUpdater updater; ASSERT_TRUE(wait([&] { return updater.request_clock_update(ros::Time(0.05)); }));
    EXPECT_TRUE(updater.unregister()); }
  EXPECT_EQ(0, call("status", {})["clients"].asInt());
  // Adding timers does not add a communication spinner/listener per timer.
  sss_utils::Timer first(nh, ros::Duration(0.1), [](const ros::TimerEvent&) {}, false, false);
  ros::WallDuration(0.05).sleep(); const auto baseline = threads(); ASSERT_GT(baseline, 0);
  std::vector<sss_utils::Timer> timers;
  for (int i = 0; i < 32; ++i) timers.emplace_back(nh, ros::Duration(0.1), [](const ros::TimerEvent&) {}, false, false);
  EXPECT_LE(threads(), baseline + 1);
}
int main(int argc, char** argv) { ros::init(argc, argv, "sss_clock_ros_native_test"); testing::InitGoogleTest(&argc, argv); return RUN_ALL_TESTS(); }
