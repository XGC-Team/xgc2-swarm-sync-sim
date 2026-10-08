#pragma once
#include <ros/ros.h>
#include <ros/callback_queue.h>
#include <rosgraph_msgs/Clock.h>
#include <std_msgs/Bool.h>
#include "sss_sim_env/TimeRequest.h"
#include "sss_sim_env/ClockControl.hpp"
#include <memory>

class TimeServer {
public:
  TimeServer(const ros::NodeHandle& nh, const ros::NodeHandle& private_nh);
  ~TimeServer();
  bool try_update_clock();
  ros::Time sim_time_;
private:
  void cb_speed_regulator_timer(const ros::WallTimerEvent&);
  void cb_control_timer(const ros::WallTimerEvent&);
  void cb_update_clock_request(const sss_sim_env::TimeRequest::ConstPtr&);
  void publish_clock();
  ros::NodeHandle nh_, nh_private_, nh_async_;
  ros::CallbackQueue async_callback_queue_;
  ros::AsyncSpinner async_spinner_{1, &async_callback_queue_};
  std::unique_ptr<sss_utils::ClockDomain> domain_;
  std::unique_ptr<sss_utils::ClockControl> control_;
  ros::Publisher sim_clock_pub_, online_pub_;
  ros::Subscriber update_clock_request_sub_;
  ros::WallTimer speed_regulator_timer_, control_timer_;
  uint64_t published_time_ = UINT64_MAX, published_revision_ = 0;
};
