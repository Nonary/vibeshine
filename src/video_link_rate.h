/**
 * @file src/video_link_rate.h
 * @brief Experimental host-side link rate adaptation for ordinary H.264/HEVC/AV1 video.
 *
 * Lowers the encoder bitrate when the network cannot carry the stream, probes
 * back up and returns to the requested bitrate after clean seconds. A pure
 * state machine: the session feeds it what the host sent and what the client
 * reports back, a periodic tick() returns the encoder bitrate to use, and
 * every instant is passed in, so tests are deterministic.
 *
 * It acts only on loss that FEC cannot absorb: unrecoverable frames, a share
 * of missing packets FEC is absorbing while the round trip time is raised,
 * and frames the host could not send whole. The one delay signal is a
 * lossless queue standing for seconds right after a probe. On a healthy link
 * it never returns a target, so the stream takes exactly the path it takes
 * without it. After every change it ignores reports for frames sent before
 * the change took effect (the fence), so the queue left over from the old
 * rate cannot cut again.
 *
 * Inactive when disabled, for PyroWave and once the client sets the bitrate
 * itself through /bitrate.
 *
 * Calling it:
 * - on_sent() once the host has sent a frame, with the wire frame number the
 *   client reports, the packets (data and parity) and the link-layer bytes
 *   that left. A report that arrives before on_sent() for its frame has
 *   nothing to count against and is dropped: loss is under-counted, never
 *   over-counted.
 * - on_status() with validated, de-duplicated FEC block reports, then
 *   on_unrecoverable() for a block the client could not rebuild.
 * - tick() from the control loop with the RTT, FEC percentage, frame rate,
 *   the encoder bitrate now in effect and whether the picture is moving. A
 *   returned kbps is applied through the runtime bitrate path; nothing means
 *   the requested bitrate. take_change() reports what the tick changed.
 *
 * Units: the controller works in total wire bits per second (video with FEC
 * and framing plus an audio reserve) and converts to encoder kbps at the
 * current FEC percentage with wire_budget_t.
 *
 * A port of Rubylight's `link_rate.rs`, without its keyframe-request trigger
 * and pacing-latch watch, which depend on classifiers this host does not
 * have. The controller is not thread-safe: callers serialize access.
 */
#pragma once

