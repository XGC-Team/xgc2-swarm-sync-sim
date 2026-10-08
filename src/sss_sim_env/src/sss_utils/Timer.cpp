/**
 * @file Timer.cpp
 * @author Peixuan Shu (shupeixuan@qq.com)
 * @brief Replace ROS timer. Utilized by other sim/real agent nodes.
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


#include "sss_sim_env/Timer.hpp"


namespace sss_utils
{

Timer::Timer(const ros::NodeHandle &nh, const ros::Duration &period, const ros::TimerCallback& callback, bool oneshot, bool autostart)
{
    impl_ = std::make_shared<Impl>(nh, period, callback, oneshot, autostart);

    // int handle = TimerManagerExtra::global().add_timer(impl_);
    // impl_->set_handle(handle);
}

Timer::Impl::Impl(const ros::NodeHandle &nh, const ros::Duration &period, const ros::TimerCallback& callback, bool oneshot, bool autostart)
    :nh_(nh), period_(period), callback_(callback), oneshot_(oneshot), autostart_(autostart), started_(false), inited_(false)
{
    timer_handle_ = TimerManagerExtra::global().add_timer();


    nh_.param<bool>("/use_sss_sim_time", use_sim_time_, false);

    /* If use_sss_sim_time, create a ROS timer with modified callback function*/
    if (use_sim_time_)
    {
        ROS_INFO("[sss_utils::Timer] use_sss_sim_time is true. Init");

        /* If use_sss_sim_time, create a ROS timer with modified callback function 
        (add request_clock_update() each loop*/
        timer_ = nh_.createTimer(period_, &Timer::Impl::sim_timer_callback, this, oneshot_, autostart_); 

        // Lifecycle initialization has a finite data-plane handshake budget.
        const auto until = ros::WallTime::now() + ros::WallDuration(2);
        while (!TimerManagerExtra::global().add_next_cb_time(timer_handle_, ros::Time::now() + period_)) {
            if (ros::WallTime::now() >= until || !ros::ok())
                throw std::runtime_error("SSS clock data-plane initialization deadline exceeded");
            ros::WallDuration(0.01).sleep();
        }
        inited_ = true;

        // /* Init callback_queue -> spinner_threads map*/
        // callback_queue_ = nh_.getCallbackQueue();
        // if (cbqueue_spinnerthreads_map.find(callback_queue_) == cbqueue_spinnerthreads_map.end())
        // {
        //     cbqueue_spinnerthreads_map[callback_queue_] = std::make_shared<ThreadIdVector>();
        // }

        if (autostart_)
        {
            start();
        }
    }
    else
    {        
        /*If not use_sss_sim_time, create a normal ROS timer */
        timer_ = nh_.createTimer(period_, callback_, oneshot_, autostart_); 
    }

    //@TODO catch SIGINT error caused by ctrl+C, and destroy the parent before ros shutdown (for clock_updater_->unregister())

}

Timer::Impl::~Impl()
{
    stop();
    TimerManagerExtra::global().remove_timer_info(timer_handle_);
}

/* callback_() + clock update in each loop */
void Timer::Impl::sim_timer_callback(const ros::TimerEvent &event)
{
    std::thread::id thread_id = std::this_thread::get_id();

    ClockUpdaterPtr clock_updater = ThreadClockupdaters::global().get_clockupdater(thread_id);

    /* make clock waiting at now util the loop completes */
    if (!clock_updater->request_clock_update(event.current_expected)) {
        ROS_ERROR("SSS clock data plane unavailable"); return;
    }

    /* Set next callback expected time as infinity as we are not sure the next expected time now */
    // @TODO In multi spinner thread mode, check if other timers in timer_manager are blocked by this thread especially when this callback contains a sleep. If blocked, add its next time as infinity too.
    /** @bug:
        [Condition] Multi thread spinner mode & multiple timers & There is a sleep in this callback & max_speed_ratio>1.
        [Description] It may cause the sim_clock directly jumps to the sleep end in the speed of speed regulator directly regardless of other timers in other threads,
            which may lead to some lose of other timer callbacks (frequency decreases)
        [Possible Solutions] Manage all timer callback thread IDs. Check if other timers in timer_manager are blocked by this thread. If blocked, add its next_cb_time as infinity too.
     */
    /* This bug does not cause much bad effect if in single thread mode or the speed regulator steps with the minimum timer period (time-wasting) */
    if (!TimerManagerExtra::global().add_next_cb_time(timer_handle_, ros::TIME_MAX)) {
        ROS_ERROR("SSS timer data plane unavailable"); return;
    }

    /* call the main callback function */
    callback_(event);


    /* Request next triggered time */
    ros::Time next_time;
    if (oneshot_)
    {
        // request a large enough time
        next_time = ros::TIME_MAX - ros::Duration(2); // Do not set as ros::TIME_MAX as it may unblock the timer manager clock updater
    }
    else if(event.current_expected + period_ <= ros::Time::now())
    {
        next_time = ros::Time::now();

        ROS_WARN("[sss_utils::Timer::Impl::sim_timer_callback] Detect timer loop jumps. Set next expected time as now %ss with last time = %ss and period = %ss. This is caused by the callback execution time longer than the timer period. Check if a too long sleep is applied in a timer/subscriber callback or if enough multi-thread spinners are set.", std::to_string(next_time.toSec()).c_str(), std::to_string(event.current_expected.toSec()).c_str(), std::to_string(period_.toSec()).c_str());
    }
    else
    {
        next_time = event.current_expected + period_;
    }
    
    /* Set next callback expected time */
    TimerManagerExtra::global().add_next_cb_time(timer_handle_, next_time);

    /* Request infinite next time in this thread as we do not know whether there will be callbacks in this spinner thread in the future */
    clock_updater->request_clock_update(ros::TIME_MAX);

    /* print the real loop rate */
    // double time_now = ros::WallTime::now().toSec();
    // double rate = 1.0 / (time_now - last_sim_timer_cb_time_);
    // last_sim_timer_cb_time_ = time_now;
    // ROS_INFO("[sss_utils::Timer] timer_callback rate: %s Hz", std::to_string(rate).c_str()); 
}

void Timer::Impl::start()
{
    if (!started_)
    {
        timer_.start();
    }
    started_ = true;
}

void Timer::Impl::stop()
{
    if (started_)
    {
        timer_.stop();
    }
    started_ = false;
}

}