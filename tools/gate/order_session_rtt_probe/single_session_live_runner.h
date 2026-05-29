#ifndef AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SINGLE_SESSION_LIVE_RUNNER_H_
#define AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SINGLE_SESSION_LIVE_RUNNER_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <magic_enum/magic_enum.hpp>

#include "core/config/instrument_catalog.h"
#include "core/market_data/types.h"
#include "core/trading/order_feedback_shm.h"
#include "core/websocket/runtime_clock.h"
#include "exchange/gate/trading/order_types.h"
#include "tools/gate/order_session_rtt_probe/config.h"
#include "tools/gate/order_session_rtt_probe/live_run_plan.h"
#include "tools/gate/order_session_rtt_probe/order_mode.h"
#include "tools/gate/order_session_rtt_probe/passive_order_builder.h"
#include "tools/gate/order_session_rtt_probe/sample_csv_writer.h"
#include "tools/gate/order_session_rtt_probe/sample_executor.h"
#include "tools/gate/order_session_rtt_probe/sample_id_allocator.h"

namespace aquila::tools::gate_order_session_rtt_probe {

struct SingleSessionLiveRunnerStats {
  std::uint64_t data_reader_events{0};
  std::uint64_t feedback_events{0};
  std::uint64_t samples_started{0};
  std::uint64_t samples_completed{0};
  std::uint64_t samples_failed{0};
  std::uint64_t skipped_book_tickers{0};
};

template <typename SessionT, typename DataReaderT,
          typename FeedbackReaderT = OrderFeedbackShmReader>
class SingleSessionLiveRunner {
 public:
  SingleSessionLiveRunner(const ProbeConfig& config,
                          const SingleSessionLiveRunPlan& plan,
                          const config::InstrumentCatalog& instrument_catalog,
                          DataReaderT& data_reader,
                          FeedbackReaderT& feedback_reader,
                          SampleCsvWriter& sample_writer,
                          double duration_sec) noexcept
      : config_(config),
        plan_(plan),
        instrument_catalog_(instrument_catalog),
        data_reader_(data_reader),
        feedback_reader_(feedback_reader),
        sample_writer_(sample_writer),
        id_allocator_(static_cast<std::uint8_t>(config.feedback.strategy_id),
                      plan.local_order_id_first, plan.local_order_id_stride),
        duration_ns_(DurationToNs(duration_sec)) {}

  void BindSession(SessionT& session) noexcept {
    session_ = &session;
  }

  void EnableExternalDispatchGate() noexcept {
    external_dispatch_required_ = true;
  }

  void GrantDispatch() noexcept {
    dispatch_grants_.fetch_add(1, std::memory_order_release);
  }

  static void RuntimeHookCallback(void* raw) noexcept {
    static_cast<SingleSessionLiveRunner*>(raw)->DriveHookOnce();
  }

  void OnLoginReady() noexcept {
    login_ready_ = true;
  }

  void OnLoginNotReady() noexcept {
    login_ready_ = false;
  }

  void OnOrderResponse(const gate::OrderResponse& response) noexcept {
    if (!active_executor_) {
      return;
    }
    ProbeSampleTransition transition =
        active_executor_->OnOrderResponse(*session_, response);
    HandleTransition(transition, NowRealtimeNs());
  }

  void OnBookTicker(const BookTicker& ticker) noexcept {
    ++stats_.data_reader_events;
    data_reader_events_.store(stats_.data_reader_events,
                              std::memory_order_release);
    if (ticker.exchange != Exchange::kGate) {
      return;
    }
    latest_gate_ticker_ = ticker;
  }