#include "video_adaptive_pacing.h"
#include "video_fec_status.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace stream::link_rate {
  using clock = std::chrono::steady_clock;
  using time_point = clock::time_point;
  using duration = clock::duration;

  /// Every evidence test runs over this window.
  inline constexpr duration evidence_window = std::chrono::seconds {2};
  /// Evidence is evaluated at most this often.
  inline constexpr duration evaluate_every = std::chrono::milliseconds {100};
  /// Distinct unrecoverable frames in the window that cut the bitrate (E1).
  inline constexpr std::size_t unrecoverable_frames = 3;
  /// Share of the packets the host sent that the FEC reports show missing,
  /// and that cuts (E2). Blocks FEC rebuilt count their missing data packets
  /// only while the RTT is elevated: a report is made once enough packets are
  /// in, so what is still in flight reads as missing, but with a queue filling
  /// the same share is real loss FEC is absorbing.
  inline constexpr std::uint64_t loss_share_percent = 5;
  /// E2 needs the share in this many consecutive windows of loss_window.
  inline constexpr std::uint8_t loss_windows = 2;
  inline constexpr duration loss_window = std::chrono::milliseconds {500};
  /// A window in which the host sent fewer packets than this says nothing: a
  /// quiet stream loses 5% to a single packet.
  inline constexpr std::uint64_t loss_min_packets = 100;
  /// Frames lost on send (socket buffer exhaustion and the like) that cut (E3).
  inline constexpr std::size_t send_loss_frames = 3;
  /// The loss share a cut will believe. Losses come in bursts, so a short
  /// window can read 100% on a link that carries half the rate.
  inline constexpr double max_loss_share = 0.75;
  /// Rebuilt blocks the running share of reordered data packets averages over.
  inline constexpr double reorder_blocks = 64.;
  /// The running reordering share fades with this time constant while no
  /// flat-RTT block renews it.
  inline constexpr duration reorder_fade = std::chrono::seconds {20};
  /// A silence this long between rebuilt blocks fades the baseline even while
  /// the RTT is up.
  inline constexpr duration reorder_gap = std::chrono::seconds {1};
  /// Reordering leaves at most the block's parity share in flight; the
  /// baseline is held to that plus this margin.
  inline constexpr double reorder_margin = 0.02;
  /// How far below the baseline the excess of rebuilt data loss may sink, in
  /// packets, so that a run of clean blocks pays for the next noisy ones.
  inline constexpr std::int64_t excess_credit_cap = 64;
  /// A cut aims at this share of the delivered rate, FEC included.
  inline constexpr std::uint64_t delivered_percent = 85;
  /// One cut lowers the target by at least 15% ...
  inline constexpr std::uint64_t max_cut_percent = 85;
  /// ... and a first cut at most by half. A further cut within the evidence
  /// window is held only to repeat_cut_floor_bps.
  inline constexpr std::uint64_t min_cut_percent = 50;
  inline constexpr std::uint64_t repeat_cut_floor_bps = 1'000'000;
  /// Target after a cut when the delivered rate is unknown.
  inline constexpr std::uint64_t unknown_share_percent = 70;
  /// After a change, evidence from frames sent within this long is ignored so
  /// the bottleneck queue can drain; the longer of this and
  /// `2 x RTT + fence_margin` applies.
  inline constexpr duration min_fence = std::chrono::milliseconds {500};
  inline constexpr duration fence_margin = std::chrono::milliseconds {250};
  /// Frames in the encoder when the bitrate changes; the fence frame is this
  /// far past the next frame sent.
  inline constexpr std::uint32_t pipeline_frames = 2;
  /// After a cut, frames that keep running more than this share above the
  /// target mean the encoder cannot go that low: cutting again only collapses
  /// the target while the stream stays the same.
  inline constexpr std::uint64_t encoder_overshoot_percent = 120;
  /// Wait after a step down before the first probe.
  inline constexpr duration hold = std::chrono::seconds {2};
  /// A failed probe doubles the wait up to this.
  inline constexpr duration max_hold = std::chrono::seconds {8};
  /// A probe that fails within knee_percent of the previous bad rate means the
  /// link has a fixed knee there. Each such failure doubles a wait, up to this,
  /// during which probes stay under knee_cap_percent of the bad rate.
  inline constexpr duration knee_max_hold = std::chrono::seconds {30};
  /// A failed probe, or a cut made with the RTT elevated, keeps probes under
  /// the knee at least this long.
  inline constexpr duration knee_quiet = std::chrono::seconds {20};
  inline constexpr std::uint64_t knee_backoff_percent = 85;
  inline constexpr std::uint64_t knee_percent = 5;
  /// A standing queue: the RTT stays this far above the lowest of the last
  /// queue_floor_window for queue_hold, in every sample, while video is busy.
  inline constexpr duration queue_margin = std::chrono::milliseconds {15};
  inline constexpr duration queue_hold = std::chrono::seconds {2};
  inline constexpr duration queue_floor_window = std::chrono::seconds {30};
  /// The queue has to start within this long of a probe step.
  inline constexpr duration queue_raised = std::chrono::seconds {8};
  /// A queue cut waits this long for the queue to drain before it looks again.
  inline constexpr duration queue_settle = std::chrono::seconds {1};
  /// A queue cut goes down by 12%: the stream is near the link's edge.
  inline constexpr std::uint64_t queue_cut_percent = 88;
  /// A queue a cut did not change (within a quarter or this) is no queue: a
  /// slower route or another flow.
  inline constexpr duration queue_flat = std::chrono::milliseconds {5};
  /// A queue three cuts did not drain is another flow's.
  inline constexpr std::uint8_t queue_steps_max = 3;
  inline constexpr std::size_t floor_buckets = 32;
  /// A step down from within this share of the bad rate is a knee failure too.
  inline constexpr std::uint64_t knee_cut_percent = 90;
  inline constexpr std::uint64_t knee_cap_percent = 90;
  /// Minimum time between probes (also at least the fence).
  inline constexpr duration probe_every = std::chrono::milliseconds {500};
  /// Probe step as a fraction of the current rate: 1/8.
  inline constexpr std::uint64_t probe_step_divisor = 8;
  /// Step 1/20 within near_bad_percent of the last bad rate, one step at a
  /// time and confirmed before the next.
  inline constexpr std::uint64_t near_step_divisor = 20;
  inline constexpr std::uint64_t near_bad_percent = 90;
  /// A probe step counts as good after this long without evidence.
  inline constexpr duration confirm_after = evidence_window;
  /// `bad` is forgotten after this long clean at 95% of it or more.
  inline constexpr duration forget_bad_after = std::chrono::seconds {60};
  /// Back to the requested bitrate after this long clean at the ceiling.
  inline constexpr duration full_after = std::chrono::seconds {10};
  /// A control ping this recent counts as fresh feedback.
  inline constexpr duration ping_fresh = std::chrono::seconds {2};
  /// The highest bitrate this controller asks for, kbps.
  inline constexpr std::uint32_t max_kbps = 500'000;
  /// The lowest bitrate a cut goes to, unless the stream asked for less.
  inline constexpr std::uint32_t min_kbps = 2'000;

  /// Converts between encoder kbps and total wire bits per second.
  struct wire_budget_t {
    std::size_t frame_header_bytes {};
    /// Video bytes in one packet.
    std::size_t payload_blocksize {};
    /// One packet on the link, headers included.
    std::size_t wire_bytes {};
    std::size_t min_parity_packets {};
    /// Reserve for audio and control traffic.
    std::uint64_t audio_bps {};

    /// Audio reserve for the negotiated channels: Opus plus audio FEC,
    /// encryption and framing, and control traffic.
    static std::uint64_t audio_reserve_bps(int channels, bool high_quality) {
      return std::uint64_t(std::max(channels, 0)) * (high_quality ? 256 : 96) * 1500 + 500'000;
    }

    std::uint64_t video_bps(std::uint32_t kbps, int fec, int framerate) const {
      return pacing::nominal_wire_demand_bps(int(std::min<std::uint32_t>(kbps, std::numeric_limits<int>::max())), framerate, frame_header_bytes, payload_blocksize, wire_bytes, fec, min_parity_packets);
    }

    /// The highest encoder bitrate, at most `ceiling`, whose video fits 90% of
    /// what `total_bps` leaves after the audio reserve. The other 10% is
    /// headroom for rate-control overshoot and keyframes.
    std::uint32_t encoder_kbps(std::uint64_t total_bps, std::uint32_t ceiling, int fec, int framerate) const {
      const std::uint64_t video = (total_bps > audio_bps ? total_bps - audio_bps : 0) * 9 / 10;
      std::uint32_t lo = 1;
      std::uint32_t hi = std::max<std::uint32_t>(ceiling, 1);
      while (lo < hi) {
        const auto mid = lo + (hi - lo + 1) / 2;
        if (video_bps(mid, fec, framerate) <= video) {
          lo = mid;
        } else {
          hi = mid - 1;
        }
      }
      return lo;
    }

    /// Round up so converting back to an encoder bitrate cannot lose the last kbps.
    std::uint64_t total_bps(std::uint32_t kbps, int fec, int framerate) const {
      return (video_bps(kbps, fec, framerate) * 10 + 8) / 9 + audio_bps;
    }
  };

  enum class state_e {
    off,  ///< Disabled, PyroWave, or the client owns the bitrate.
    full,  ///< At the requested bitrate. tick() returns nothing.
    limited,  ///< Below the requested bitrate because of the network, probing back up.
    floor,  ///< At the minimum bitrate and still losing.
  };

  inline const char *state_name(state_e state) {
    switch (state) {
      case state_e::off:
        return "off";
      case state_e::full:
        return "full";
      case state_e::limited:
        return "limited";
      case state_e::floor:
        return "floor";
    }
    return "off";
  }

  enum class change_kind_e {
    step_down,
    probe,
    probe_failed,
    floor,
    /// The encoder keeps sending far more than the target; the rate it sends
    /// is now the lowest this controller asks for.
    encoder_floor,
    full,
  };

  inline const char *change_kind_name(change_kind_e kind) {
    switch (kind) {
      case change_kind_e::step_down:
        return "step_down";
      case change_kind_e::probe:
        return "probe";
      case change_kind_e::probe_failed:
        return "probe_failed";
      case change_kind_e::floor:
        return "floor";
      case change_kind_e::encoder_floor:
        return "encoder_floor";
      case change_kind_e::full:
        return "full";
    }
    return "full";
  }

  /// The evidence behind a step down, for the log.
  struct evidence_t {
    std::size_t unrecoverable_frames {};
    std::size_t send_losses {};
    /// Consecutive 500 ms windows at or over loss_share_percent.
    std::uint8_t loss_windows {};
    /// Missing packets over packets sent, 0..1.
    double loss_share {};

    bool operator==(const evidence_t &) const = default;
  };

  /// One change of state or bitrate, taken with take_change() and logged.
  struct change_t {
    change_kind_e kind {change_kind_e::full};
    /// "unrecoverable_frames", "fec_loss_share", "send_losses", "queue",
    /// "probe", "encoder_floor" or "recovered".
    const char *reason {""};
    std::uint32_t from_kbps {};
    std::uint32_t to_kbps {};
    /// Video wire rate the link delivered, kbps (0 when unknown).
    std::uint32_t delivered_kbps {};
    evidence_t evidence;
  };

  struct snapshot_t {
    state_e state {state_e::off};
    const char *reason {""};
    std::uint32_t ceiling_kbps {};
    std::uint32_t current_kbps {};
    std::uint64_t delivered_kbps {};
    std::uint32_t bad_kbps {};
    std::uint32_t encoder_floor_kbps {};
    std::uint32_t steps_down {};
    std::uint32_t probes {};
    std::uint32_t failed_probes {};
    std::uint64_t rebuilt_blocks {};
    /// Data packets of rebuilt blocks reported missing, over all their packets.
    double rebuilt_data_missing_percent {};
    std::uint64_t rtt_ms {};
    std::uint64_t rtt_floor_ms {};
  };

  struct link_rate_access_t;

  class link_rate_t {
  public:
    /// Off, reason "disabled".
    link_rate_t() = default;

    /// `requested_kbps` is the encoder bitrate the stream started with. The
    /// budget converts it to wire bits per second.
    link_rate_t(bool enabled, bool pyrowave, std::uint32_t requested_kbps, const wire_budget_t &budget) {
      if (!enabled) {
        return;
      }
      if (pyrowave) {
        reason_ = "pyrowave";
        return;
      }
      if (requested_kbps == 0 || budget.payload_blocksize == 0 || budget.wire_bytes == 0) {
        reason_ = "no_bitrate";
        return;
      }
      ceiling_kbps_ = std::min(requested_kbps, max_kbps);
      floor_kbps_ = std::min(ceiling_kbps_, min_kbps);
      budget_ = budget;
      ring_.assign(ring_size, sent_t {});
      events_.unrecoverable.reserve(event_cap);
      events_.send_losses.reserve(event_cap);
      state_ = state_e::full;
      reason_ = "healthy";
    }

    state_e state() const {
      return state_;
    }

    const char *reason() const {
      return reason_;
    }

    std::uint32_t ceiling_kbps() const {
      return ceiling_kbps_;
    }

    /// The client set the bitrate itself (/bitrate); this controller stays off
    /// for the rest of the stream.
    void client_takeover() {
      stop("client_controlled");
    }

    /// The encoder cannot apply a bitrate without rebuilding itself, which
    /// costs a keyframe each time: this controller stays off.
    void encoder_rebuilds() {
      stop("encoder_rebuilds");
    }

    /// Record the video frame the host just sent: its wire frame number,
    /// packets (data and parity) and link-layer bytes.
    void on_sent(std::uint32_t frame, std::uint32_t packets, std::uint64_t wire_bytes, time_point now) {
      if (!active()) {
        return;
      }
      if (!last_sent_ || newer(frame, *last_sent_)) {
        last_sent_ = frame;
      }
      ring_[frame % ring_size] = sent_t {frame, packets, std::uint32_t(std::min<std::uint64_t>(wire_bytes, std::numeric_limits<std::uint32_t>::max())), 0, now, false};
      events_.sent += packets;
    }

    /// One validated, de-duplicated block of a client FEC status report.
    /// Feeds the packet-loss share (E2).
    void on_status(const pacing::fec_status_t &status, time_point now) {
      if (!active() || !admits(status.frame, now)) {
        return;
      }
      auto *sent = find_sent(status.frame);
      // Packets of a frame the host stopped sending are its own loss, counted
      // once on send (E3), not as the network's.
      if (!sent || sent->abandoned) {
        return;
      }
      const std::uint32_t total = std::uint32_t(status.data) + status.parity;
      const std::uint32_t received = std::uint32_t(status.received_data) + status.received_parity;
      // A block the client rebuilt is FEC's job and says nothing about the
      // link on its own: it is reported the moment `data` packets are in, so
      // the parity (and, on a path that reorders, some data) still on its way
      // reads as missing. While the RTT is up, a queue is filling, and the
      // same share is loss FEC is absorbing for now.
      if (received >= status.data) {
        const std::uint32_t data_missing = status.data > status.received_data ? std::uint32_t(status.data - status.received_data) : 0;
        ++rebuilt_blocks_;
        rebuilt_packets_ += total;
        rebuilt_data_missing_ += data_missing;
        const std::uint64_t data = std::max<std::uint16_t>(status.data, 1);
        const bool elevated = rtt_elevated();
        // The baseline fades with the time since the last block that renewed
        // it; while the RTT is up only a silence counts, or a long queue would
        // fade its own baseline and cut itself.
        if (reorder_at_) {
          const auto dt = since(now, *reorder_at_);
          if (!elevated || dt >= reorder_gap) {
            reorder_share_ *= std::exp(-seconds(dt) / seconds(reorder_fade));
          }
        }
        reorder_at_ = now;
        const double cap = double(status.parity) / double(std::max<std::uint32_t>(total, 1)) + reorder_margin;
        if (!elevated) {
          // What this path reorders, learned while nothing queues.
          const double share = double(data_missing) / double(data);
          reorder_share_ += (share - reorder_share_) / reorder_blocks;
          reorder_share_ = std::min(reorder_share_, cap);
          excess_credit_ = 0;
          excess_high_ = 0;
        }
        // Only the packets over what the flat RTT already showed are a queue.
        // The excess is a running sum; only what it rises over its own
        // high-water mark counts, so noise around the baseline adds up to
        // nothing and a standing queue's extra loss adds up.
        std::uint64_t excess = 0;
        if (elevated) {
          const auto expected = std::int64_t(std::llround(std::min(reorder_share_, cap) * double(data)));
          const auto credit = std::max<std::int64_t>(excess_credit_ + std::int64_t(data_missing) - expected, -excess_credit_cap);
          const auto counted = std::max<std::int64_t>(credit - excess_high_, 0);
          excess_high_ = std::max(excess_high_, credit);
          excess_credit_ = credit;
          excess = std::uint64_t(counted);
        }
        if (excess > 0) {
          // The parity in flight cannot be told from lost, so the share is
          // taken from the data packets and scaled up to all of them.
          const auto missing = (excess * total + data - 1) / data;
          events_.missing += missing;
          events_.lost(status.frame);
          sent->missing = saturating_add(sent->missing, missing);
        }
        return;
      }
      const auto missing = total - received;
      if (missing > 0) {
        events_.missing += missing;
        events_.lost(status.frame);
        sent->missing = saturating_add(sent->missing, missing);
      }
    }

    /// A distinct frame the client could not rebuild. Feeds E1.
    void on_unrecoverable(std::uint32_t frame, time_point now) {
      if (!active()) {
        return;
      }
      if (const auto *sent = find_sent(frame); sent && sent->abandoned) {
        return;
      }
      if (admits(frame, now)) {
        distinct(events_.unrecoverable, now, frame);
        events_.lost(frame);
      }
    }

    /// A frame the host could not send whole, after on_sent() for it. Feeds E3.
    void on_send_loss(std::uint32_t frame, time_point now) {
      if (!active()) {
        return;
      }
      if (auto *sent = find_sent(frame)) {
        sent->abandoned = true;
      }
      if (admits(frame, now)) {
        distinct(events_.send_losses, now, frame);
        events_.lost(frame);
      }
    }

    /// A standard client control ping (payload starting 04 00 00 00, 8 bytes),
    /// the proof that feedback is fresh.
    void on_ping(time_point now, std::string_view payload) {
      if (payload.size() == 8 && payload[0] == 4 && payload[1] == 0 && payload[2] == 0 && payload[3] == 0) {
        last_ping_ = now;
      }
    }

    /// Call every control loop pass; it evaluates every 100 ms. Returns the
    /// encoder bitrate to use while limited, nothing while at the requested
    /// bitrate or off. `busy` is whether video traffic is busy (at least 60% of
    /// the frames and bytes expected), `rtt` the control RTT (zero if unknown).
    std::optional<std::uint32_t> tick(time_point now, duration rtt, int fec, int framerate, std::uint32_t current_kbps, bool busy) {
      if (!active()) {
        return std::nullopt;
      }
      framerate = std::max(framerate, 1);
      rtt_ = rtt;
      if (!next_eval_ || now >= *next_eval_) {
        next_eval_ = now + evaluate_every;
        observe_rtt(now, rtt);
        events_.roll(now);
        events_.prune(now);
        if (const auto why = triggered()) {
          queue_since_.reset();
          on_evidence(now, *why, fec, framerate, current_kbps);
        } else if (standing_queue(now, busy)) {
          queue_cut(now, fec, framerate, current_kbps);
        } else if (state_ != state_e::full) {
          advance(now, busy, fec, framerate);
        }
      }
      if (state_ == state_e::full) {
        return std::nullopt;
      }
      return encoder_kbps(fec, framerate);
    }

    /// The change made by the last tick(), once.
    std::optional<change_t> take_change() {
      return std::exchange(change_, std::nullopt);
    }

    snapshot_t snapshot() const {
      snapshot_t s;
      s.state = state_;
      s.reason = reason_;
      s.ceiling_kbps = ceiling_kbps_;
      s.current_kbps = cache_ && (state_ == state_e::limited || state_ == state_e::floor) ? cache_->kbps : ceiling_kbps_;
      s.delivered_kbps = delivered_bps_ / 1000;
      s.bad_kbps = bad_ ? kbps_of(*bad_) : 0;
      s.encoder_floor_kbps = encoder_floor_ ? kbps_of(*encoder_floor_) : 0;
      s.steps_down = steps_down_;
      s.probes = probes_;
      s.failed_probes = failed_probes_;
      s.rebuilt_blocks = rebuilt_blocks_;
      s.rebuilt_data_missing_percent = rebuilt_packets_ ? double(rebuilt_data_missing_) * 100. / double(rebuilt_packets_) : 0.;
      s.rtt_ms = std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(rtt_).count());
      const auto floor = rtt_floor();
      s.rtt_floor_ms = floor ? std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(*floor).count()) : 0;
      return s;
    }

  private:
    friend struct link_rate_access_t;

    static constexpr std::size_t ring_size = 1024;
    static constexpr std::size_t event_cap = 32;
    static constexpr std::size_t pending_cap = 4;
    static constexpr std::uint32_t half_range = 0x8000'0000u;

    struct sent_t {
      std::uint32_t frame {};
      std::uint32_t packets {};
      std::uint32_t bytes {};
      /// Packets the client reported missing.
      std::uint32_t missing {};
      std::optional<time_point> at;
      /// The host stopped sending this frame part way.
      bool abandoned {};
    };

    /// Evidence counts only for frames sent at or after `frame` and `at`.
    struct fence_t {
      std::optional<std::uint32_t> frame;
      time_point at;

      bool admits(std::optional<std::uint32_t> f, time_point sent) const {
        return sent >= at && (!frame || !f || std::uint32_t(*f - *frame) < half_range);
      }
    };

    /// A confirmed probe step, kept for one evidence window: loss that FEC
    /// absorbs for two seconds shows up just after the step was confirmed.
    struct confirmed_t {
      std::uint64_t before;  ///< The good rate before the step.
      std::uint64_t rate;
      time_point at;
    };

    struct step_t {
      std::uint64_t rate;
      time_point at;
    };

    struct trigger_t {
      const char *reason;
      evidence_t evidence;
    };

    struct cache_t {
      std::uint64_t current;
      int fec;
      int framerate;
      std::uint32_t kbps;
    };

    struct events_t {
      std::vector<std::pair<time_point, std::uint32_t>> unrecoverable;
      std::vector<std::pair<time_point, std::uint32_t>> send_losses;
      std::optional<time_point> window;
      /// Packets reported missing and packets sent in the current E2 window.
      std::uint64_t missing {};
      std::uint64_t sent {};
      std::uint8_t hot {};
      /// The oldest and newest frames with loss evidence: the delivered rate
      /// is measured between them, not diluted by clean frames before the
      /// drop or by frames not reported yet.
      std::optional<std::uint32_t> first_loss;
      std::optional<std::uint32_t> last_loss;

      void clear() {
        unrecoverable.clear();
        send_losses.clear();
        window.reset();
        missing = 0;
        sent = 0;
        hot = 0;
        first_loss.reset();
        last_loss.reset();
      }

      void lost(std::uint32_t frame) {
        if (!last_loss || std::uint32_t(frame - *last_loss) < half_range) {
          last_loss = frame;
        }
        if (!first_loss || std::uint32_t(*first_loss - frame) < half_range) {
          first_loss = frame;
        }
      }

      void prune(time_point now) {
        const auto old = [&](const std::pair<time_point, std::uint32_t> &event) {
          return since(now, event.first) >= evidence_window;
        };
        std::erase_if(unrecoverable, old);
        std::erase_if(send_losses, old);
      }

      /// Closes the finished 500 ms windows of reported loss.
      void roll(time_point now) {
        if (!window) {
          window = now;
          missing = 0;
          sent = 0;
          return;
        }
        if (since(now, *window) < loss_window) {
          return;
        }
        const bool is_hot = sent >= loss_min_packets && missing * 100 >= sent * loss_share_percent;
        hot = is_hot ? std::uint8_t(std::min<int>(hot + 1, 255)) : 0;
        missing = 0;
        sent = 0;
        window = now;
      }
    };

    static duration since(time_point now, time_point at) {
      return now > at ? now - at : duration::zero();
    }

    static double seconds(duration d) {
      return std::chrono::duration<double>(d).count();
    }

    static bool newer(std::uint32_t frame, std::uint32_t than) {
      return std::uint32_t(frame - than) < half_range;
    }

    static std::uint32_t saturating_add(std::uint32_t a, std::uint64_t b) {
      return std::uint32_t(std::min<std::uint64_t>(std::uint64_t(a) + b, std::numeric_limits<std::uint32_t>::max()));
    }

    static void distinct(std::vector<std::pair<time_point, std::uint32_t>> &events, time_point now, std::uint32_t frame) {
      if (std::any_of(events.begin(), events.end(), [&](const auto &event) {
            return event.second == frame;
          })) {
        return;
      }
      if (events.size() >= event_cap) {
        events.erase(events.begin());
      }
      events.emplace_back(now, frame);
    }

    void stop(const char *why) {
      state_ = state_e::off;
      reason_ = why;
      events_.clear();
      pending_ = {};
      confirmed_.reset();
    }

    bool active() const {
      return budget_.has_value() && state_ != state_e::off;
    }

    sent_t *find_sent(std::uint32_t frame) {
      if (ring_.empty()) {
        return nullptr;
      }
      auto &sent = ring_[frame % ring_size];
      return sent.at && sent.frame == frame ? &sent : nullptr;
    }

    const sent_t *find_sent(std::uint32_t frame) const {
      return const_cast<link_rate_t *>(this)->find_sent(frame);
    }

    bool admits(std::optional<std::uint32_t> frame, time_point now) const {
      if (!fence_) {
        return true;
      }
      const sent_t *sent = frame ? find_sent(*frame) : nullptr;
      return fence_->admits(frame, sent ? *sent->at : now);
    }

    duration fence_wait() const {
      return std::max(min_fence, rtt_ * 2 + fence_margin);
    }

    void set_fence(time_point now) {
      fence_ = fence_t {
        last_sent_ ? std::optional<std::uint32_t> {*last_sent_ + 1 + pipeline_frames} : std::nullopt,
        now + fence_wait(),
      };
      events_.clear();
    }

    bool ping_is_fresh(time_point now) const {
      return last_ping_ && since(now, *last_ping_) < ping_fresh;
    }

    void observe_rtt(time_point now, duration rtt) {
      if (rtt <= duration::zero() || !ping_is_fresh(now)) {
        return;
      }
      minimum_rtt_ = minimum_rtt_ ? std::min(*minimum_rtt_, rtt) : rtt;
      if (!rtt_floor_.empty() && since(now, rtt_floor_.back().first) < std::chrono::seconds {1}) {
        rtt_floor_.back().second = std::min(rtt_floor_.back().second, rtt);
      } else {
        if (rtt_floor_.size() >= floor_buckets) {
          rtt_floor_.pop_front();
        }
        rtt_floor_.emplace_back(now, rtt);
      }
      while (!rtt_floor_.empty() && since(now, rtt_floor_.front().first) > queue_floor_window) {
        rtt_floor_.pop_front();
      }
    }

    std::optional<duration> rtt_floor() const {
      if (rtt_floor_.empty()) {
        return std::nullopt;
      }
      auto low = rtt_floor_.front().second;
      for (const auto &[at, value] : rtt_floor_) {
        low = std::min(low, value);
      }
      return low;
    }

    bool rtt_elevated() const {
      return rtt_ > duration::zero() && minimum_rtt_ && rtt_ > *minimum_rtt_ + std::max<duration>(*minimum_rtt_ / 2, std::chrono::milliseconds {8});
    }

    /// The RTT has been over the floor by queue_margin in every sample for
    /// queue_hold while video is busy: a lossless queue stands in the path. A
    /// queue that a cut did not change is no queue: the floor is re-learned.
    bool standing_queue(time_point now, bool busy) {
      const auto floor = rtt_floor();
      std::optional<duration> excess;
      if (floor && rtt_ > duration::zero()) {
        excess = rtt_ > *floor ? rtt_ - *floor : duration::zero();
      }
      if (excess && *excess <= queue_margin) {
        // The RTT came back: the queue is over, the next is a new one.
        queue_excess_.reset();
        queue_steps_ = 0;
      }
      if (!(busy && ping_is_fresh(now) && excess && *excess > queue_margin)) {
        queue_since_.reset();
        return false;
      }
      if (queue_quiet_until_ && now < *queue_quiet_until_) {
        return false;
      }
      if (!queue_since_) {
        // Only a queue the stream's own rate can have made: one that starts
        // soon after a probe step, or goes on after a cut.
        const bool caused = queue_excess_.has_value() || (raised_at_ && since(now, *raised_at_) <= queue_raised);
        if (!caused) {
          return false;
        }
        queue_since_ = now;
      }
      if (since(now, *queue_since_) < queue_hold) {
        return false;
      }
      if (queue_excess_) {
        const auto before = *queue_excess_;
        const auto slack = std::max<duration>(before / 4, queue_flat);
        if (*excess + slack < before) {
          // Draining, only slowly: the cut works, wait for it.
          queue_excess_ = *excess;
          queue_since_.reset();
          return false;
        }
        const bool flat = (*excess > before ? *excess - before : before - *excess) <= slack;
        if (flat || queue_steps_ >= queue_steps_max) {
          // A slower route, or a flow that keeps the queue full whatever the
          // stream sends. Only the queue's own floor is re-learned.
          rtt_floor_.clear();
          rtt_floor_.emplace_back(now, rtt_);
          raised_at_.reset();
          queue_since_.reset();
          queue_excess_.reset();
          queue_steps_ = 0;
          return false;
        }
      }
      return true;
    }

    std::uint32_t encoder_kbps(int fec, int framerate) {
      if (cache_ && cache_->current == current_ && cache_->fec == fec && cache_->framerate == framerate) {
        return cache_->kbps;
      }
      auto kbps = std::clamp(budget_->encoder_kbps(current_, ceiling_kbps_, fec, framerate), floor_kbps_, ceiling_kbps_);
      // Less FEC frees wire bits for video, but only a probe may spend them.
      if (cache_ && cache_->current == current_ && cache_->framerate == framerate && fec < cache_->fec) {
        kbps = std::min(kbps, cache_->kbps);
      }
      cache_ = cache_t {current_, fec, framerate, kbps};
      return kbps;
    }

    std::uint32_t kbps_for(std::uint64_t total, int fec, int framerate) const {
      return std::clamp(budget_->encoder_kbps(total, ceiling_kbps_, fec, framerate), floor_kbps_, ceiling_kbps_);
    }

    /// Rough encoder kbps for a wire rate at the last FEC seen, for the snapshot.
    std::uint32_t kbps_of(std::uint64_t wire) const {
      if (!cache_ || !budget_) {
        return 0;
      }
      return budget_->encoder_kbps(wire, ceiling_kbps_, cache_->fec, cache_->framerate);
    }

    std::optional<trigger_t> triggered() const {
      const evidence_t evidence {
        events_.unrecoverable.size(),
        events_.send_losses.size(),
        events_.hot,
        0.,
      };
      const char *reason;
      if (evidence.unrecoverable_frames >= unrecoverable_frames) {
        reason = "unrecoverable_frames";
      } else if (evidence.loss_windows >= loss_windows) {
        reason = "fec_loss_share";
      } else if (evidence.send_losses >= send_loss_frames) {
        reason = "send_losses";
      } else {
        return std::nullopt;
      }
      return trigger_t {reason, evidence};
    }

    /// Frames sent since the fence and since the first frame with loss
    /// evidence, within the evidence window: the video wire rate the host
    /// offered, and the share of its packets the client reports missing.
    std::optional<std::pair<std::uint64_t, double>> delivered(time_point now) const {
      std::size_t frames = 0;
      std::optional<time_point> first;
      std::optional<time_point> last;
      std::uint64_t bytes = 0;
      std::uint64_t first_bytes = 0;
      std::uint64_t packets = 0;
      std::uint64_t missing = 0;
      for (const auto &s : ring_) {
        if (!s.at) {
          continue;
        }
        const auto at = *s.at;
        if (since(now, at) >= evidence_window || (fence_ && !fence_->admits(s.frame, at)) ||
            (events_.first_loss && std::uint32_t(s.frame - *events_.first_loss) >= half_range) ||
            (events_.last_loss && std::uint32_t(*events_.last_loss - s.frame) >= half_range)) {
          continue;
        }
        ++frames;
        bytes += s.bytes;
        packets += s.packets;
        missing += s.missing;
        if (!first || at < *first) {
          first = at;
          first_bytes = s.bytes;
        }
        if (!last || at > *last) {
          last = at;
        }
      }
      if (!first || !last) {
        return std::nullopt;
      }
      const auto span = seconds(since(*last, *first));
      if (frames < 2 || span <= 0. || packets == 0) {
        return std::nullopt;
      }
      const double packet_share = std::min(double(missing) / double(packets), 1.);
      const double frame_share = std::min(double(events_.unrecoverable.size() + events_.send_losses.size()) / double(frames), 1.);
      return std::pair {
        std::uint64_t(double(bytes - first_bytes) * 8. / span),
        std::min(std::max(packet_share, frame_share), max_loss_share),
      };
    }

    std::uint64_t floor_total(int fec, int framerate) const {
      return budget_->total_bps(floor_kbps_, fec, framerate);
    }

    /// The lowest total a cut may ask for: the minimum bitrate, or the rate
    /// the encoder was seen to send if that is higher.
    std::uint64_t cut_floor(int fec, int framerate) const {
      return std::max(floor_total(fec, framerate), encoder_floor_.value_or(0));
    }

    std::uint64_t ceiling_total(int fec, int framerate) const {
      return budget_->total_bps(ceiling_kbps_, fec, framerate);
    }

    void record(time_point now, const change_t &change) {
      last_change_ = now;
      change_ = change;
    }

    std::uint32_t delivered_kbps() const {
      return std::uint32_t(std::min<std::uint64_t>(delivered_bps_ / 1000, std::numeric_limits<std::uint32_t>::max()));
    }

    /// From `full`, the controller starts from the bitrate the encoder runs at.
    std::uint32_t leave_full(int fec, int framerate, std::uint32_t current_kbps) {
      if (state_ != state_e::full) {
        return encoder_kbps(fec, framerate);
      }
      const auto from_kbps = std::min(current_kbps, ceiling_kbps_);
      current_ = std::min(budget_->total_bps(std::max<std::uint32_t>(from_kbps, 1), fec, framerate), ceiling_total(fec, framerate));
      good_ = current_;
      return from_kbps;
    }

    void on_evidence(time_point now, const trigger_t &why, int fec, int framerate, std::uint32_t current_kbps) {
      const auto from_kbps = leave_full(fec, framerate, current_kbps);
      // A probe is failing: go back to the last good rate, not below it.
      std::optional<confirmed_t> recent;
      if (confirmed_ && since(now, confirmed_->at) < evidence_window) {
        recent = confirmed_;
      }
      std::optional<std::pair<std::uint64_t, std::uint64_t>> failing;
      if (pending_[0]) {
        failing = std::pair {pending_[0]->rate, good_};
      } else if (recent) {
        failing = std::pair {recent->rate, recent->before};
      }
      if (failing) {
        const auto [failed, before] = *failing;
        pending_ = {};
        confirmed_.reset();
        const bool knee = bad_ && failed * 100 >= *bad_ * (100 - knee_percent) && failed * 100 <= *bad_ * (100 + knee_percent);
        bad_ = knee ? std::min(*bad_, failed) : failed;
        // A second failure at the knee goes back to under it, not to a rate a
        // step above the first.
        auto back_to = before;
        if (knee) {
          back_to = std::max(std::min(back_to, (*bad_ * knee_cap_percent + 99) / 100), encoder_floor_.value_or(0));
        }
        good_ = back_to;
        current_ = back_to;
        hold_ = std::min(hold_ * 2, max_hold);
        next_probe_ = now + hold_;
        knee_failed(now, knee);
        long_knee(now);
        ++failed_probes_;
        at_ceiling_.reset();
        near_bad_.reset();
        set_fence(now);
        record(now, change_t {change_kind_e::probe_failed, why.reason, from_kbps, encoder_kbps(fec, framerate), delivered_kbps(), why.evidence});
        return;
      }
      const auto cur = current_;
      const auto estimate = delivered(now);
      // A cut was made and the frames sent since still run well above what it
      // asked for, twice running: the encoder has a minimum, and cutting below
      // it only lowers a target nothing follows. Two rounds, because a
      // keyframe among the few frames of one round looks the same.
      std::optional<std::pair<std::uint64_t, std::uint64_t>> overshoot;
      if (estimate && state_ != state_e::full) {
        const auto offered = estimate->first + budget_->audio_bps;
        if (offered * 100 > cur * encoder_overshoot_percent) {
          overshoot = std::pair {estimate->first, offered};
        }
      }
      if (!overshoot) {
        overshoots_ = 0;
        cut(now, why, fec, framerate, from_kbps, estimate);
        return;
      }
      if (++overshoots_ < 2) {
        cut(now, why, fec, framerate, from_kbps, estimate);
        return;
      }
      overshoots_ = 0;
      const auto [sent, offered] = *overshoot;
      const auto observed = std::min(offered, ceiling_total(fec, framerate));
      encoder_floor_ = observed;
      current_ = std::max(observed, cur);
      good_ = current_;
      confirmed_.reset();
      pending_ = {};
      delivered_bps_ = sent;
      at_ceiling_.reset();
      near_bad_.reset();
      reason_ = "encoder_floor";
      next_probe_ = now + std::max(hold_, fence_wait());
      set_fence(now);
      record(now, change_t {change_kind_e::encoder_floor, "encoder_floor", from_kbps, encoder_kbps(fec, framerate), std::uint32_t(sent / 1000), why.evidence});
    }

    /// Cut the rate toward what the link delivered, or hold at the floor.
    void cut(time_point now, const trigger_t &why, int fec, int framerate, std::uint32_t from_kbps, std::optional<std::pair<std::uint64_t, double>> estimate) {
      const auto cur = current_;
      auto evidence = why.evidence;
      const auto floor = std::min(cut_floor(fec, framerate), cur);
      if (cur <= floor) {
        // Nothing left to cut. At the encoder's own minimum the stream is
        // already as low as it goes and has said so.
        if (estimate) {
          evidence.loss_share = estimate->second;
          delivered_bps_ = std::uint64_t(double(estimate->first) * (1. - estimate->second));
        }
        if (state_ != state_e::floor && !encoder_floor_) {
          state_ = state_e::floor;
          reason_ = "floor";
          record(now, change_t {change_kind_e::floor, why.reason, from_kbps, from_kbps, delivered_kbps(), evidence});
        }
        set_fence(now);
        return;
      }
      // The first cut of an episode goes down by at most half; one right after
      // it means the estimate was too high, so it may go to the delivered rate.
      const bool repeat = last_cut_ && since(now, *last_cut_) < evidence_window;
      const auto lowest = repeat ? repeat_cut_floor_bps : cur * min_cut_percent / 100;
      // A queue standing at a rate the stream climbed to after an earlier cut:
      // the link edge is just under it, not far below.
      const bool climbing = state_ == state_e::limited && bad_ && rtt_elevated();
      std::uint64_t target;
      if (estimate) {
        evidence.loss_share = estimate->second;
        const auto delivered = std::uint64_t(double(estimate->first) * (1. - estimate->second));
        delivered_bps_ = delivered;
        target = (delivered + budget_->audio_bps) * delivered_percent / 100;
      } else {
        delivered_bps_ = 0;
        target = cur * (climbing ? knee_backoff_percent : unknown_share_percent) / 100;
      }
      const auto most = cur * max_cut_percent / 100;
      target = std::max(std::clamp(target, std::min(lowest, most), most), floor);
      const bool knee = bad_ && cur * 100 >= *bad_ * knee_cut_percent;
      bad_ = bad_ ? std::min(*bad_, cur) : cur;
      confirmed_.reset();
      current_ = target;
      good_ = target;
      // A cut from the knee is a failed probe that confirmed first.
      hold_ = knee ? std::min(hold_ * 2, max_hold) : hold;
      next_probe_ = now + std::max(hold_, fence_wait());
      knee_failed(now, knee);
      if (rtt_elevated()) {
        long_knee(now);
      }
      at_ceiling_.reset();
      near_bad_.reset();
      ++steps_down_;
      last_cut_ = now;
      state_ = state_e::limited;
      reason_ = why.reason;
      set_fence(now);
      record(now, change_t {change_kind_e::step_down, why.reason, from_kbps, encoder_kbps(fec, framerate), delivered_kbps(), evidence});
    }

    /// Step down 12% for a standing queue, or back to the last good rate when
    /// a probe is the one that raised it, and keep probes under it for the
    /// quiet hold.
    void queue_cut(time_point now, int fec, int framerate, std::uint32_t current_kbps) {
      const auto from_kbps = leave_full(fec, framerate, current_kbps);
      const auto cur = current_;
      const auto floor = std::min(cut_floor(fec, framerate), cur);
      const auto low = rtt_floor();
      const auto excess = low && rtt_ > *low ? rtt_ - *low : duration::zero();
      queue_since_.reset();
      queue_quiet_until_ = now + queue_settle;
      if (cur <= floor) {
        // Nothing to cut; a delay alone never makes a floor.
        queue_excess_.reset();
        return;
      }
      queue_excess_ = excess;
      ++queue_steps_;
      // A probe that raised the rate into this queue is the failed one.
      std::optional<std::uint64_t> back_to;
      if (pending_[0]) {
        back_to = good_;
      } else if (confirmed_ && since(now, confirmed_->at) < evidence_window) {
        back_to = confirmed_->before;
      }
      pending_ = {};
      confirmed_.reset();
      if (back_to) {
        ++failed_probes_;
      }
      auto target = cur * queue_cut_percent / 100;
      if (back_to) {
        target = std::min(target, *back_to);
      }
      target = std::max(target, floor);
      const bool knee = bad_ && cur * 100 >= *bad_ * knee_cut_percent;
      bad_ = bad_ ? std::min(*bad_, cur) : cur;
      current_ = target;
      good_ = target;
      hold_ = knee ? std::min(hold_ * 2, max_hold) : hold;
      next_probe_ = now + std::max(hold_, fence_wait());
      knee_failed(now, knee);
      long_knee(now);
      at_ceiling_.reset();
      near_bad_.reset();
      if (!back_to) {
        ++steps_down_;
      }
      last_cut_ = now;
      state_ = state_e::limited;
      reason_ = "queue";
      set_fence(now);
      record(now, change_t {back_to ? change_kind_e::probe_failed : change_kind_e::step_down, "queue", from_kbps, encoder_kbps(fec, framerate), delivered_kbps(), evidence_t {}});
    }

    /// A failure at the bad rate (`knee`) doubles the wait that keeps probes
    /// under it; any other failure leaves the bad rate somewhere new.
    void knee_failed(time_point now, bool knee) {
      if (knee) {
        knee_ = std::min(knee_ * 2, knee_max_hold);
        knee_until_ = now + knee_;
      } else {
        knee_ = hold;
        knee_until_.reset();
      }
    }

    /// A probe that failed, or a cut with a queue standing in the path, found
    /// the link's edge: keep probes under it for a long quiet hold.
    void long_knee(time_point now) {
      knee_ = std::max(knee_, knee_quiet);
      knee_until_ = now + knee_;
    }

    void forget_knee() {
      knee_ = hold;
      knee_until_.reset();
    }

    bool unconfirmed() const {
      return pending_[0].has_value();
    }

    /// No evidence this round: confirm earlier probe steps, then probe or
    /// return to `full`.
    void advance(time_point now, bool busy, int fec, int framerate) {
      if (!busy) {
        // A static picture proves nothing: a step sent just before it needs
        // its two seconds of motion.
        for (auto &step : pending_) {
          if (step) {
            step->at = now;
          }
        }
      }
      for (auto &slot : pending_) {
        if (slot && since(now, slot->at) >= confirm_after) {
          if (slot->rate > good_) {
            // A failure now goes back to the rate before this step, not to
            // where the climb began.
            confirmed_ = confirmed_t {good_, slot->rate, now};
          }
          good_ = std::max(good_, slot->rate);
          hold_ = hold;
          if (!bad_ || slot->rate > *bad_) {
            bad_.reset();
            knee_ = hold;
            knee_until_.reset();
          }
          slot.reset();
        }
      }
      (void) std::stable_partition(pending_.begin(), pending_.end(), [](const auto &step) {
        return step.has_value();
      });
      // Quiet video, stale pings or a queue in the path prove nothing.
      if (!(busy && ping_is_fresh(now) && rtt_ > duration::zero() && !rtt_elevated())) {
        return;
      }
      const auto ceiling = ceiling_total(fec, framerate);
      const bool pending = unconfirmed();
      if (current_ >= ceiling) {
        if (!at_ceiling_) {
          at_ceiling_ = now;
        }
        if (!pending && since(now, *at_ceiling_) >= full_after) {
          finish(now, fec, framerate);
        }
        return;
      }
      if (bad_) {
        if (!near_bad_) {
          if (current_ * 100 >= *bad_ * 95) {
            near_bad_ = now;
          }
        } else if (current_ * 100 < *bad_ * 95) {
          near_bad_.reset();
        } else if (since(now, *near_bad_) >= forget_bad_after && !pending) {
          bad_.reset();
          near_bad_.reset();
          forget_knee();
        }
      }
      if ((next_probe_ && now < *next_probe_) || pending_[pending_cap - 1]) {
        return;
      }
      auto divisor = probe_step_divisor;
      const auto wait = std::max(probe_every, fence_wait());
      if (bad_ && (current_ + current_ / probe_step_divisor) * 100 >= *bad_ * near_bad_percent) {
        if (pending) {
          return;
        }
        divisor = near_step_divisor;
      }
      // The packet count per frame quantizes the encoder bitrate; a step that
      // changes nothing is no probe.
      const auto step = std::max<std::uint64_t>(current_ / divisor, 1);
      const auto level = kbps_for(current_, fec, framerate);
      // On a known knee, probes stop just under it until the wait is over.
      auto limit = ceiling;
      if (bad_ && knee_until_ && now < *knee_until_) {
        limit = std::min(ceiling, (*bad_ * knee_cap_percent + 99) / 100);
      }
      auto next = current_;
      while (next < limit && kbps_for(next, fec, framerate) <= level) {
        next += step;
      }
      next = std::min(next, limit);
      if (next <= current_ || (limit < ceiling && kbps_for(next, fec, framerate) <= level)) {
        return;
      }
      const auto from_kbps = encoder_kbps(fec, framerate);
      current_ = next;
      next_probe_ = now + wait;
      for (auto &slot : pending_) {
        if (!slot) {
          slot = step_t {next, now};
          break;
        }
      }
      ++probes_;
      raised_at_ = now;
      if (state_ == state_e::floor) {
        state_ = state_e::limited;
      }
      record(now, change_t {change_kind_e::probe, "probe", from_kbps, encoder_kbps(fec, framerate), delivered_kbps(), evidence_t {}});
    }

    void finish(time_point now, int fec, int framerate) {
      const auto kbps = encoder_kbps(fec, framerate);
      state_ = state_e::full;
      reason_ = "healthy";
      bad_.reset();
      encoder_floor_.reset();
      overshoots_ = 0;
      hold_ = hold;
      forget_knee();
      at_ceiling_.reset();
      near_bad_.reset();
      pending_ = {};
      confirmed_.reset();
      record(now, change_t {change_kind_e::full, "recovered", kbps, kbps, 0, evidence_t {}});
    }

    std::optional<wire_budget_t> budget_;
    const char *reason_ {"disabled"};
    state_e state_ {state_e::off};
    std::uint32_t ceiling_kbps_ {};
    std::uint32_t floor_kbps_ {};
    /// Total wire bits per second the encoder is held to.
    std::uint64_t current_ {};
    /// The last rate that stayed clean.
    std::uint64_t good_ {};
    /// The last rate that did not.
    std::optional<std::uint64_t> bad_;
    /// Total wire bits per second the encoder was seen to send after a cut
    /// asked for less; cuts stop here.
    std::optional<std::uint64_t> encoder_floor_;
    /// Evidence rounds in a row whose frames ran above the target.
    std::uint8_t overshoots_ {};
    /// Probe steps above `good_` not yet confirmed clean, oldest first.
    std::array<std::optional<step_t>, pending_cap> pending_ {};
    std::optional<confirmed_t> confirmed_;
    duration hold_ {hold};
    /// Knee wait, doubled by failures at the bad rate, reset past it.
    duration knee_ {hold};
    /// Probes stay under knee_cap_percent of `bad_` until then.
    std::optional<time_point> knee_until_;
    std::optional<time_point> next_probe_;
    std::optional<time_point> at_ceiling_;
    std::optional<time_point> near_bad_;
    std::optional<fence_t> fence_;
    /// Rebuilt blocks seen, their packets and those of them reported missing
    /// before the block was rebuilt (data only).
    std::uint64_t rebuilt_blocks_ {};
    std::uint64_t rebuilt_packets_ {};
    std::uint64_t rebuilt_data_missing_ {};
    /// Running share of a rebuilt block's data packets reported missing while
    /// the RTT is flat (what reordering looks like on this path), and the
    /// excess over it seen while the RTT is up, in packets.
    double reorder_share_ {};
    std::optional<time_point> reorder_at_;
    std::int64_t excess_credit_ {};
    std::int64_t excess_high_ {};
    std::optional<std::uint32_t> last_sent_;
    std::vector<sent_t> ring_;
    events_t events_;
    std::optional<time_point> last_ping_;
    std::optional<duration> minimum_rtt_;
    /// Lowest RTT per second over queue_floor_window, oldest first.
    std::deque<std::pair<time_point, duration>> rtt_floor_;
    /// Since when every RTT sample was over the floor by queue_margin.
    std::optional<time_point> queue_since_;
    /// A probe raised the rate at this time.
    std::optional<time_point> raised_at_;
    std::optional<time_point> queue_quiet_until_;
    /// The excess over the floor when the last queue cut was made, and the
    /// cuts of this queue so far.
    std::optional<duration> queue_excess_;
    std::uint8_t queue_steps_ {};
    duration rtt_ {};
    std::optional<time_point> next_eval_;
    std::optional<cache_t> cache_;
    std::uint64_t delivered_bps_ {};
    std::optional<time_point> last_change_;
    std::optional<change_t> change_;
    std::uint32_t steps_down_ {};
    std::optional<time_point> last_cut_;
    std::uint32_t probes_ {};
    std::uint32_t failed_probes_ {};
  };

  /// Whether video is busy: over the last sample (at least 500 ms, at most
  /// 2 s old) at least 60% of the frames and bytes the stream should send. A
  /// static desktop sends almost nothing, so a probe sent then proves nothing.
  class busy_meter_t {
  public:
    bool update(time_point now, std::uint64_t frames, std::uint64_t bytes, int framerate, std::uint32_t kbps) {
      if (!sample_) {
        sample_ = sample_t {now, frames, bytes};
        return false;
      }
      const auto elapsed = now > sample_->at ? now - sample_->at : duration::zero();
      if (elapsed < std::chrono::milliseconds {500}) {
        return busy_;
      }
      const auto before = *sample_;
      sample_ = sample_t {now, frames, bytes};
      const auto secs = std::chrono::duration<double>(elapsed).count();
      busy_ = elapsed <= std::chrono::seconds {2} &&
              double(frames > before.frames ? frames - before.frames : 0) >= secs * double(std::max(framerate, 1)) * 0.6 &&
              double(bytes > before.bytes ? bytes - before.bytes : 0) * 8. >= double(kbps) * 1000. * secs * 0.6;
      return busy_;
    }

  private:
    struct sample_t {
      time_point at;
      std::uint64_t frames;
      std::uint64_t bytes;
    };

    std::optional<sample_t> sample_;
    bool busy_ {};
  };
}  // namespace stream::link_rate
