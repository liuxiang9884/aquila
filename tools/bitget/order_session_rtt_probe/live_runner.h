#ifndef AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_LIVE_RUNNER_H_
#define AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_LIVE_RUNNER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <magic_enum/magic_enum.hpp>

#include "core/config/instrument_catalog.h"
#include "core/market_data/types.h"
#include "core/websocket/runtime_clock.h"
#include "exchange/bitget/trading/order_types.h"
#include "tools/bitget/order_session_rtt_probe/config.h"
#include "tools/bitget/order_session_rtt_probe/connection_plan.h"
#include "tools/bitget/order_session_rtt_probe/passive_order_builder.h"
#include "tools/bitget/order_session_rtt_probe/sample_csv_writer.h"
#include "tools/bitget/order_session_rtt_probe/sample_flow.h"
#include "tools/bitget/order_session_rtt_probe/sample_id_allocator.h"

namespace aquila::tools::bitget_order_session_rtt_probe {

struct LiveRunnerStats {
  std::uint64_t data_reader_events{0};
  std::uint64_t feedback_events{0};
  std::uint64_t samples_started{0};
  std::uint64_t samples_completed{0};
  std::uint64_t samples_failed{0};
  std::uint64_t stale_book_tickers{0};
};

using LiveRunnerNowFunction = std::int64_t (*)(void*) noexcept;

template <typename SessionT, typename DataReaderT, typename FeedbackQueueT,
          typename WriterT>
class LiveRunner {
 public:
  LiveRunner(const ProbeConfig& config, const ProbeConnectionConfig& connection,
             std::size_t session_index, std::size_t session_count,
             const config::InstrumentCatalog& instrument_catalog,
             DataReaderT& data_reader, FeedbackQueueT& feedback_queue,
             WriterT& writer, void* clock_context = nullptr,
             LiveRunnerNowFunction now_function = nullptr) noexcept
      : config_(config),
        connection_(connection),
        target_instrument_(
            instrument_catalog.Find(Exchange::kBitget, config.order.symbol)),
        data_reader_(data_reader),
        feedback_queue_(feedback_queue),
        writer_(writer),
        id_allocator_(
            config.feedback.strategy_id,
            1 + static_cast<std::uint64_t>(session_index) * 2,
            static_cast<std::uint64_t>(session_count == 0 ? 1 : session_count) *
                2),
        clock_context_(clock_context),
        now_function_(now_function) {
    const std::size_t expected_ids =
        static_cast<std::size_t>(config.sampling.samples_per_session) * 2;
    finalized_local_order_ids_.reserve(expected_ids);
    finalized_response_local_ids_.reserve(expected_ids);
  }

  void BindSession(SessionT& session) noexcept {
    session_ = &session;
  }

  static void RuntimeHookCallback(void* context) noexcept {
    static_cast<LiveRunner*>(context)->DriveHookOnce();
  }

  void GrantDispatch() noexcept {
    dispatch_grants_.fetch_add(1, std::memory_order_release);
  }

  void OnLoginReady() noexcept {
    login_ready_.store(true, std::memory_order_release);
  }

  void OnLoginNotReady() noexcept {
    login_ready_.store(false, std::memory_order_release);
    FailRun("order session login not ready");
  }

  void OnOrderSessionConnected(
      const bitget::OrderSessionConnectionInfo& info) noexcept {
    connection_info_ = info;
  }

  void OnOrderResponse(const bitget::OrderResponse& response) noexcept {
    if (failed()) {
      return;
    }
    if (!active_flow_.has_value()) {
      const auto finalized =
          finalized_response_local_ids_.find(response.request_sequence);
      if (finalized != finalized_response_local_ids_.end() &&
          finalized->second == response.local_order_id) {
        return;
      }
      FailRun("order response arrived without an active sample");
      return;
    }
    if (deadline_ns_ > 0 && response.local_receive_ns > deadline_ns_) {
      HandleTransition(active_flow_->OnTimeout(), response.local_receive_ns);
      return;
    }
    const ProbeSampleTransition transition =
        active_flow_->OnOrderResponse(response);
    HandleTransition(transition, response.local_receive_ns);
    if (active_flow_.has_value() &&
        response.request_sequence == active_flow_->stats().request_sequence &&
        response.kind == bitget::OrderResponseKind::kAck &&
        !active_flow_->stats().safety_close_sent) {
      deadline_ns_ = AddMilliseconds(response.local_receive_ns,
                                     config_.feedback.terminal_timeout_ms);
    }
  }