  void DriveHookOnce() noexcept {
    if (run_start_steady_ns_ == 0) {
      run_start_steady_ns_ = NowSteadyNs();
      next_sample_steady_ns_ = run_start_steady_ns_;
    }

    const std::int64_t now_steady_ns = NowSteadyNs();
    const std::int64_t now_realtime_ns = NowRealtimeNs();
    if (DurationElapsed(now_steady_ns)) {
      Stop(0, "duration_reached");
      return;
    }

    PollFeedback(now_realtime_ns);
    if (stopping()) {
      return;
    }
    CheckStageTimeout(now_realtime_ns);
    if (stopping()) {
      return;
    }
    data_reader_.Drain(*this, config_.sampling.max_events_per_drain);
    if (stopping() || !login_ready_ || active_executor_) {
      return;
    }
    if (stats_.samples_completed >= plan_.sample_count) {
      Stop(0, "sample_count_reached");
      return;
    }
    if (now_steady_ns < next_sample_steady_ns_) {
      return;
    }
    if (!HasDispatchGrant()) {
      return;
    }
    if (StartNextSample(now_realtime_ns, now_steady_ns)) {
      ConsumeDispatchGrant();
    }
  }

  [[nodiscard]] const SingleSessionLiveRunnerStats& stats() const noexcept {
    return stats_;
  }

  [[nodiscard]] std::uint64_t samples_started() const noexcept {
    return samples_started_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::uint64_t data_reader_events() const noexcept {
    return data_reader_events_.load(std::memory_order_acquire);
  }

  [[nodiscard]] int exit_code() const noexcept {
    return exit_code_;
  }

  [[nodiscard]] std::string_view stop_reason() const noexcept {
    return stop_reason_;
  }

  [[nodiscard]] bool stopping() const noexcept {
    return stopping_.load(std::memory_order_acquire);
  }

 private:
  struct TimeoutWatch {
    ProbeStage stage{ProbeStage::kIdle};
    ProbeStageStatus status{ProbeStageStatus::kNotSubmitted};
    std::int64_t deadline_realtime_ns{0};
  };

  [[nodiscard]] static std::int64_t NowRealtimeNs() noexcept {
    return static_cast<std::int64_t>(websocket::RealtimeClockNowNs());
  }

  [[nodiscard]] static std::int64_t NowSteadyNs() noexcept {
    return static_cast<std::int64_t>(websocket::SteadyClockNowNs());
  }

  [[nodiscard]] static std::int64_t DurationToNs(double duration_sec) noexcept {
    if (duration_sec <= 0.0) {
      return 0;
    }
    const auto duration = std::chrono::duration<double>(duration_sec);
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration)
        .count();
  }

  [[nodiscard]] bool DurationElapsed(
      std::int64_t now_steady_ns) const noexcept {
    return duration_ns_ > 0 &&
           now_steady_ns - run_start_steady_ns_ >= duration_ns_;
  }

  [[nodiscard]] const config::InstrumentInfo* FindInstrument(
      const BookTicker& ticker) const noexcept {
    for (const config::InstrumentInfo& instrument :
         instrument_catalog_.instruments()) {
      if (instrument.exchange == Exchange::kGate &&
          instrument.symbol_id == ticker.symbol_id) {
        return &instrument;
      }
    }
    return nullptr;
  }

  [[nodiscard]] PassiveOrderBuildResult BuildIocPassive() noexcept {
    if (!latest_gate_ticker_) {
      ++stats_.skipped_book_tickers;
      return {.error = "missing latest Gate BBO"};
    }
    const config::InstrumentInfo* instrument =
        FindInstrument(*latest_gate_ticker_);
    if (instrument == nullptr) {
      ++stats_.skipped_book_tickers;
      return {.error = "missing instrument for latest Gate BBO"};
    }
    return BuildPassiveBuyOrder(
        *latest_gate_ticker_, *instrument,
        PassiveOrderOptions{.passive_price_limit_fraction =
                                config_.order.passive_price_limit_fraction});
  }

