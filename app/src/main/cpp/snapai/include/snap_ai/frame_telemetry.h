#ifndef SNAP_AI_FRAME_TELEMETRY_H
#define SNAP_AI_FRAME_TELEMETRY_H
#include "snap_ai/snap_ai.h"
#include <stdint.h>
#ifdef __cplusplus
#include <memory>
namespace snap_ai::telemetry {
enum class Stream : uint8_t { Source = 0, Generated = 1 };
enum class PresentApi : uint8_t { Present = 0, Present1 = 1, Vulkan = 2 };
struct PresentEvent {
  uint64_t logical_present_id=0, frame_id=0, monotonic_time_ns=0, cpu_duration_ns=0, swapchain_generation=0;
  Stream stream=Stream::Source; PresentApi api=PresentApi::Present;
};
struct StreamStats {
  uint64_t presents=0;
  double interval_median_ms=0, interval_p95_ms=0, interval_p99_ms=0;
  double present_cpu_median_ms=0, present_cpu_p95_ms=0;
};
struct Snapshot {
  uint64_t accepted_events=0, duplicate_events=0, stale_generation_events=0, invalid_events=0, current_generation=0;
  StreamStats source{}, generated{};
  double output_interval_median_ms=0, output_interval_p95_ms=0, output_interval_p99_ms=0;
};
class SNAP_AI_API Recorder {
public:
  explicit Recorder(uint32_t capacity=256); ~Recorder();
  Recorder(Recorder&&) noexcept; Recorder& operator=(Recorder&&) noexcept;
  Recorder(const Recorder&)=delete; Recorder& operator=(const Recorder&)=delete;
  bool record(const PresentEvent& event); void reset(uint64_t generation); Snapshot snapshot() const;
private: struct Impl; std::unique_ptr<Impl> impl_;
};
}
#endif
#endif