  void OnBookTicker(const BookTicker& ticker) noexcept {
    ++stats_.data_reader_events;
    if (ticker.exchange != Exchange::kBitget) {
      return;
    }
    if (target_instrument_ == nullptr ||
        ticker.symbol_id != target_instrument_->symbol_id) {
      return;
    }
    if (!latest_ticker_.has_value() ||
        ticker.local_ns >= latest_ticker_->local_ns) {
      latest_ticker_ = ticker;
    }
  }

  void OnTrade(const Trade&) noexcept {
    ++stats_.data_reader_events;
  }

  void DriveHookOnce() noexcept {
    if (failed()) {
      return;
    }
    data_reader_.Drain(*this, config_.sampling.max_events_per_drain);
    if (feedback_queue_.dropped_count() != 0) {
      FailRun("local feedback queue overflow");
      return;
    }

    PollFeedback();
    if (failed()) {
      return;
    }
    if (safety_close_pending_) {
      TrySubmitSafetyClose(NowNs());
    }
    if (failed()) {
      return;
    }

    const std::int64_t now_ns = NowNs();
    if (active_flow_.has_value() && deadline_ns_ > 0 &&
        now_ns >= deadline_ns_) {
      HandleTransition(active_flow_->OnTimeout(), now_ns);
      return;
    }
    if (active_flow_.has_value() ||
        !login_ready_.load(std::memory_order_acquire) || !HasDispatchGrant()) {
      return;
    }
    if (TryStartSample(now_ns)) {
      ++consumed_dispatch_grants_;
    }
  }