  [[nodiscard]] bool StartNextSample(std::int64_t now_realtime_ns,
                                     std::int64_t now_steady_ns) noexcept {
    PassiveOrderBuildResult ioc_passive = BuildIocPassive();
    if (!ioc_passive.ok) {
      return false;
    }
    PassiveOrderBuildResult gtc_passive;
    ProbeSampleLocalIds ids = id_allocator_.Next();
    auto executor_result = ProbeSampleExecutor::Create(
        std::move(gtc_passive), ioc_passive, ids, ProbeOrderMode::kIoc);
    if (!executor_result.ok) {
      Stop(2, executor_result.error);
      return true;
    }

    active_executor_ = std::move(executor_result.value);
    active_ids_ = ids;
    active_row_ = ProbeSampleCsvRow{
        .run_id = config_.run_id,
        .connect_ip = plan_.connect_ip,
        .order_session_id = plan_.order_session_id,
        .round_index = stats_.samples_started,
        .sample_index = stats_.samples_started,
        .contract = ioc_passive.contract,
        .quantity_text = ioc_passive.quantity_text,
    };
    active_ioc_price_text_ = ioc_passive.price_text;
    active_ioc_bbo_ticker_id_ = ioc_passive.bbo_ticker_id;
    active_ioc_bbo_local_ns_ = ioc_passive.bbo_local_ns;
    ++stats_.samples_started;
    samples_started_.store(stats_.samples_started, std::memory_order_release);
    next_sample_steady_ns_ =
        now_steady_ns +
        static_cast<std::int64_t>(config_.sampling.cycle_cooldown_ms) *
            1'000'000;

    ProbeSampleTransition transition = active_executor_->Start(*session_);
    HandleTransition(transition, now_realtime_ns);
    UpdateTimeoutWatch(now_realtime_ns);
    return true;
  }

  [[nodiscard]] bool HasDispatchGrant() const noexcept {
    if (!external_dispatch_required_) {
      return true;
    }
    return consumed_dispatch_grants_ <
           dispatch_grants_.load(std::memory_order_acquire);
  }

  void ConsumeDispatchGrant() noexcept {
    if (external_dispatch_required_) {
      ++consumed_dispatch_grants_;
    }
  }

  void PollFeedback(std::int64_t now_realtime_ns) noexcept {
    const std::size_t handled = feedback_reader_.Poll(
        config_.feedback.poll_budget,
        [this, now_realtime_ns](const OrderFeedbackEvent& event) {
          ++stats_.feedback_events;
          if (!active_executor_) {
            return;
          }
          ProbeSampleTransition transition =
              active_executor_->OnOrderFeedback(*session_, event);
          HandleTransition(transition, now_realtime_ns);
        });
    (void)handled;
  }

  [[nodiscard]] TimeoutWatch DesiredTimeoutWatch() const noexcept {
    if (!active_executor_) {
      return {};
    }
    const ProbeSampleStats& stats = active_executor_->stats();
    if (stats.ioc_close_status == ProbeStageStatus::kSent ||
        stats.ioc_close_status == ProbeStageStatus::kAcked) {
      return TimeoutWatch{.stage = ProbeStage::kIocClose,
                          .status = stats.ioc_close_status};
    }
    if (stats.ioc_place_status == ProbeStageStatus::kSent ||
        stats.ioc_place_status == ProbeStageStatus::kAcked) {
      return TimeoutWatch{.stage = ProbeStage::kIocPlace,
                          .status = stats.ioc_place_status};
    }
    return {};
  }

  void UpdateTimeoutWatch(std::int64_t now_realtime_ns) noexcept {
    TimeoutWatch desired = DesiredTimeoutWatch();
    if (desired.stage == ProbeStage::kIdle) {
      timeout_watch_ = {};
      return;
    }
    if (timeout_watch_.stage == desired.stage &&
        timeout_watch_.status == desired.status) {
      return;
    }
    const std::uint32_t timeout_ms = desired.status == ProbeStageStatus::kSent
                                         ? config_.sessions.request_timeout_ms
                                         : config_.feedback.terminal_timeout_ms;
    desired.deadline_realtime_ns =
        now_realtime_ns + static_cast<std::int64_t>(timeout_ms) * 1'000'000;
    timeout_watch_ = desired;
  }

