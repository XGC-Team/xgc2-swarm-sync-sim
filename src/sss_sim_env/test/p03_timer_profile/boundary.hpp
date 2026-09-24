// Test-only single-thread ROS boundary spy. Never installed into the ROS include path.
// It does not model transport, scheduling, service registration or actual ClockUpdater.
#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#define ROS_INFO(...) ((void)0)
namespace ros {
struct Time {
    uint32_t sec = 0, nsec = 0;
    Time() = default;
    explicit Time(double x) : sec(static_cast<uint32_t>(x)),
        nsec(static_cast<uint32_t>((x-sec)*1e9)) {}
    Time(uint32_t s, uint32_t n) : sec(s), nsec(n) {}
    static Time now();
    double toSec() const { return sec + nsec * 1e-9; }
};
inline bool operator<(Time a, Time b) { return std::make_pair(a.sec,a.nsec) < std::make_pair(b.sec,b.nsec); }
inline bool operator==(Time a, Time b) { return a.sec==b.sec && a.nsec==b.nsec; }
inline bool operator!=(Time a, Time b) { return !(a==b); }
inline bool operator<=(Time a, Time b) { return a<b || a==b; }
inline uint64_t ns(Time t) { return uint64_t(t.sec)*1000000000ULL+t.nsec; }
inline Time& test_now() { static Time t; return t; }
inline Time Time::now() { return test_now(); }
const Time TIME_MAX{UINT32_MAX,999999999};
struct Duration { explicit Duration(double=0) {} };
const Duration DURATION_MAX{};
struct TimerEvent {};
using TimerCallback = std::function<void(const TimerEvent&)>;
struct CallbackQueueInterface {};
struct CallbackQueue : CallbackQueueInterface {};
struct AsyncSpinner { AsyncSpinner(int,CallbackQueue*) {} void start() {} };
inline uint64_t& wake_count() { static uint64_t n=0; return n; }
struct Timer {
    void start() {} void stop() {}
    bool hasStarted() const { return true; }
    bool isValid() const { return true; }
    bool hasPending() const { return false; }
    void setPeriod(const Duration&, bool=true) { ++wake_count(); }
};
struct Subscriber {};
}
namespace std_msgs { struct Bool { using ConstPtr=std::shared_ptr<const Bool>; bool data=false; }; }
namespace rosgraph_msgs { struct Clock { using ConstPtr=std::shared_ptr<const Clock>; ros::Time clock; }; }
namespace ros {
inline std::function<void(Time)>& clock_callback() { static std::function<void(Time)> cb; return cb; }
struct NodeHandle {
    template<class T> void param(const char*,T& out,const T&) { out=true; }
    void setCallbackQueue(CallbackQueue*) {}
    template<class T> Subscriber subscribe(const char*,int,
        void (T::*cb)(const rosgraph_msgs::Clock::ConstPtr&),T* obj) {
        clock_callback()=[obj,cb](Time t) {
            auto msg=std::make_shared<rosgraph_msgs::Clock>(); msg->clock=t; (obj->*cb)(msg);
        };
        return {};
    }
    template<class T> Timer createTimer(const Duration&,void (T::*)(const TimerEvent&),T*) { return {}; }
};
inline void deliver_clock(Time t) { test_now()=t; clock_callback()(t); }
}
namespace sss_utils {
class ClockUpdater {
public:
    ros::Time last{};
    uint64_t calls=0, digest=1469598103934665603ULL;
    bool success=true;
    // Observe each boundary call. Deliberately do not emulate publisher dedup/retry.
    bool request_clock_update(const ros::Time& t) {
        last=t; ++calls; digest=(digest^ros::ns(t))*1099511628211ULL; return success;
    }
    ros::Time get_request_time() const { return last; }
};
using ClockUpdaterPtr=std::shared_ptr<ClockUpdater>;
}
