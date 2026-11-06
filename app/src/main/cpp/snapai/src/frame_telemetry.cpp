#include "snap_ai/frame_telemetry.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <vector>
namespace snap_ai::telemetry {
namespace {
constexpr size_t kRecentIds=256;
double percentile(std::vector<double> v,double p){if(v.empty())return 0;std::sort(v.begin(),v.end());size_t i=static_cast<size_t>(std::ceil(p*v.size()))-1;return v[std::min(i,v.size()-1)];}
struct Sample{uint64_t time_ns=0,cpu_ns=0;Stream stream=Stream::Source;};
}
struct Recorder::Impl{
 explicit Impl(uint32_t n):capacity(std::max<uint32_t>(n,8)),samples(capacity){}
 mutable std::mutex mutex;size_t capacity;std::vector<Sample> samples;size_t count=0,next=0;
 std::array<uint64_t,kRecentIds> recent{};size_t recent_next=0,recent_count=0;uint64_t generation=0;Snapshot counters{};
 bool duplicate(uint64_t id)const{for(size_t i=0;i<recent_count;++i)if(recent[i]==id)return true;return false;}
 void push(const PresentEvent&e){samples[next]={e.monotonic_time_ns,e.cpu_duration_ns,e.stream};next=(next+1)%capacity;count=std::min(count+1,capacity);recent[recent_next]=e.logical_present_id;recent_next=(recent_next+1)%kRecentIds;recent_count=std::min(recent_count+1,kRecentIds);}
};
Recorder::Recorder(uint32_t n):impl_(new Impl(n)){} Recorder::~Recorder()=default;Recorder::Recorder(Recorder&&)noexcept=default;Recorder& Recorder::operator=(Recorder&&)noexcept=default;
bool Recorder::record(const PresentEvent&e){
 if(!impl_)return false;std::lock_guard<std::mutex>l(impl_->mutex);
 if(!e.logical_present_id||!e.frame_id||!e.monotonic_time_ns||(e.stream!=Stream::Source&&e.stream!=Stream::Generated)||(e.api!=PresentApi::Present&&e.api!=PresentApi::Present1&&e.api!=PresentApi::Vulkan)){++impl_->counters.invalid_events;return false;}
 if(e.swapchain_generation<impl_->generation){++impl_->counters.stale_generation_events;return false;}
 if(e.swapchain_generation>impl_->generation){impl_->generation=e.swapchain_generation;impl_->count=impl_->next=impl_->recent_count=impl_->recent_next=0;}
 if(impl_->duplicate(e.logical_present_id)){++impl_->counters.duplicate_events;return false;}
 impl_->push(e);++impl_->counters.accepted_events;impl_->counters.current_generation=impl_->generation;return true;
}
void Recorder::reset(uint64_t g){if(!impl_)return;std::lock_guard<std::mutex>l(impl_->mutex);impl_->generation=g;impl_->count=impl_->next=impl_->recent_count=impl_->recent_next=0;impl_->counters.current_generation=g;}
Snapshot Recorder::snapshot()const{
 Snapshot o{};if(!impl_)return o;std::lock_guard<std::mutex>l(impl_->mutex);o=impl_->counters;o.current_generation=impl_->generation;
 std::vector<double> all,si,gi,sc,gc;uint64_t la=0,ls=0,lg=0,sn=0,gn=0;size_t start=(impl_->next+impl_->capacity-impl_->count)%impl_->capacity;
 for(size_t i=0;i<impl_->count;++i){const Sample&s=impl_->samples[(start+i)%impl_->capacity];if(la&&s.time_ns>la)all.push_back(double(s.time_ns-la)/1e6);la=s.time_ns;
 if(s.stream==Stream::Source){++sn;if(ls&&s.time_ns>ls)si.push_back(double(s.time_ns-ls)/1e6);ls=s.time_ns;if(s.cpu_ns)sc.push_back(double(s.cpu_ns)/1e6);}
 else{++gn;if(lg&&s.time_ns>lg)gi.push_back(double(s.time_ns-lg)/1e6);lg=s.time_ns;if(s.cpu_ns)gc.push_back(double(s.cpu_ns)/1e6);}}
 auto fill=[](StreamStats&s,uint64_t n,const std::vector<double>&i,const std::vector<double>&c){s.presents=n;s.interval_median_ms=percentile(i,.5);s.interval_p95_ms=percentile(i,.95);s.interval_p99_ms=percentile(i,.99);s.present_cpu_median_ms=percentile(c,.5);s.present_cpu_p95_ms=percentile(c,.95);};
 fill(o.source,sn,si,sc);fill(o.generated,gn,gi,gc);o.output_interval_median_ms=percentile(all,.5);o.output_interval_p95_ms=percentile(all,.95);o.output_interval_p99_ms=percentile(all,.99);return o;
}
}