  void CheckStageTimeout(std::int64_t now_realtime_ns) noexcept {
    if (!active_executor_) {
      return;
    }
    UpdateTimeoutWatch(now_realtime_ns);
    if (timeout_watch_.stage == ProbeStage::kIdle ||
        now_realtime_ns < timeout_watch_.deadline_realtime_ns) {
      return;
    }
    ProbeSampleTransition transition =
        active_executor_->OnStageTimeout(*session_, timeout_watch_.stage);
    HandleTransition(transition, now_realtime_ns);
    UpdateTimeoutWatch(now_realtime_ns);
  }

  void HandleTransition(const ProbeSampleTransition& transition,
                        std::int64_t now_realtime_ns) noexcept {
    if (!active_executor_) {
      return;
    }
    if (!transition.ok || transition.action == ProbeSampleAction::kFail) {
      FinishActiveSample(transition.error, false);
      Stop(2, transition.error.empty() ? "sample_failed" : transition.error);
      return;
    }
    if (transition.action == ProbeSampleAction::kFinish) {
      FinishActiveSample("", true);
      return;
    }
    UpdateTimeoutWatch(now_realtime_ns);
  }

  static std::string StatusName(ProbeStageStatus status) {
    return std::string(magic_enum::enum_name(status));
  }

  static std::string FeedbackKindName(const ProbeStageCsvStats& stats) {
    if (!stats.has_terminal_feedback_kind) {
      return {};
    }
    return std::string(magic_enum::enum_name(stats.terminal_feedback_kind));
  }

  void WriteOrderActionRow(const ProbeStageCsvStats& stats,
                           ProbeStageStatus status,
                           std::string_view probe_order_type,
                           std::string_view order_action,
                           std::string_view price_text,
                           std::int64_t bbo_ticker_id,
                           std::int64_t bbo_local_ns) noexcept {
    if (status == ProbeStageStatus::kNotSubmitted &&
        stats.local_order_id == 0) {
      return;
    }
    ProbeSampleCsvRow row = active_row_;
    row.price_text.assign(price_text.data(), price_text.size());
    row.probe_order_type.assign(probe_order_type.data(),
                                probe_order_type.size());
    row.order_action.assign(order_action.data(), order_action.size());
    row.local_order_id = stats.local_order_id;
    row.request_sequence = stats.request_sequence;
    row.bbo_ticker_id = bbo_ticker_id;
    row.bbo_local_ns = bbo_local_ns;
    row.request_send_local_ns = stats.request_send_local_ns;
    row.ack_receive_local_ns = stats.ack_receive_local_ns;
    row.ack_exchange_ns = stats.ack_exchange_ns;
    row.ack_exchange_to_local_ns = stats.ack_exchange_to_local_ns;
    row.ack_rtt_ns = stats.ack_rtt_ns;
    row.response_receive_local_ns = stats.response_receive_local_ns;
    row.response_exchange_ns = stats.response_exchange_ns;
    row.response_exchange_to_local_ns = stats.response_exchange_to_local_ns;
    row.response_rtt_ns = stats.response_rtt_ns;
    row.status = StatusName(status);
    row.terminal_feedback_kind = FeedbackKindName(stats);
    sample_writer_.Write(row);
  }