  [[nodiscard]] bool SampleFinished() const noexcept {
    return samples_finished_.load(std::memory_order_acquire) != 0;
  }
  [[nodiscard]] bool failed() const noexcept {
    return failed_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool login_ready() const noexcept {
    return login_ready_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool has_active_sample() const noexcept {
    return active_sample_.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::uint64_t samples_finished() const noexcept {
    return samples_finished_.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::uint64_t samples_completed() const noexcept {
    return samples_completed_.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::uint64_t samples_failed() const noexcept {
    return samples_failed_.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::string_view failure_reason() const noexcept {
    return failure_reason_;
  }
  [[nodiscard]] const LiveRunnerStats& stats() const noexcept {
    return stats_;
  }

 private:
  [[nodiscard]] std::int64_t NowNs() const noexcept {
    if (now_function_ != nullptr) {
      return now_function_(clock_context_);
    }
    return static_cast<std::int64_t>(websocket::RealtimeClockNowNs());
  }

  [[nodiscard]] static std::int64_t AddMilliseconds(
      std::int64_t now_ns, std::uint32_t milliseconds) noexcept {
    constexpr std::int64_t kNanosecondsPerMillisecond = 1'000'000;
    const std::int64_t delay =
        static_cast<std::int64_t>(milliseconds) * kNanosecondsPerMillisecond;
    if (now_ns > std::numeric_limits<std::int64_t>::max() - delay) {
      return std::numeric_limits<std::int64_t>::max();
    }
    return now_ns + delay;
  }

  [[nodiscard]] bool HasDispatchGrant() const noexcept {
    return consumed_dispatch_grants_ <
           dispatch_grants_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool HasFreshBbo(std::int64_t now_ns) const noexcept {
    return latest_ticker_.has_value() && latest_ticker_->local_ns > 0 &&
           now_ns >= latest_ticker_->local_ns &&
           static_cast<std::uint64_t>(now_ns - latest_ticker_->local_ns) <=
               config_.order.bbo_freshness_ns;
  }

  [[nodiscard]] bool TryStartSample(std::int64_t now_ns) noexcept {
    if (target_instrument_ == nullptr) {
      FailRun("missing configured Bitget probe instrument");
      return true;
    }
    if (!HasFreshBbo(now_ns)) {
      ++stats_.stale_book_tickers;
      return false;
    }
    ProbeOrderBuildResult built =
        BuildPassiveBuyIoc(*latest_ticker_, *target_instrument_,
                           config_.order.passive_price_limit_fraction);
    if (!built.ok) {
      FailRun(built.error);
      return true;
    }
    if (session_ == nullptr) {
      FailRun("order session is not bound");
      return true;
    }

    active_ids_ = id_allocator_.Next();
    built.order.local_order_id = active_ids_.ioc_local_order_id;
    active_order_ = built.order;
    active_instrument_ = target_instrument_;
    active_row_ = MakeSampleRow(built);
    active_flow_.emplace(active_ids_);
    active_sample_.store(true, std::memory_order_release);
    safety_close_pending_ = false;
    ++stats_.samples_started;

    const ProbeSampleTransition started = active_flow_->Start();
    if (!started.ok || started.action != ProbeSampleAction::kSubmitIoc) {
      HandleTransition(started, now_ns);
      return true;
    }
    const bitget::OrderSendResult sent = session_->PlaceOrder(active_order_);
    deadline_ns_ = AddMilliseconds(now_ns, config_.sessions.request_timeout_ms);
    HandleTransition(active_flow_->OnOrderSent(sent), now_ns);
    return true;
  }

  [[nodiscard]] SampleCsvRow MakeSampleRow(
      const ProbeOrderBuildResult& built) const {
    return SampleCsvRow{
        .run_id = config_.run_id,
        .session_name = connection_.name,
        .group = connection_.group,
        .host = connection_.host,
        .connect_ip = connection_.connect_ip,
        .port = connection_.port,
        .worker_cpu = connection_.worker_cpu_id,
        .owner_thread_cpu = connection_info_.owner_thread_cpu,
        .owner_thread_tid = connection_info_.owner_thread_tid,
        .cycle_index = stats_.samples_started,
        .sample_index = stats_.samples_started,
        .local_order_id = active_ids_.ioc_local_order_id,
        .close_local_order_id = active_ids_.close_local_order_id,
        .symbol = built.order.symbol,
        .side = std::string(magic_enum::enum_name(built.order.side)),
        .quantity_text = built.order.quantity_text,
        .price_text = built.order.price_text,
        .bbo_ticker_id = built.bbo_ticker_id,
        .bbo_local_ns = built.bbo_local_ns,
    };
  }

  void PollFeedback() noexcept {
    feedback_queue_.Poll(
        config_.feedback.poll_budget,
        [this](const OrderFeedbackEvent& event) noexcept {
          ++stats_.feedback_events;
          if (failed()) {
            return;
          }
          if (event.kind == OrderFeedbackKind::kContinuityLost) {
            if (!active_flow_.has_value()) {
              FailRun("feedback continuity lost without active sample");
              return;
            }
            HandleTransition(active_flow_->OnOrderFeedback(event), NowNs());
            return;
          }
          if (!active_flow_.has_value()) {
            if (finalized_local_order_ids_.contains(event.local_order_id)) {
              return;
            }
            FailRun("unmapped feedback arrived without an active sample");
            return;
          }
          if (event.local_order_id != active_ids_.ioc_local_order_id &&
              event.local_order_id != active_ids_.close_local_order_id) {
            if (finalized_local_order_ids_.contains(event.local_order_id)) {
              return;
            }
            FailRun("unmapped feedback for active session");
            return;
          }
          const bool contains_fill =
              event.kind == OrderFeedbackKind::kPartialFilled ||
              event.kind == OrderFeedbackKind::kFilled ||
              event.cumulative_filled_quantity > 0.0;
          if (!contains_fill &&
              event.kind != OrderFeedbackKind::kContinuityLost &&
              deadline_ns_ > 0 && event.local_receive_ns > deadline_ns_) {
            HandleTransition(active_flow_->OnTimeout(), event.local_receive_ns);
            return;
          }
          HandleTransition(active_flow_->OnOrderFeedback(event), NowNs());
        });
  }

  void HandleTransition(const ProbeSampleTransition& transition,
                        std::int64_t now_ns) noexcept {
    if (!active_flow_.has_value()) {
      return;
    }
    if (!transition.ok || transition.action == ProbeSampleAction::kFail) {
      FinishActiveSample(transition.error, false);
      return;
    }
    switch (transition.action) {
      case ProbeSampleAction::kSubmitSafetyClose:
        safety_close_pending_ = true;
        TrySubmitSafetyClose(now_ns);
        return;
      case ProbeSampleAction::kComplete:
        FinishActiveSample({}, true);
        return;
      case ProbeSampleAction::kNone:
      case ProbeSampleAction::kSubmitIoc:
      case ProbeSampleAction::kFail:
        return;
    }
  }

  void TrySubmitSafetyClose(std::int64_t now_ns) noexcept {
    if (!active_flow_.has_value() || !safety_close_pending_ ||
        active_flow_->stats().safety_close_sent) {
      return;
    }
    if (!HasFreshBbo(now_ns)) {
      return;
    }
    if (target_instrument_ == nullptr ||
        latest_ticker_->symbol_id != target_instrument_->symbol_id ||
        target_instrument_ != active_instrument_) {
      FinishActiveSample("missing matching instrument for safety close", false);
      return;
    }
    ProbeOrderBuildResult built =
        BuildSafetyCloseSellIoc(*latest_ticker_, *target_instrument_,
                                active_flow_->stats().observed_fill_quantity,
                                config_.order.passive_price_limit_fraction);
    if (!built.ok) {
      FinishActiveSample(built.error, false);
      return;
    }
    built.order.local_order_id = active_ids_.close_local_order_id;
    safety_close_order_ = built.order;
    const bitget::OrderSendResult sent =
        session_->PlaceOrder(safety_close_order_);
    safety_close_pending_ = false;
    deadline_ns_ =
        AddMilliseconds(now_ns, config_.feedback.terminal_timeout_ms);
    HandleTransition(active_flow_->OnSafetyCloseSent(sent), now_ns);
  }

  void FinishActiveSample(std::string_view error,
                          bool transition_completed) noexcept {
    if (!active_flow_.has_value()) {
      SetFailed(error.empty() ? "sample failed" : error);
      return;
    }
    const ProbeSampleStats sample = active_flow_->stats();
    finalized_local_order_ids_.insert(sample.ioc_local_order_id);
    if (sample.safety_close_sent) {
      finalized_local_order_ids_.insert(sample.close_local_order_id);
    }
    if (sample.request_sequence != 0) {
      finalized_response_local_ids_.emplace(sample.request_sequence,
                                            sample.ioc_local_order_id);
    }
    if (sample.close_request_sequence != 0) {
      finalized_response_local_ids_.emplace(sample.close_request_sequence,
                                            sample.close_local_order_id);
    }
    active_row_.request_sequence = sample.request_sequence;
    active_row_.close_request_sequence = sample.close_request_sequence;
    active_row_.exchange_order_id = sample.exchange_order_id;
    active_row_.close_exchange_order_id = sample.close_exchange_order_id;
    active_row_.request_send_ns = sample.request_send_ns;
    active_row_.response_receive_ns = sample.response_receive_ns;
    active_row_.response_exchange_ns = sample.response_exchange_ns;
    active_row_.ack_rtt_ns = sample.ack_rtt_ns;
    active_row_.response_kind =
        sample.place_response_observed
            ? std::string(magic_enum::enum_name(sample.response_kind))
            : std::string{};
    active_row_.error_code = sample.error_code;
    active_row_.connection_id_hash = sample.connection_id_hash;
    active_row_.close_request_send_ns = sample.close_request_send_ns;
    active_row_.close_response_receive_ns = sample.close_response_receive_ns;
    active_row_.close_response_exchange_ns = sample.close_response_exchange_ns;
    active_row_.close_ack_rtt_ns = sample.close_ack_rtt_ns;
    active_row_.close_response_kind =
        sample.close_response_observed
            ? std::string(magic_enum::enum_name(sample.close_response_kind))
            : std::string{};
    active_row_.close_error_code = sample.close_error_code;
    active_row_.close_connection_id_hash = sample.close_connection_id_hash;
    active_row_.terminal_feedback_kind =
        sample.terminal_feedback_observed
            ? std::string(magic_enum::enum_name(sample.terminal_feedback_kind))
            : std::string{};
    active_row_.terminal_feedback_local_ns = sample.terminal_feedback_local_ns;
    active_row_.terminal_feedback_exchange_ns =
        sample.terminal_feedback_exchange_ns;
    active_row_.terminal_finish_reason =
        sample.terminal_feedback_observed
            ? std::string(magic_enum::enum_name(sample.terminal_finish_reason))
            : std::string{};
    active_row_.cumulative_fill = sample.observed_fill_quantity;
    active_row_.invalid = sample.invalid || !sample.normal_terminal_confirmed;
    active_row_.unexpected_fill = sample.unexpected_fill;
    active_row_.safety_close_requested = sample.safety_close_requested;
    active_row_.safety_close_sent = sample.safety_close_sent;
    active_row_.safety_close_confirmed = sample.safety_close_confirmed;
    active_row_.safety_close_filled_quantity =
        sample.safety_close_filled_quantity;
    active_row_.invalid_reason = sample.invalid_reason;
    if (!error.empty() && active_row_.invalid_reason.empty()) {
      active_row_.invalid_reason.assign(error.data(), error.size());
    }

    const bool normal_success = transition_completed &&
                                sample.normal_terminal_confirmed &&
                                !sample.invalid;
    if (normal_success) {
      active_row_.outcome = "normal_terminal_confirmed";
    } else if (sample.safety_close_confirmed) {
      active_row_.outcome = "safety_close_confirmed_but_run_invalid";
    } else {
      active_row_.outcome = "failed";
    }

    std::string write_error;
    const bool write_ok = writer_.Write(active_row_, &write_error);
    active_flow_.reset();
    active_instrument_ = nullptr;
    active_ids_ = {};
    active_order_ = {};
    safety_close_order_ = {};
    active_row_ = {};
    deadline_ns_ = 0;
    safety_close_pending_ = false;
    active_sample_.store(false, std::memory_order_release);

    if (normal_success && write_ok) {
      ++stats_.samples_completed;
      samples_completed_.fetch_add(1, std::memory_order_release);
      samples_finished_.fetch_add(1, std::memory_order_release);
      return;
    }
    ++stats_.samples_failed;
    samples_failed_.fetch_add(1, std::memory_order_release);
    if (!write_ok) {
      SetFailed(write_error.empty() ? "sample CSV write failed" : write_error);
    } else if (!error.empty()) {
      SetFailed(error);
    } else if (!sample.invalid_reason.empty()) {
      SetFailed(sample.invalid_reason);
    } else {
      SetFailed("sample did not reach normal terminal confirmation");
    }
    samples_finished_.fetch_add(1, std::memory_order_release);
  }

  void FailRun(std::string_view reason) noexcept {
    if (failed()) {
      return;
    }
    if (active_flow_.has_value()) {
      FinishActiveSample(reason, false);
      return;
    }
    SetFailed(reason);
  }

  void SetFailed(std::string_view reason) noexcept {
    if (failed_.load(std::memory_order_relaxed)) {
      return;
    }
    failure_reason_.assign(reason.data(), reason.size());
    failed_.store(true, std::memory_order_release);
    if (session_ != nullptr) {
      session_->Stop();
    }
  }

  const ProbeConfig& config_;
  const ProbeConnectionConfig& connection_;
  const config::InstrumentInfo* target_instrument_{nullptr};
  DataReaderT& data_reader_;
  FeedbackQueueT& feedback_queue_;
  WriterT& writer_;
  ProbeSampleIdAllocator id_allocator_;
  SessionT* session_{nullptr};
  void* clock_context_{nullptr};
  LiveRunnerNowFunction now_function_{nullptr};
  std::optional<BookTicker> latest_ticker_;
  std::optional<ProbeSampleFlow> active_flow_;
  const config::InstrumentInfo* active_instrument_{nullptr};
  ProbeSampleLocalIds active_ids_;
  ProbeWireOrder active_order_;
  ProbeWireOrder safety_close_order_;
  SampleCsvRow active_row_;
  bitget::OrderSessionConnectionInfo connection_info_;
  LiveRunnerStats stats_;
  std::string failure_reason_;
  std::atomic<std::uint64_t> dispatch_grants_{0};
  std::uint64_t consumed_dispatch_grants_{0};
  std::atomic<std::uint64_t> samples_finished_{0};
  std::atomic<std::uint64_t> samples_completed_{0};
  std::atomic<std::uint64_t> samples_failed_{0};
  std::atomic<bool> login_ready_{false};
  std::atomic<bool> active_sample_{false};
  std::atomic<bool> failed_{false};
  absl::flat_hash_set<std::uint64_t> finalized_local_order_ids_;
  absl::flat_hash_map<std::uint64_t, std::uint64_t>
      finalized_response_local_ids_;
  std::int64_t deadline_ns_{0};
  bool safety_close_pending_{false};
};

}  // namespace aquila::tools::bitget_order_session_rtt_probe

#endif  // AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_LIVE_RUNNER_H_
