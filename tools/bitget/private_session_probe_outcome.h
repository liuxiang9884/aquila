#ifndef AQUILA_TOOLS_BITGET_PRIVATE_SESSION_PROBE_OUTCOME_H_
#define AQUILA_TOOLS_BITGET_PRIVATE_SESSION_PROBE_OUTCOME_H_

namespace aquila::bitget {

struct PrivateSessionProbeOutcome {
  bool started_ok{false};
  bool completed_requested_duration{false};
  bool reached_ready{false};
  bool response_stream_clean{true};
};

[[nodiscard]] inline bool PrivateSessionProbeSucceeded(
    const PrivateSessionProbeOutcome& outcome) noexcept {
  return outcome.started_ok && outcome.reached_ready &&
         outcome.response_stream_clean && outcome.completed_requested_duration;
}

}  // namespace aquila::bitget

#endif  // AQUILA_TOOLS_BITGET_PRIVATE_SESSION_PROBE_OUTCOME_H_