  void WriteOrderActionRows(const ProbeSampleStats& stats) noexcept {
    WriteOrderActionRow(stats.gtc_open_csv, stats.gtc_place_status, "gtc",
                        "open", active_gtc_price_text_,
                        active_gtc_bbo_ticker_id_, active_gtc_bbo_local_ns_);
    WriteOrderActionRow(stats.gtc_cancel_csv, stats.gtc_cancel_status, "gtc",
                        "cancel", active_gtc_price_text_,
                        active_gtc_bbo_ticker_id_, active_gtc_bbo_local_ns_);
    WriteOrderActionRow(stats.ioc_open_csv, stats.ioc_place_status, "ioc",
                        "open", active_ioc_price_text_,
                        active_ioc_bbo_ticker_id_, active_ioc_bbo_local_ns_);
    WriteOrderActionRow(stats.gtc_close_csv, stats.gtc_close_status, "gtc",
                        "close", "0", 0, 0);
    WriteOrderActionRow(stats.ioc_close_csv, stats.ioc_close_status, "ioc",
                        "close", "0", 0, 0);
  }

  void FinishActiveSample(std::string_view error, bool completed) noexcept {
    if (!active_executor_) {
      return;
    }
    const ProbeSampleStats& stats = active_executor_->stats();
    active_row_.unexpected_fill = stats.unexpected_fill;
    active_row_.invalid_for_rtt_distribution =
        stats.invalid_for_rtt_distribution || !completed;
    active_row_.invalid_reason = stats.invalid_reason;
    if (!completed && active_row_.invalid_reason.empty()) {
      active_row_.invalid_reason.assign(error.data(), error.size());
    }
    WriteOrderActionRows(stats);
    active_executor_.reset();
    active_ids_ = {};
    active_row_ = {};
    active_gtc_price_text_.clear();
    active_ioc_price_text_.clear();
    active_gtc_bbo_ticker_id_ = 0;
    active_gtc_bbo_local_ns_ = 0;
    active_ioc_bbo_ticker_id_ = 0;
    active_ioc_bbo_local_ns_ = 0;
    timeout_watch_ = {};
    if (completed) {
      ++stats_.samples_completed;
    } else {
      ++stats_.samples_failed;
    }
  }

  void Stop(int exit_code, std::string_view reason) noexcept {
    bool expected = false;
    if (!stopping_.compare_exchange_strong(expected, true,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
      return;
    }
    exit_code_ = exit_code;
    stop_reason_.assign(reason.data(), reason.size());
    if (session_ != nullptr) {
      session_->Stop();
    }
  }

  const ProbeConfig& config_;
  const SingleSessionLiveRunPlan& plan_;
  const config::InstrumentCatalog& instrument_catalog_;
  DataReaderT& data_reader_;
  FeedbackReaderT& feedback_reader_;
  SampleCsvWriter& sample_writer_;
  ProbeSampleIdAllocator id_allocator_;
  SessionT* session_{nullptr};
  std::optional<BookTicker> latest_gate_ticker_;
  std::unique_ptr<ProbeSampleExecutor> active_executor_;
  ProbeSampleLocalIds active_ids_{};
  ProbeSampleCsvRow active_row_;
  std::string active_gtc_price_text_;
  std::string active_ioc_price_text_;
  std::int64_t active_gtc_bbo_ticker_id_{0};
  std::int64_t active_gtc_bbo_local_ns_{0};
  std::int64_t active_ioc_bbo_ticker_id_{0};
  std::int64_t active_ioc_bbo_local_ns_{0};
  TimeoutWatch timeout_watch_;
  SingleSessionLiveRunnerStats stats_{};
  std::atomic<std::uint64_t> samples_started_{0};
  std::atomic<std::uint64_t> data_reader_events_{0};
  std::atomic<std::uint64_t> dispatch_grants_{0};
  std::uint64_t consumed_dispatch_grants_{0};
  std::int64_t run_start_steady_ns_{0};
  std::int64_t next_sample_steady_ns_{0};
  std::int64_t duration_ns_{0};
  bool login_ready_{false};
  bool external_dispatch_required_{false};
  std::atomic<bool> stopping_{false};
  int exit_code_{1};
  std::string stop_reason_{"not_started"};
};

}  // namespace aquila::tools::gate_order_session_rtt_probe

#endif  // AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SINGLE_SESSION_LIVE_RUNNER_H_
