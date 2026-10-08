/**
 * @file ClockUpdater.hpp
 * @author Peixuan Shu (shupeixuan@qq.com)
 * @brief Update sim clock. Utilized by other sim/real agent nodes.
 * 
 * Note: This program relies on 
 * 
 * @version 1.0
 * @date 2023-12-7
 * 
 * @license BSD 3-Clause License
 * @copyright (c) 2023, Peixuan Shu
 * All rights reserved.
 * 
 */

#ifndef __CLOCK_UPDATER__
#define __CLOCK_UPDATER__
#include <ros/ros.h>
#include <ros/callback_queue.h>
#include <std_msgs/Bool.h>
#include <thread>
#include <map>
#include "sss_sim_env/TimeRequest.h"
#include <stdexcept>
namespace xgc2 { namespace xrpc { class HttpClient; } }
#include <atomic>
#include <mutex>

// #ifndef MAX_ROS_TIME
// #define MAX_ROS_TIME UINT32_MAX, 999999999UL  // {sec, nsec}
// #endif

namespace sss_utils
{

/* Static (global) update clock pulisher used by all threads */
class UpdateClockPublisher
{
    public:
        UpdateClockPublisher()
        {
            update_clock_pub_ = nh_.advertise<sss_sim_env::TimeRequest>("/sss_time_client/update_clock_request", 1024, false);
        }

        /* static(global) publisher */
        static UpdateClockPublisher& global()
        {
            static UpdateClockPublisher global;
            return global;
        }

        /* publish time request for time client i */
        bool publish(const int& time_client_id, const ros::Time &new_time)
        {
            sss_sim_env::TimeRequest::Ptr msg(new sss_sim_env::TimeRequest);
            msg->time_client_id = time_client_id;
            msg->request_time = new_time;
            update_clock_pub_.publish(msg);

            return update_clock_pub_.getNumSubscribers() > 0;
        }
    private:
        ros::NodeHandle nh_;
        ros::Publisher update_clock_pub_;
};


class ClockUpdater /* : public std::enable_shared_from_this<ClockUpdater> */
{
    public:
        ClockUpdater();
        ~ClockUpdater();

        /* Publish new time request */
        bool request_clock_update(const ros::Time &new_time);

        /* unregister from time server. Stop time request in the future */
        bool unregister();

        /* get the latest request clock time */
        ros::Time get_request_time() { std::lock_guard<std::mutex> lock(state_mutex_); return request_time_; }
    
    private:
        bool use_sim_time;

        std::mutex state_mutex_;
        bool inited_ = false, has_request_ = false;
        uint32_t time_client_id_ = 0;
        ros::Time request_time_;
        // Process-wide shared C++ SDK pool. No listener/spinner per updater.
        static xgc2::xrpc::HttpClient& control_client();
        static std::string next_request_id();
};


typedef std::shared_ptr<ClockUpdater> ClockUpdaterPtr;

/* global variables */
// static std::map<std::thread::id, ClockUpdaterPtr>  thread_clockupdater_map;  

/* static(global) thread->clockupdaters map */
class ThreadClockupdaters {
public:
    static ThreadClockupdaters& global() { static ThreadClockupdaters global; return global; }
    ClockUpdaterPtr get_clockupdater(const std::thread::id& thread_id) {
        if (thread_id != std::this_thread::get_id())
            throw std::invalid_argument("SSS clock updater must belong to the calling native thread");
        // Thread exit releases its client; stale thread IDs do not accumulate
        // in a process-global map. The provider caps the total at 255 clients.
        thread_local ClockUpdaterPtr updater = std::make_shared<ClockUpdater>();
        return updater;
    }
};

} //sss_utils

#endif