// Exercise the production Timer.hpp, not a copied selection implementation.
#include "boundary.hpp"
#include "sss_sim_env/Timer.hpp"
#include <cerrno>
#include <cstdlib>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <time.h>

namespace {
void require(bool ok,const std::string& why) { if(!ok) throw std::runtime_error(why); }
uint64_t clock_ns(clockid_t id) {
    timespec t{}; if(clock_gettime(id,&t)!=0) throw std::runtime_error("clock_gettime failed");
    return uint64_t(t.tv_sec)*1000000000ULL+t.tv_nsec;
}
// Specification oracle for fixed membership only. No sorting and no ROS behavior.
struct Oracle {
    std::map<int,std::pair<bool,ros::Time>> timers;
    ros::Time last{}, last_request=ros::TIME_MAX;
    uint64_t calls=0,digest=1469598103934665603ULL,wakes=0;
    bool success=true;
    bool send(ros::Time t) { last=t; ++calls; digest=(digest^ros::ns(t))*1099511628211ULL; return success; }
    bool update(int h,ros::Time t) {
        auto it=timers.find(h); if(it==timers.end()) return true;
        it->second={true,t};
        bool ready=true; ros::Time minimum=ros::TIME_MAX;
        for(const auto& x:timers) { ready &= x.second.first; if(x.second.second<minimum) minimum=x.second.second; }
        if(ready) { last_request=minimum; return send(minimum); }
        if(last==ros::TIME_MAX) { last_request=ros::Time::now(); return send(last_request); }
        return true;
    }
    void clock(ros::Time t) { for(auto& x:timers) if(x.second.second<=t) { x.second.first=false; ++wakes; } }
    bool repeat() { if(last_request==ros::TIME_MAX) last_request=ros::Time::now(); return send(last_request); }
};
ros::Time value(std::mt19937_64& rng) {
    const uint64_t x=rng();
    if(x%7==0) return ros::Time(0,0);
    if(x%7==1) return ros::TIME_MAX;
    return ros::Time(uint32_t(x%31),uint32_t(rng()%1000000000));
}
void semantics(unsigned n,uint64_t seed) {
    sss_utils::TimerManagerExtra manager;
    Oracle oracle;
    std::vector<int> handles;
    for(unsigned i=0;i<n;++i) { int h=manager.add_timer(); handles.push_back(h); oracle.timers[h]={false,ros::TIME_MAX}; }
    std::mt19937_64 rng(seed);
    const uint64_t wake_start=ros::wake_count();
    auto compare=[&](bool a,bool b,unsigned step) {
        require(a==b,"return mismatch step="+std::to_string(step));
        require(manager.clock_updater_->calls==oracle.calls,"call count mismatch step="+std::to_string(step));
        require(manager.clock_updater_->last==oracle.last,"request time mismatch step="+std::to_string(step));
        require(manager.clock_updater_->digest==oracle.digest,"request trace mismatch step="+std::to_string(step));
        require(ros::wake_count()-wake_start==oracle.wakes,"clock invalidation mismatch step="+std::to_string(step));
    };
    for(unsigned k=0;k<20000;++k) {
        const auto op=rng()%20;
        bool a=true,b=true;
        const auto t=value(rng);
        oracle.success=manager.clock_updater_->success=(rng()%5!=0);
        if(op<15) {
            const int h=handles[rng()%n]; a=manager.add_next_cb_time(h,t); b=oracle.update(h,t);
        } else if(op<19) {
            ros::deliver_clock(t); oracle.clock(t);
        } else { a=manager.request_last_time(); b=oracle.repeat(); }
        compare(a,b,k);
    }
    // Unknown handles are acknowledged without changing the request trace.
    compare(manager.add_next_cb_time(-1,ros::TIME_MAX),oracle.update(-1,ros::TIME_MAX),20000);
    std::cout<<"SEMANTICS_OK n="<<n<<" seed="<<seed<<" steps=20001 digest="<<oracle.digest<<"\n";
}
void edges() {
    sss_utils::TimerManagerExtra manager;
    Oracle oracle;
    std::vector<int> handles;
    const uint64_t wake_start=ros::wake_count();
    auto compare=[&](bool a,bool b) {
        require(a==b && manager.clock_updater_->calls==oracle.calls &&
                manager.clock_updater_->digest==oracle.digest &&
                manager.clock_updater_->last==oracle.last &&
                ros::wake_count()-wake_start==oracle.wakes,"deterministic edge mismatch");
    };
    // Empty manager and unknown handle must not dereference an empty minimum.
    compare(manager.add_next_cb_time(-1,ros::TIME_MAX),oracle.update(-1,ros::TIME_MAX));
    for(int i=0;i<3;++i) { int h=manager.add_timer(); handles.push_back(h); oracle.timers[h]={false,ros::TIME_MAX}; }
    auto update=[&](int i,ros::Time t) { compare(manager.add_next_cb_time(handles[i],t),oracle.update(handles[i],t)); };
    for(int i=0;i<3;++i) update(i,ros::TIME_MAX); // all infinity
    compare(manager.request_last_time(),oracle.repeat()); // infinity -> now
    for(int i=0;i<3;++i) update(i,ros::Time(0,0)); // ties and zero
    ros::deliver_clock(ros::Time(0,0)); oracle.clock(ros::Time(0,0)); compare(true,true);
    oracle.success=manager.clock_updater_->success=false;
    for(int i=0;i<3;++i) update(i,ros::Time(7,11)); // partial readiness, then failure
    oracle.success=manager.clock_updater_->success=true;
    update(0,ros::Time(7,11)); // equal-time retry
    ros::deliver_clock(ros::Time(7,11)); oracle.clock(ros::Time(7,11)); compare(true,true);
    ros::deliver_clock(ros::Time(7,11)); oracle.clock(ros::Time(7,11)); compare(true,true);
    std::cout<<"EDGES_OK empty,unknown,partial,infinity,zero,ties,failure,retry,repeat,clock_equal\n";
}
void removal() {
    sss_utils::TimerManagerExtra manager;
    const int a=manager.add_timer(), b=manager.add_timer(), c=manager.add_timer();
    manager.add_next_cb_time(a,ros::Time(1,0));
    manager.add_next_cb_time(b,ros::Time(2,0));
    manager.add_next_cb_time(c,ros::Time(3,0));
    require(manager.remove_timer_info(a),"remove a returned false");
    manager.add_next_cb_time(b,ros::Time(7,0));
    const auto actual=manager.clock_updater_->get_request_time();
    std::cout<<"REMOVE_OBSERVATION expected_min_ns=3000000000 actual_min_ns="<<ros::ns(actual)<<"\n";
    require(actual==ros::Time(3,0),"removal erased the wrong timer after selection reordered its iterator");
    require(manager.remove_timer_info(b),"surviving b missing");
    require(manager.remove_timer_info(c),"surviving c missing");
    require(!manager.remove_timer_info(a),"removed a unexpectedly remains");
    std::cout<<"REMOVAL_OK\n";
}
void benchmark(unsigned n,uint64_t seed) {
    sss_utils::TimerManagerExtra manager;
    std::vector<int> h;
    for(unsigned i=0;i<n;++i) h.push_back(manager.add_timer());
    for(unsigned i=0;i<n;++i) manager.add_next_cb_time(h[i],ros::Time(i+1,0));
    std::mt19937_64 rng(seed);
    std::vector<std::pair<int,ros::Time>> inputs;
    for(unsigned i=0;i<4096;++i) inputs.emplace_back(h[rng()%n],value(rng));
    // Warmup is fixed and not included in the latency samples.
    for(unsigned i=0;i<2000;++i) manager.add_next_cb_time(inputs[i].first,inputs[i].second);
    for(unsigned k=0;k<101;++k) {
        const auto& p=inputs[k+2000];
        const uint64_t w0=clock_ns(CLOCK_MONOTONIC), c0=clock_ns(CLOCK_THREAD_CPUTIME_ID);
        const bool ret=manager.add_next_cb_time(p.first,p.second);
        const uint64_t c1=clock_ns(CLOCK_THREAD_CPUTIME_ID), w1=clock_ns(CLOCK_MONOTONIC);
        require(ret,"unexpected boundary failure in benchmark");
        std::cout<<"SAMPLE,"<<n<<","<<seed<<","<<k<<","<<w1-w0<<","<<c1-c0<<"\n";
    }
    // Same clocks, no operation. Report observer cost; do not silently subtract it.
    for(unsigned k=0;k<101;++k) {
        const uint64_t w0=clock_ns(CLOCK_MONOTONIC), c0=clock_ns(CLOCK_THREAD_CPUTIME_ID);
        asm volatile("" ::: "memory");
        const uint64_t c1=clock_ns(CLOCK_THREAD_CPUTIME_ID), w1=clock_ns(CLOCK_MONOTONIC);
        std::cout<<"OBSERVER,"<<n<<","<<seed<<","<<k<<","<<w1-w0<<","<<c1-c0<<"\n";
    }
    rusage usage{}; require(getrusage(RUSAGE_SELF,&usage)==0,"getrusage failed");
    std::cout<<"BENCH_META n="<<n<<" seed="<<seed<<" digest="<<manager.clock_updater_->digest
             <<" max_rss_kib="<<usage.ru_maxrss<<"\n";
}
}
int main(int argc,char** argv) {
    try {
        require(argc==4,"usage: probe semantics|edges|removal|bench N SEED");
        const std::string mode=argv[1]; const unsigned n=std::stoul(argv[2]); const uint64_t seed=std::stoull(argv[3]);
        require(n>=1 && n<=100,"N outside bounded test range 1..100");
        if(mode=="semantics") semantics(n,seed);
        else if(mode=="edges") edges();
        else if(mode=="removal") removal();
        else if(mode=="bench") benchmark(n,seed);
        else throw std::runtime_error("unknown mode");
        return 0;
    } catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<"\n"; return 2; }
}
