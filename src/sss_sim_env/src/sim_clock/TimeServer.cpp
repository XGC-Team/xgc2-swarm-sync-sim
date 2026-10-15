#include "sss_sim_env/TimeServer.hpp"
#include <stdexcept>

TimeServer::TimeServer(const ros::NodeHandle& nh, const ros::NodeHandle& private_nh)
    : nh_(nh), nh_private_(private_nh), nh_async_(nh) {
  bool use_sim = false, start = false; double speed = 100; std::string socket;
  nh_.param<bool>("/use_sim_time", use_sim, false);
  if (!use_sim) throw std::invalid_argument("SSS clock requires startup /use_sim_time=true");
  nh_private_.param<double>("max_speed_ratio", speed, 100);
  nh_private_.param<bool>("auto_start", start, false);
  if (!nh_private_.getParam("control_socket", socket) || socket.empty())
    throw std::invalid_argument("control_socket must be an explicit private runtime allocation");
  domain_ = std::make_unique<sss_utils::ClockDomain>(speed, start);
  control_ = std::make_unique<sss_utils::ClockControl>(socket, domain_->snapshot());
  nh_async_.setCallbackQueue(&async_callback_queue_);
  sim_clock_pub_ = nh_.advertise<rosgraph_msgs::Clock>("/clock", 10, true);
  online_pub_ = nh_.advertise<std_msgs::Bool>("/sss_clock_is_online", 1, true);
  // Algorithm time requests are the ROS data plane. Control and registration
  // are XRPC; one owner serializes all clock state and native commands.
  update_clock_request_sub_ = nh_async_.subscribe("/sss_time_client/update_clock_request", 1024,
      &TimeServer::cb_update_clock_request, this);
  speed_regulator_timer_ = nh_async_.createWallTimer(ros::WallDuration(0.01 / speed),
      &TimeServer::cb_speed_regulator_timer, this);
  control_timer_ = nh_async_.createWallTimer(ros::WallDuration(0.005),
      &TimeServer::cb_control_timer, this);
  publish_clock();
  std_msgs::Bool online; online.data = true; online_pub_.publish(online);
  async_spinner_.start();
}
TimeServer::~TimeServer() {
  control_timer_.stop(); speed_regulator_timer_.stop(); update_clock_request_sub_.shutdown();
  async_spinner_.stop();
  std_msgs::Bool online; online.data = false; online_pub_.publish(online);
  control_.reset();
}
void TimeServer::cb_control_timer(const ros::WallTimerEvent&) {
  control_->process([this](const sss_utils::ClockCommand& c) {
    using K = sss_utils::ClockCommand::Kind;
    sss_utils::ClockDomain::Result result;
    switch (c.kind) {
      case K::Control:
        result = domain_->control(c.expected_revision, c.proceed, c.speed);
        if (result.error == sss_utils::ClockDomain::Error::None)
          speed_regulator_timer_.setPeriod(ros::WallDuration(0.01 / c.speed), false);
        break;
      case K::Reset: result = domain_->reset(c.expected_revision, c.time_ns); break;
      case K::Register: result = domain_->register_client(); break;
      case K::Unregister: result = domain_->unregister_client(c.client_id); break;
    }
    publish_clock(); return result;
  });
  publish_clock();
}
void TimeServer::cb_speed_regulator_timer(const ros::WallTimerEvent&) {
  domain_->tick(); publish_clock();
}
void TimeServer::cb_update_clock_request(const sss_sim_env::TimeRequest::ConstPtr& msg) {
  if (domain_->request(msg->time_client_id, msg->request_time.toNSec())) publish_clock();
}
bool TimeServer::try_update_clock() {
  // Native owner only; no ROS-graph inspection or transport wait.
  auto prior = domain_->snapshot().time_ns; domain_->tick(); publish_clock();
  return prior != domain_->snapshot().time_ns;
}
void TimeServer::publish_clock() {
  const auto s = domain_->snapshot();
  if (s.time_ns != published_time_) {
    sim_time_.fromNSec(s.time_ns); rosgraph_msgs::Clock msg; msg.clock = sim_time_;
    sim_clock_pub_.publish(msg); published_time_ = s.time_ns;
  }
  if (s.revision != published_revision_) {
    if (control_->publish(s)) published_revision_ = s.revision;
  }
}
