/**
 * @file tests/unit/test_video_link_rate.cpp
 * @brief Test src/video_link_rate.h
 *
 * Ported from Rubylight's link_rate/tests.rs and link_rate/sim.rs, without the
 * keyframe-request and pacing-latch cases this port leaves out. The rig is a
 * 60 fps host ticking every 8 ms with a client ping every 100 ms; the
 * simulation is a token-bucket link with a drop-tail queue and client FEC
 * reports that arrive after a delay.
 */

#include "../tests_common.h"
#include "src/video_link_rate.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace stream::link_rate {
  /// Test-only view of the controller's private state.
  struct link_rate_access_t {
    static std::uint64_t &good(link_rate_t &lr) {
      return lr.good_;
    }

    static std::uint64_t &current(link_rate_t &lr) {
      return lr.current_;
    }

    static std::optional<std::uint64_t> &bad(link_rate_t &lr) {
      return lr.bad_;
    }

    static duration &hold(link_rate_t &lr) {
      return lr.hold_;
    }

    static duration &knee(link_rate_t &lr) {
      return lr.knee_;
    }

    static std::optional<time_point> &knee_until(link_rate_t &lr) {
      return lr.knee_until_;
    }

    static std::optional<time_point> &next_probe(link_rate_t &lr) {
      return lr.next_probe_;
    }

    static std::optional<time_point> &last_cut(link_rate_t &lr) {
      return lr.last_cut_;
    }

    static bool has_pending(const link_rate_t &lr) {
      return std::any_of(lr.pending_.begin(), lr.pending_.end(), [](const auto &step) {
        return step.has_value();
      });
    }

    static std::optional<std::uint64_t> confirmed_before(const link_rate_t &lr) {
      if (!lr.confirmed_) {
        return std::nullopt;
      }
      return lr.confirmed_->before;
    }

    static std::uint64_t missing(const link_rate_t &lr) {
      return lr.events_.missing;
    }

    static std::optional<std::uint32_t> sent_missing(link_rate_t &lr, std::uint32_t frame) {
      const auto *sent = lr.find_sent(frame);
      if (!sent) {
        return std::nullopt;
      }
      return sent->missing;
    }

    static std::uint32_t kbps_for(const link_rate_t &lr, std::uint64_t total, int fec, int framerate) {
      return lr.kbps_for(total, fec, framerate);
    }
  };
}  // namespace stream::link_rate

namespace {
  using namespace stream::link_rate;
  using namespace std::chrono_literals;
  // Hides ::clock from <ctime>.
  using stream::link_rate::clock;
  using access = link_rate_access_t;
  using fec_status_t = stream::pacing::fec_status_t;
  using ms_t = std::chrono::milliseconds;

  constexpr std::uint64_t period_ms = 17;
  constexpr std::uint32_t frame_packets = 100;
  constexpr std::uint64_t frame_wire_bytes = 112'600;
  constexpr int framerate = 60;
  constexpr int fec = 20;

  const std::string ping_payload {"\x04\0\0\0\0\0\0\0", 8};

  /// 1024-byte packets (1008 bytes of video each), IPv6 framing, stereo audio.
  wire_budget_t budget() {
    return {8, 1008, 1024 + 16 + 86, 0, wire_budget_t::audio_reserve_bps(2, false)};
  }

  link_rate_t make(std::uint32_t kbps, bool enabled = true, bool pyrowave = false) {
    return link_rate_t {enabled, pyrowave, kbps, budget()};
  }

  fec_status_t status(std::uint32_t frame, std::uint16_t received, std::uint16_t parity_lost) {
    fec_status_t s;
    s.frame = frame;
    s.data = 80;
    s.parity = 16;
    s.received_data = received;
    s.received_parity = std::uint16_t(16 - parity_lost);
    s.percentage = 20;
    s.block = 0;
    s.blocks = 1;
    return s;
  }

  /// A frame the client could not rebuild.
  void unrecoverable(link_rate_t &lr, std::uint32_t frame, time_point now) {
    lr.on_status(status(frame, 60, 0), now);
    lr.on_unrecoverable(frame, now);
  }

  /// A frame that lost `missing` parity packets, all of them repaired by FEC.
  void repaired(link_rate_t &lr, std::uint32_t frame, std::uint16_t missing, time_point now) {
    lr.on_status(status(frame, 80, missing), now);
  }

  /// A frame that lost `missing` data packets that FEC rebuilt.
  void rebuilt_data(link_rate_t &lr, std::uint32_t frame, std::uint16_t missing, time_point now) {
    lr.on_status(status(frame, std::uint16_t(80 - missing), 0), now);
  }

  bool same(const change_t &a, const change_t &b) {
    return a.kind == b.kind && std::string_view {a.reason} == b.reason && a.from_kbps == b.from_kbps &&
           a.to_kbps == b.to_kbps && a.delivered_kbps == b.delivered_kbps && a.evidence == b.evidence;
  }

  std::string describe(const std::vector<std::pair<std::uint64_t, change_t>> &changes) {
    std::string out;
    for (const auto &[at, c] : changes) {
      out += std::to_string(at) + "ms " + change_kind_name(c.kind) + '(' + c.reason + ") " +
             std::to_string(c.from_kbps) + "->" + std::to_string(c.to_kbps) + "; ";
    }
    return out;
  }

  /// A 60 fps host: one frame every 17 ms, a tick every 8 ms and a ping every
  /// 100 ms, one millisecond at a time.
  struct rig_t {
    link_rate_t lr;
    time_point base {clock::now()};
    std::uint64_t ms {};
    std::uint32_t frame {};
    std::uint32_t kbps;
    ms_t rtt {20};
    /// Up to this many milliseconds are added to `rtt` at every tick.
    std::uint64_t rtt_jitter_ms {};
    /// False while the picture is static.
    bool busy {true};
    std::vector<std::pair<std::uint64_t, change_t>> changes;

    explicit rig_t(std::uint32_t kbps):
        rig_t(make(kbps), kbps) {
    }

    rig_t(link_rate_t controller, std::uint32_t kbps):
        lr {std::move(controller)},
        kbps {kbps} {
    }

    time_point now() const {
      return base + ms_t(ms);
    }

    /// Runs `duration_ms` milliseconds; `each` sees every frame right after it is sent.
    template<class Each>
    void run(std::uint64_t duration_ms, Each &&each) {
      for (std::uint64_t i = 0; i < duration_ms; ++i) {
        ++ms;
        const auto t = now();
        if (ms % period_ms == 0) {
          ++frame;
          // The encoder follows its bitrate, so the frames shrink with it.
          const auto bytes = frame_wire_bytes * kbps / 40'000;
          const auto packets = std::max<std::uint64_t>(std::uint64_t(frame_packets) * kbps / 40'000, 1);
          lr.on_sent(frame, std::uint32_t(packets), bytes, t);
          each(lr, frame, t);
        }
        if (ms % 100 == 0) {
          lr.on_ping(t, ping_payload);
        }
        if (ms % 8 == 0) {
          const auto jitter = ms_t((ms * 2'654'435'761ull) % (rtt_jitter_ms + 1));
          if (const auto next = lr.tick(t, rtt + jitter, fec, framerate, kbps, busy)) {
            kbps = *next;
          }
        }
        if (auto change = lr.take_change()) {
          changes.emplace_back(ms, *change);
        }
      }
    }

    void clean(std::uint64_t duration_ms) {
      run(duration_ms, [](link_rate_t &, std::uint32_t, time_point) {});
    }

    /// Three unrecoverable frames in a row.
    void lose_three() {
      int left = 3;
      run(period_ms * 3 + 1, [&](link_rate_t &l, std::uint32_t f, time_point t) {
        if (left > 0) {
          --left;
          unrecoverable(l, f, t);
        }
      });
    }

    std::size_t count(change_kind_e kind) const {
      return std::size_t(std::count_if(changes.begin(), changes.end(), [&](const auto &c) {
        return c.second.kind == kind;
      }));
    }

    std::size_t cuts() const {
      return count(change_kind_e::step_down);
    }

    std::size_t probes() const {
      return count(change_kind_e::probe);
    }

    bool has(change_kind_e kind) const {
      return count(kind) > 0;
    }

    std::optional<change_t> last(change_kind_e kind) const {
      for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
        if (it->second.kind == kind) {
          return it->second;
        }
      }
      return std::nullopt;
    }

    /// Runs until one more probe was issued; returns it.
    change_t until_probe() {
      const auto seen = probes();
      for (int i = 0; i < 6'000; ++i) {
        clean(10);
        if (probes() > seen) {
          return *last(change_kind_e::probe);
        }
      }
      ADD_FAILURE() << "no probe in 60 s: " << describe(changes);
      return {};
    }
  };

  // ---- the rig ----

  TEST(VideoLinkRate, AHealthyLinkGetsNoTargetAndNoChangeInAMinute) {
    rig_t rig {40'000};
    // Recovered loss well under the threshold, a packet or two per frame, is
    // what a good Wi-Fi link looks like.
    rig.run(60'000, [](link_rate_t &lr, std::uint32_t frame, time_point now) {
      if (frame % 7 == 0) {
        repaired(lr, frame, 2, now);
      }
    });
    EXPECT_TRUE(rig.changes.empty()) << describe(rig.changes);
    EXPECT_EQ(rig.kbps, 40'000u);
    EXPECT_EQ(rig.lr.state(), state_e::full);
    EXPECT_FALSE(rig.lr.tick(rig.now(), rig.rtt, fec, framerate, 40'000, true));
  }

  TEST(VideoLinkRate, ThreeUnrecoverableFramesCutOnceToBetweenHalfAnd85Percent) {
    rig_t rig {40'000};
    rig.clean(2'000);
    rig.lose_three();
    rig.clean(500);
    ASSERT_EQ(rig.cuts(), 1u) << describe(rig.changes);
    const auto cut = rig.changes[0].second;
    EXPECT_STREQ(cut.reason, "unrecoverable_frames");
    EXPECT_EQ(cut.from_kbps, 40'000u);
    EXPECT_LT(cut.to_kbps, 40'000u * 85 / 100);
    EXPECT_GE(cut.to_kbps, 40'000u * 40 / 100);
    EXPECT_EQ(rig.kbps, cut.to_kbps);
    EXPECT_EQ(rig.lr.state(), state_e::limited);
    EXPECT_EQ(cut.evidence.unrecoverable_frames, 3u);
  }

  TEST(VideoLinkRate, AReportBeforeTheFrameIsRecordedCountsNothingAndOneAfterDoes) {
    const auto now = clock::now();
    auto lr = make(40'000);
    const auto early = status(7, 5, 0);
    lr.on_status(early, now);
    EXPECT_EQ(access::missing(lr), 0u);
    lr.on_sent(7, frame_packets, frame_wire_bytes, now);
    lr.on_status(early, now);
    EXPECT_GT(access::missing(lr), 0u);
    EXPECT_EQ(access::sent_missing(lr, 7), std::optional<std::uint32_t> {std::uint32_t(access::missing(lr))});
  }

  TEST(VideoLinkRate, TwoUnrecoverableFramesOrThreeSpreadOverTimeDoNotCut) {
    rig_t rig {40'000};
    int left = 2;
    rig.run(1'000, [&](link_rate_t &lr, std::uint32_t frame, time_point now) {
      if (left > 0) {
        --left;
        unrecoverable(lr, frame, now);
      }
    });
    rig.clean(5'000);
    // Three, but one every 2.5 s.
    for (int i = 0; i < 3; ++i) {
      bool once = true;
      rig.run(2'500, [&](link_rate_t &lr, std::uint32_t frame, time_point now) {
        if (std::exchange(once, false)) {
          unrecoverable(lr, frame, now);
        }
      });
    }
    EXPECT_TRUE(rig.changes.empty()) << describe(rig.changes);
  }

  TEST(VideoLinkRate, TheSameFrameReportedInSeveralBlocksCountsOnce) {
    rig_t rig {40'000};
    rig.run(500, [](link_rate_t &lr, std::uint32_t frame, time_point now) {
      for (int i = 0; i < 5; ++i) {
        unrecoverable(lr, std::min<std::uint32_t>(frame, 1), now);
      }
    });
    EXPECT_TRUE(rig.changes.empty()) << describe(rig.changes);
  }

  TEST(VideoLinkRate, RecoveredLossNeverCutsHoweverManyPacketsItLacked) {
    // A block is reported the moment `data` packets are in, so the parity still
    // in flight reads as missing; FEC rebuilt every one of these frames.
    for (std::uint16_t lost_parity : {4, 8, 16}) {
      rig_t rig {40'000};
      rig.run(20'000, [&](link_rate_t &lr, std::uint32_t frame, time_point now) {
        repaired(lr, frame, lost_parity, now);
      });
      EXPECT_TRUE(rig.changes.empty()) << lost_parity << ": " << describe(rig.changes);
      EXPECT_EQ(rig.kbps, 40'000u);
    }
  }

  TEST(VideoLinkRate, AnOccasionalFailedFrameIsNotACut) {
    // One frame in sixty fails beyond repair: under three in two seconds, and
    // under 5% of the packets sent.
    rig_t rig {40'000};
    int n = 0;
    rig.run(20'000, [&](link_rate_t &lr, std::uint32_t frame, time_point now) {
      if (++n % 60 == 0) {
        unrecoverable(lr, frame, now);
      }
    });
    EXPECT_TRUE(rig.changes.empty()) << describe(rig.changes);
  }

  TEST(VideoLinkRate, RebuiltDataLossCutsOnlyWhileTheRttIsElevated) {
    struct case_t {
      int rtt;
      std::uint16_t missing;
      bool cut;
    };

    for (const auto &c : {
           case_t {20, 10, false},
           case_t {100, 10, true},
           // 4 of 80 data packets is 5% of the block's 96, not 4% of the 100 sent.
           case_t {100, 4, true},
           case_t {100, 1, false},
           case_t {100, 0, false},
         }) {
      rig_t rig {40'000};
      rig.clean(3'000);
      rig.rtt = ms_t(c.rtt);
      rig.run(5'000, [&](link_rate_t &lr, std::uint32_t frame, time_point now) {
        rebuilt_data(lr, frame, c.missing, now);
      });
      EXPECT_EQ(rig.cuts() > 0, c.cut) << "rtt " << c.rtt << ", " << c.missing << ": " << describe(rig.changes);
      if (c.cut && !rig.changes.empty()) {
        EXPECT_STREQ(rig.changes[0].second.reason, "fec_loss_share");
      }
    }
  }

  /// One block of a Wi-Fi path with reordering as the client reports it: 32 data
  /// packets plus `parity_shards` in a random arrival order, `loss_permille` of
  /// the packets lost, a report at the 32nd arrival unless all 32 data packets
  /// came first. `queued` more data packets than that were still behind a queue
  /// when parity made up for them. Returns whether the block was reported.
  bool wifi_block(std::uint64_t &seed, link_rate_t &lr, std::uint32_t frame, std::uint16_t parity_shards, std::uint64_t loss_permille, std::uint16_t queued, time_point now) {
    const auto random = [&]() {
      seed ^= seed << 13;
      seed ^= seed >> 7;
      seed ^= seed << 17;
      return seed;
    };
    std::vector<std::uint16_t> order(std::size_t(32 + parity_shards));
    std::iota(order.begin(), order.end(), std::uint16_t {0});
    for (std::size_t i = order.size() - 1; i >= 1; --i) {
      std::swap(order[i], order[std::size_t(random() % (i + 1))]);
    }
    std::uint16_t data = 0;
    std::uint16_t parity = 0;
    for (const auto index : order) {
      if (random() % 1000 < loss_permille) {
        continue;
      }
      if (index < 32) {
        ++data;
      } else {
        ++parity;
      }
      if (data + parity == 32) {
        break;
      }
    }
    // A block whose data all came first needs no report.
    if (data == 32) {
      return false;
    }
    fec_status_t s;
    s.frame = frame;
    s.highest_received = std::uint16_t(31 + parity_shards);
    s.data = 32;
    s.parity = parity_shards;
    s.received_data = data > queued ? std::uint16_t(data - queued) : std::uint16_t {0};
    s.received_parity = std::min<std::uint16_t>(std::uint16_t(parity + queued), parity_shards);
    s.missing_before_highest = std::uint16_t(32 + parity_shards - s.received_data - s.received_parity);
    s.percentage = std::uint8_t(parity_shards * 100 / 32);
    s.block = 0;
    s.blocks = 1;
    lr.on_status(s, now);
    return true;
  }

  TEST(VideoLinkRate, AWifiPathWithReorderingAndRandomLossNeverCuts) {
    rig_t rig {14'988};
    rig.rtt_jitter_ms = 8;
    std::uint64_t seed = 0x9e37'79b9'7f4a'7c15ull;
    std::uint32_t blocks = 0;
    std::uint32_t rebuilt = 0;
    rig.run(45'000, [&](link_rate_t &lr, std::uint32_t frame, time_point now) {
      ++blocks;
      rebuilt += wifi_block(seed, lr, frame, 7, 5, 0, now) ? 1 : 0;
    });
    EXPECT_TRUE(rig.changes.empty()) << describe(rig.changes);
    EXPECT_EQ(rig.kbps, 14'988u);
    // Nearly every block is reported, and the data packets the client had not
    // yet were far over the 5% that cut: counting them as loss would cut.
    EXPECT_GT(rebuilt * 100, blocks * 95) << rebuilt << " of " << blocks;
    EXPECT_GT(rig.lr.snapshot().rebuilt_data_missing_percent, 5.);
  }

  TEST(VideoLinkRate, AShortRttExcursionOnAWifiPathIsNoCutButARealQueueIs) {
    // Data packets behind a queue on top of the reordering, per block, while
    // the RTT is up. At 20 to 50% FEC the data a reordering path has not
    // delivered at report time is up to the parity.
    for (const auto &arms : std::array<std::pair<std::uint16_t, std::uint16_t>, 8> {{{7, 0}, {7, 3}, {10, 0}, {10, 3}, {13, 0}, {13, 3}, {16, 0}, {16, 3}}}) {
      const auto parity = arms.first;
      const auto extra = arms.second;
      rig_t rig {14'988};
      rig.rtt_jitter_ms = 8;
      std::uint64_t seed = 0x9e37'79b9'7f4a'7c15ull;
      const auto arm = [&seed, parity](std::uint16_t queued) {
        return [&seed, parity, queued](link_rate_t &lr, std::uint32_t frame, time_point now) {
          wifi_block(seed, lr, frame, parity, 5, queued, now);
        };
      };
      rig.run(20'000, arm(0));
      EXPECT_TRUE(rig.changes.empty()) << describe(rig.changes);
      rig.rtt = 60ms;
      rig.run(1'500, arm(extra));
      rig.rtt = 20ms;
      rig.run(20'000, arm(0));
      EXPECT_EQ(rig.cuts() > 0, extra > 0) << "parity " << parity << " extra " << extra << ": " << describe(rig.changes);
      if (extra == 0) {
        EXPECT_EQ(rig.kbps, 14'988u);
      }
    }
  }

  TEST(VideoLinkRate, AnOldSpellOfFlatRttLossDoesNotHideAQueueLater) {
    struct case_t {
      std::uint16_t spell;
      std::uint64_t spell_s;
      std::uint64_t quiet_s;
      std::uint16_t queue;
    };

    for (const auto &c : {case_t {0, 0, 0, 8}, case_t {10, 10, 300, 8}}) {
      rig_t rig {40'000};
      rig.run(20'000, [](link_rate_t &lr, std::uint32_t frame, time_point now) {
        rebuilt_data(lr, frame, 2, now);
      });
      if (c.spell > 0) {
        rig.run(c.spell_s * 1'000, [&](link_rate_t &lr, std::uint32_t frame, time_point now) {
          rebuilt_data(lr, frame, c.spell, now);
        });
      }
      rig.clean(c.quiet_s * 1'000);
      EXPECT_TRUE(rig.changes.empty()) << describe(rig.changes);
      rig.rtt = 60ms;
      rig.run(20'000, [&](link_rate_t &lr, std::uint32_t frame, time_point now) {
        rebuilt_data(lr, frame, c.queue, now);
      });
      EXPECT_GT(rig.cuts(), 0u) << "spell " << c.spell << ": " << describe(rig.changes);
    }
  }

  TEST(VideoLinkRate, AFlatRttShareOverWhatReorderingProducesIsNotTheBaseline) {
    // Data 32, parity 16: 14 data packets missing is 0.44, over the 0.35 that
    // reordering can leave, and still rebuilt.
    const auto block = [](link_rate_t &lr, std::uint32_t frame, time_point now) {
      fec_status_t s;
      s.frame = frame;
      s.data = 32;
      s.parity = 16;
      s.received_data = 32 - 14;
      s.received_parity = 16;
      s.percentage = 50;
      s.blocks = 1;
      lr.on_status(s, now);
    };
    rig_t rig {40'000};
    rig.run(30'000, block);
    EXPECT_TRUE(rig.changes.empty()) << describe(rig.changes);
    rig.rtt = 60ms;
    rig.run(10'000, block);
    EXPECT_GT(rig.cuts(), 0u) << describe(rig.changes);
  }

  TEST(VideoLinkRate, NoiseAroundTheReorderingBaselineNeverAddsUp) {
    rig_t rig {40'000};
    rig.run(10'000, [](link_rate_t &lr, std::uint32_t frame, time_point now) {
      rebuilt_data(lr, frame, 8, now);
    });
    rig.rtt = 60ms;
    std::uint32_t n = 0;
    rig.run(20'000, [&](link_rate_t &lr, std::uint32_t frame, time_point now) {
      ++n;
      rebuilt_data(lr, frame, n % 2 == 0 ? 0 : 16, now);
    });
    EXPECT_TRUE(rig.changes.empty()) << describe(rig.changes);
  }

  TEST(VideoLinkRate, AFivePercentBurstInOneWindowDoesNotCut) {
    rig_t rig {40'000};
    rig.clean(1'000);
    // One window of loss, then clean traffic: the second window resets it.
    rig.run(450, [](link_rate_t &lr, std::uint32_t frame, time_point now) {
      repaired(lr, frame, 8, now);
    });
    rig.clean(5'000);
    EXPECT_TRUE(rig.changes.empty()) << describe(rig.changes);
  }

  TEST(VideoLinkRate, AQuietStreamCannotCutOnASingleLostPacket) {
    rig_t rig {40'000};
    // Under loss_min_packets sent per window.
    for (int i = 0; i < 4; ++i) {
      const auto now = rig.now();
      rig.lr.on_sent(rig.frame + 1, 10, 11'000, now);
      ++rig.frame;
      repaired(rig.lr, rig.frame, 6, now);
      rig.clean(500);
    }
    EXPECT_TRUE(rig.changes.empty()) << describe(rig.changes);
  }

  TEST(VideoLinkRate, ThreeFramesLostOnSendCut) {
    rig_t rig {40'000};
    rig.clean(1'000);
    int left = 3;
    rig.run(200, [&](link_rate_t &lr, std::uint32_t frame, time_point now) {
      if (left > 0) {
        --left;
        lr.on_send_loss(frame, now);
      }
    });
    rig.clean(300);
    ASSERT_EQ(rig.cuts(), 1u) << describe(rig.changes);
    EXPECT_STREQ(rig.changes[0].second.reason, "send_losses");
  }

  TEST(VideoLinkRate, ReportsForAFrameTheHostAbandonedAreNotTheNetworks) {
    // The host counts a frame it stopped sending once, on send; the client's
    // reports of its missing packets are not loss on the link as well.
    rig_t rig {40'000};
    rig.clean(1'000);
    rig.run(400, [](link_rate_t &lr, std::uint32_t frame, time_point now) {
      if (frame % 12 == 0) {
        lr.on_send_loss(frame, now);
        unrecoverable(lr, frame, now);
      }
    });
    EXPECT_TRUE(rig.changes.empty()) << describe(rig.changes);
    EXPECT_EQ(access::missing(rig.lr), 0u);
  }

  TEST(VideoLinkRate, ARiseInRttAloneIsNoEvidence) {
    rig_t rig {40'000};
    rig.clean(3'000);
    rig.rtt = 400ms;
    rig.clean(20'000);
    EXPECT_TRUE(rig.changes.empty()) << describe(rig.changes);
    EXPECT_EQ(rig.kbps, 40'000u);
  }

  TEST(VideoLinkRate, ReportsForFramesSentBeforeACutTookEffectAreIgnored) {
    rig_t rig {40'000};
    rig.clean(2'000);
    rig.lose_three();
    rig.clean(150);
    ASSERT_EQ(rig.cuts(), 1u) << describe(rig.changes);
    // Late reports for frames sent at the old rate (the queue is still
    // draining): plenty of them, and none counts.
    const auto last = rig.frame;
    const auto now = rig.now();
    for (auto old = last - 40; old < last - 10; ++old) {
      unrecoverable(rig.lr, old, now);
    }
    rig.clean(1'500);
    EXPECT_EQ(rig.cuts(), 1u) << describe(rig.changes);
    // Frames sent after the cut do count: a second cut follows.
    rig.lose_three();
    rig.clean(500);
    EXPECT_EQ(rig.cuts(), 2u) << describe(rig.changes);
  }

  TEST(VideoLinkRate, ASecondCutRightAfterTheFirstMayGoBelowHalf) {
    rig_t rig {40'000};
    rig.clean(2'000);
    rig.lose_three();
    rig.clean(1'000);
    const auto first = rig.kbps;
    // The first cut went down by at most half; the link carries far less, so
    // the next one inside the window is not held to half of that again.
    EXPECT_GE(first, 40'000u * 40 / 100);
    rig.lose_three();
    rig.clean(500);
    EXPECT_EQ(rig.cuts(), 2u) << describe(rig.changes);
    EXPECT_LT(rig.kbps, first / 2);
    EXPECT_GE(rig.kbps, 1'000u);
  }

  TEST(VideoLinkRate, ACutAfterAQuietWindowIsHeldToHalfAgain) {
    rig_t rig {40'000};
    rig.clean(2'000);
    rig.lose_three();
    rig.clean(1'000);
    const auto first = rig.kbps;
    // The previous cut is older than the evidence window: an isolated cut.
    ASSERT_TRUE(access::last_cut(rig.lr));
    *access::last_cut(rig.lr) -= 3s;
    rig.lose_three();
    rig.clean(500);
    EXPECT_EQ(rig.cuts(), 2u) << describe(rig.changes);
    EXPECT_GE(rig.kbps, first * 49 / 100);
  }

  TEST(VideoLinkRate, ItProbesBackUpInSmallStepsAndReturnsToFull) {
    rig_t rig {40'000};
    rig.clean(2'000);
    rig.lose_three();
    rig.clean(60'000);
    ASSERT_GT(rig.probes(), 0u) << describe(rig.changes);
    const auto cut_at = rig.changes[0].first;
    bool first = true;
    for (const auto &[at, c] : rig.changes) {
      if (c.kind != change_kind_e::probe) {
        continue;
      }
      if (std::exchange(first, false)) {
        EXPECT_GE(at, cut_at + 2'000) << "the first probe waits the hold";
      }
      // One step is about 12.5% (5% near the failing rate), never 25%.
      EXPECT_GT(c.to_kbps, c.from_kbps);
      EXPECT_LE(c.to_kbps * 100, c.from_kbps * 125);
    }
    EXPECT_TRUE(rig.has(change_kind_e::full)) << describe(rig.changes);
    EXPECT_EQ(rig.lr.state(), state_e::full);
    EXPECT_EQ(rig.kbps, 40'000u) << "full goes back to the requested rate";
  }

  TEST(VideoLinkRate, AFailedProbeReturnsToTheLastGoodRateAndWaitsTwiceAsLong) {
    rig_t rig {40'000};
    rig.clean(2'000);
    rig.lose_three();
    const auto probe = rig.until_probe();
    const auto probe_at = rig.changes.back().first;
    rig.lose_three();
    rig.clean(400);
    const auto failed = std::find_if(rig.changes.begin(), rig.changes.end(), [](const auto &c) {
      return c.second.kind == change_kind_e::probe_failed;
    });
    ASSERT_NE(failed, rig.changes.end()) << describe(rig.changes);
    const auto failed_at = failed->first;
    EXPECT_EQ(failed->second.to_kbps, probe.from_kbps) << "back to the good rate";
    EXPECT_EQ(rig.kbps, probe.from_kbps);
    EXPECT_EQ(rig.cuts(), 1u) << "a failed probe is not another cut";
    // The wait doubled from 2 s to 4 s.
    rig.clean(30'000);
    const auto next = std::find_if(rig.changes.begin(), rig.changes.end(), [&](const auto &c) {
      return c.first > failed_at && c.second.kind == change_kind_e::probe;
    });
    ASSERT_NE(next, rig.changes.end()) << describe(rig.changes);
    EXPECT_GE(next->first, failed_at + 4'000);
    EXPECT_LT(probe_at, failed_at);
  }

  TEST(VideoLinkRate, ItDoesNotProbeWhileTheRttIsElevated) {
    rig_t rig {40'000};
    rig.clean(3'000);
    rig.lose_three();
    rig.clean(500);
    rig.rtt = 300ms;
    rig.clean(15'000);
    EXPECT_EQ(rig.cuts(), rig.changes.size()) << describe(rig.changes);
    rig.rtt = 20ms;
    rig.clean(5'000);
    EXPECT_GT(rig.probes(), 0u) << describe(rig.changes);
  }

  TEST(VideoLinkRate, ItNeverGoesBelowTheFloorAndSaysSo) {
    rig_t rig {4'000};
    rig.clean(1'000);
    auto lowest = rig.kbps;
    for (int i = 0; i < 12; ++i) {
      rig.lose_three();
      rig.clean(1'200);
      EXPECT_GE(rig.kbps, 2'000u);
      lowest = std::min(lowest, rig.kbps);
    }
    // The wire allowance for 2 Mbps is whole packets per frame.
    EXPECT_GE(lowest, 2'000u);
    EXPECT_LE(lowest, 2'500u);
    const auto floor = rig.last(change_kind_e::floor);
    ASSERT_TRUE(floor) << describe(rig.changes);
    // The floor line says what the link delivered, not 0.
    EXPECT_GT(floor->delivered_kbps, 0u);
    EXPECT_GT(floor->evidence.loss_share, 0.);
    // And the probe after it, once the losses stop.
    rig.clean(60'000);
    const auto probe = rig.last(change_kind_e::probe);
    ASSERT_TRUE(probe) << describe(rig.changes);
    EXPECT_GT(probe->delivered_kbps, 0u);
  }

  TEST(VideoLinkRate, ItIsOffWhenDisabledForPyroWaveAndAfterTheClientSetsTheBitrate) {
    auto disabled = make(40'000, false);
    EXPECT_EQ(disabled.state(), state_e::off);
    EXPECT_STREQ(disabled.reason(), "disabled");
    auto pyrowave = make(40'000, true, true);
    EXPECT_EQ(pyrowave.state(), state_e::off);
    EXPECT_STREQ(pyrowave.snapshot().reason, "pyrowave");
    for (auto *lr : {&disabled, &pyrowave}) {
      rig_t rig {std::move(*lr), 40'000};
      rig.clean(1'000);
      rig.lose_three();
      rig.clean(2'000);
      EXPECT_TRUE(rig.changes.empty()) << describe(rig.changes);
      EXPECT_EQ(rig.kbps, 40'000u);
    }

    rig_t rig {40'000};
    rig.clean(1'000);
    rig.lr.client_takeover();
    rig.lose_three();
    rig.clean(2'000);
    EXPECT_TRUE(rig.changes.empty()) << describe(rig.changes);
    EXPECT_EQ(rig.lr.state(), state_e::off);
    EXPECT_STREQ(rig.lr.snapshot().reason, "client_controlled");
  }

  TEST(VideoLinkRate, ItIsOffForAnEncoderThatRebuildsToChangeTheBitrate) {
    rig_t rig {40'000};
    rig.clean(1'000);
    rig.lr.encoder_rebuilds();
    rig.lose_three();
    rig.clean(2'000);
    EXPECT_TRUE(rig.changes.empty()) << describe(rig.changes);
    EXPECT_EQ(rig.lr.state(), state_e::off);
    EXPECT_STREQ(rig.lr.reason(), "encoder_rebuilds");
    EXPECT_FALSE(rig.lr.tick(rig.now(), 20ms, fec, framerate, 40'000, true));
  }

  TEST(VideoLinkRate, NoRequestedBitrateIsOff) {
    const auto lr = make(0);
    EXPECT_EQ(lr.state(), state_e::off);
    EXPECT_STREQ(lr.reason(), "no_bitrate");
  }

  TEST(VideoLinkRate, TheCeilingIsTheRequestedBitrateUpToTheMaximum) {
    EXPECT_EQ(make(40'000).ceiling_kbps(), 40'000u);
    EXPECT_EQ(make(900'000).ceiling_kbps(), max_kbps);
  }

  TEST(VideoLinkRate, AProbeThatFailsAfterAChainOfConfirmedStepsGoesBackOneStep) {
    // A cut, then four steps confirmed in a row, then the RTT rises and three
    // frames are lost: back to the step before the last one, not to where the
    // climb began.
    rig_t rig {40'000};
    rig.clean(2'000);
    rig.lose_three();
    rig.clean(200);
    const auto cut = rig.last(change_kind_e::step_down);
    ASSERT_TRUE(cut);
    const auto cut_total = access::good(rig.lr);
    while (rig.probes() < 4 && !::testing::Test::HasFailure()) {
      rig.until_probe();
    }
    rig.rtt = 300ms;
    // Every outstanding step is confirmed; no new one is issued.
    for (int i = 0; i < 400 && access::has_pending(rig.lr); ++i) {
      rig.clean(10);
    }
    ASSERT_FALSE(access::has_pending(rig.lr));
    const auto before = access::confirmed_before(rig.lr);
    ASSERT_TRUE(before) << "a recent confirmation";
    EXPECT_GT(*before, cut_total) << "a chain, not one step";
    const auto expect = access::kbps_for(rig.lr, *before, fec, framerate);
    EXPECT_GT(expect, cut->to_kbps);
    rig.lose_three();
    rig.clean(200);
    const auto failed = rig.last(change_kind_e::probe_failed);
    ASSERT_TRUE(failed) << describe(rig.changes);
    EXPECT_EQ(failed->to_kbps, expect);
    EXPECT_EQ(rig.kbps, expect);
    EXPECT_EQ(rig.cuts(), 1u);
  }

  TEST(VideoLinkRate, AStepSentJustBeforeThePictureGoesStaticIsNotConfirmedByTheStillness) {
    rig_t rig {40'000};
    rig.clean(2'000);
    rig.lose_three();
    const auto probe = rig.until_probe();
    const auto good = access::good(rig.lr);
    rig.busy = false;
    rig.clean(6'000);
    EXPECT_EQ(access::good(rig.lr), good) << "a static picture confirmed a step";
    EXPECT_TRUE(access::bad(rig.lr));
    // Motion returns and the link cannot carry the step.
    rig.busy = true;
    rig.lose_three();
    rig.clean(200);
    const auto failed = rig.last(change_kind_e::probe_failed);
    ASSERT_TRUE(failed) << describe(rig.changes);
    EXPECT_EQ(failed->to_kbps, probe.from_kbps);
    EXPECT_EQ(rig.cuts(), 1u) << "not another cut";
  }

  TEST(VideoLinkRate, AQuietStreamNeverProbes) {
    rig_t rig {40'000};
    rig.clean(2'000);
    rig.lose_three();
    rig.busy = false;
    rig.clean(40'000);
    EXPECT_EQ(rig.probes(), 0u) << describe(rig.changes);
    EXPECT_EQ(rig.lr.state(), state_e::limited);
    rig.busy = true;
    rig.clean(5'000);
    EXPECT_GT(rig.probes(), 0u);
  }

  TEST(VideoLinkRate, LessFecNeverRaisesTheBitrateWithoutAProbe) {
    rig_t rig {40'000};
    rig.clean(2'000);
    rig.lose_three();
    rig.clean(200);
    const auto now = rig.now();
    const auto at = [&](int with_fec) {
      return rig.lr.tick(now, rig.rtt, with_fec, framerate, rig.kbps, true).value_or(0);
    };
    const auto with_20 = at(20);
    ASSERT_GT(with_20, 0u);
    EXPECT_LE(at(10), with_20);
    EXPECT_LE(at(0), with_20);
    // More FEC leaves less for the video, as before.
    EXPECT_LT(at(30), with_20);
  }

  TEST(VideoLinkRate, BadIsForgottenAfterAMinuteAtIt) {
    rig_t rig {40'000};
    rig.clean(2'000);
    rig.lose_three();
    rig.clean(500);
    // Sit at the bad rate: no probes, so only time passes.
    access::bad(rig.lr) = access::current(rig.lr);
    access::next_probe(rig.lr) = rig.now() + 1'000s;
    rig.clean(59'000);
    EXPECT_TRUE(access::bad(rig.lr)) << "forgotten too early";
    rig.clean(2'000);
    EXPECT_FALSE(access::bad(rig.lr)) << "still remembered after a minute";
  }

  TEST(VideoLinkRate, ConsecutiveFailedProbesDoubleTheWaitUpToTheMaximum) {
    rig_t rig {40'000};
    rig.clean(2'000);
    rig.lose_three();
    std::vector<duration> holds;
    for (int i = 0; i < 5; ++i) {
      rig.until_probe();
      rig.lose_three();
      rig.clean(200);
      holds.push_back(access::hold(rig.lr));
    }
    const std::vector<duration> expected {4s, 8s, 8s, 8s, 8s};
    EXPECT_EQ(holds, expected);
    EXPECT_EQ(access::hold(rig.lr), max_hold);
  }

  TEST(VideoLinkRate, FailuresAtTheSameRateDoubleTheKneeWaitUpToItsMaximum) {
    // The link carries 27 Mbps of the 40 requested: every step over it fails.
    rig_t rig {40'000};
    rig.clean(2'000);
    rig.lose_three();
    constexpr std::uint32_t limit = 27'000;
    std::vector<duration> knees;
    while (rig.ms < 600'000) {
      rig.clean(100);
      if (access::has_pending(rig.lr) && rig.kbps > limit) {
        rig.lose_three();
        rig.clean(200);
        knees.push_back(access::knee(rig.lr));
      }
    }
    ASSERT_GE(knees.size(), 6u);
    EXPECT_TRUE(std::is_sorted(knees.begin(), knees.end()));
    EXPECT_EQ(knees.back(), knee_max_hold);
    EXPECT_TRUE(std::all_of(knees.begin(), knees.end(), [](duration knee) {
      return knee <= knee_max_hold;
    }));
  }

  TEST(VideoLinkRate, ACutFromWithinTenPercentOfTheBadRateCountsAsAKneeFailure) {
    {
      rig_t rig {40'000};
      rig.clean(2'000);
      rig.lose_three();
      rig.clean(500);
      // The bad rate sits 5% above where the stream is.
      access::bad(rig.lr) = access::current(rig.lr) * 105 / 100;
      access::hold(rig.lr) = hold;
      access::next_probe(rig.lr) = rig.now() + 1'000s;
      rig.clean(1'000);
      rig.lose_three();
      rig.clean(200);
      EXPECT_EQ(rig.cuts(), 2u) << describe(rig.changes);
      EXPECT_EQ(access::knee(rig.lr), hold * 2);
      EXPECT_TRUE(access::knee_until(rig.lr));
      EXPECT_EQ(access::hold(rig.lr), hold * 2) << "the wait is not reset to the first";
    }
    // A cut from far above the bad rate is a new situation.
    rig_t rig {40'000};
    rig.clean(2'000);
    rig.lose_three();
    rig.clean(500);
    access::bad(rig.lr) = access::current(rig.lr) * 2;
    access::next_probe(rig.lr) = rig.now() + 1'000s;
    rig.clean(1'000);
    rig.lose_three();
    rig.clean(200);
    EXPECT_EQ(access::knee(rig.lr), hold);
    EXPECT_FALSE(access::knee_until(rig.lr));
    EXPECT_EQ(access::hold(rig.lr), hold);
  }

  TEST(VideoLinkRate, ProbesStayUnderAKnownKneeUntilItsWaitIsOver) {
    rig_t rig {40'000};
    rig.clean(2'000);
    rig.lose_three();
    rig.clean(500);
    const auto before = rig.changes.size();
    const auto bad = access::current(rig.lr) * 100 / 80;
    access::bad(rig.lr) = bad;
    access::knee_until(rig.lr) = rig.now() + 20s;
    access::next_probe(rig.lr) = rig.now();
    const auto cap = access::kbps_for(rig.lr, bad * knee_cap_percent / 100, fec, framerate);
    rig.clean(15'000);
    EXPECT_GT(rig.probes(), 0u);
    for (auto i = before; i < rig.changes.size(); ++i) {
      EXPECT_LE(rig.changes[i].second.to_kbps, cap) << describe(rig.changes);
    }
    // After the wait the probes go on past it.
    rig.clean(30'000);
    EXPECT_TRUE(std::any_of(rig.changes.begin() + std::ptrdiff_t(before), rig.changes.end(), [&](const auto &c) {
      return c.second.to_kbps > cap;
    })) << describe(rig.changes);
  }

  TEST(VideoLinkRate, AConfirmationBelowTheBadRateKeepsTheKneeWait) {
    rig_t rig {40'000};
    rig.clean(2'000);
    rig.lose_three();
    rig.clean(500);
    access::bad(rig.lr) = access::current(rig.lr) * 2;
    access::knee(rig.lr) = hold * 4;
    access::knee_until(rig.lr) = rig.now() + 1s;
    const auto good = access::good(rig.lr);
    rig.until_probe();
    rig.clean(3'000);
    EXPECT_GT(access::good(rig.lr), good) << "the step was confirmed";
    EXPECT_TRUE(access::bad(rig.lr));
    EXPECT_EQ(access::knee(rig.lr), hold * 4);
  }

  TEST(VideoLinkRate, AWrapOfTheFrameCounterChangesNothing) {
    const auto run = [](std::uint32_t start) {
      rig_t rig {40'000};
      rig.frame = start;
      rig.clean(2'000);
      rig.lose_three();
      rig.clean(6'000);
      rig.lose_three();
      rig.clean(30'000);
      return rig.changes;
    };
    const auto plain = run(0);
    ASSERT_GT(plain.size(), 3u) << describe(plain);
    for (const std::uint32_t start : {0xffff'ffffu - 100, 0xffff'ffffu - 1'500, 0xffff'ffffu - 20}) {
      const auto wrapped = run(start);
      ASSERT_EQ(wrapped.size(), plain.size()) << "start " << start << ": " << describe(wrapped);
      for (std::size_t i = 0; i < plain.size(); ++i) {
        EXPECT_EQ(wrapped[i].first, plain[i].first) << "start " << start;
        EXPECT_TRUE(same(wrapped[i].second, plain[i].second)) << "start " << start << ": " << describe(wrapped);
      }
    }
  }

  /// Lets the RTT follow a queue that fills while the encoder sends more than
  /// `link_kbps`, seen through a smoothed sample. Returns the largest queue, ms.
  double queue_for(rig_t &rig, std::uint32_t link_kbps, std::uint64_t duration_ms, double &queue) {
    double largest = 0.;
    double shown = queue;
    for (std::uint64_t i = 0; i < duration_ms / 100; ++i) {
      queue = std::clamp(queue + (double(rig.kbps) / double(link_kbps) - 1.) * 100., 0., 300.);
      largest = std::max(largest, queue);
      shown += (queue - shown) * 0.1;
      rig.rtt = ms_t(20 + std::uint64_t(shown));
      rig.clean(100);
    }
    return largest;
  }

  std::vector<change_t> queue_changes(const rig_t &rig) {
    std::vector<change_t> out;
    for (const auto &[at, c] : rig.changes) {
      if (std::string_view {c.reason} == "queue") {
        out.push_back(c);
      }
    }
    return out;
  }

  TEST(VideoLinkRate, ALosslessStandingQueueAfterAProbeIsCutAndTheRttReturnsToTheFloor) {
    // A probe step goes over the link, the queue grows without a lost packet,
    // and probes stop while the RTT is up. No loss evidence exists to cut on;
    // the RTT staying over its floor does.
    rig_t rig {40'000};
    rig.rtt_jitter_ms = 3;
    rig.clean(3'000);
    rig.lose_three();
    double queue = 0.;
    const auto largest = queue_for(rig, 30'000, 60'000, queue);
    const auto cuts = queue_changes(rig);
    ASSERT_FALSE(cuts.empty()) << describe(rig.changes);
    // 15 ms of queue for two seconds, then 12%; never the 300 ms an uncut
    // probe would grow to.
    EXPECT_LT(largest, 200.) << describe(rig.changes);
    EXPECT_LE(cuts.size(), 6u);
    const auto low = std::min_element(cuts.begin(), cuts.end(), [](const auto &a, const auto &b) {
                       return a.to_kbps < b.to_kbps;
                     })->to_kbps;
    EXPECT_GE(low, 30'000u * 60 / 100);
    EXPECT_LE(rig.kbps, 31'500u);
  }

  TEST(VideoLinkRate, RttSpikesAndExcursionsUnderTwoSecondsAreNoQueue) {
    for (const std::uint16_t parity : {7, 10, 13, 16}) {
      rig_t rig {14'988};
      rig.rtt_jitter_ms = 8;
      std::uint64_t seed = 0x9e37'79b9'7f4a'7c15ull;
      const auto arm = [&](link_rate_t &lr, std::uint32_t frame, time_point now) {
        wifi_block(seed, lr, frame, parity, 5, 0, now);
      };
      rig.run(5'000, arm);
      rig.lose_three();
      // 700 ms up to 60 ms, 700 ms back, while it probes back up.
      for (int i = 0; i < 42; ++i) {
        rig.rtt = i % 2 == 0 ? 60ms : 20ms;
        rig.run(700, arm);
      }
      // 1.8 s up, five times, with 4 s of calm between.
      for (int i = 0; i < 5; ++i) {
        rig.rtt = 60ms;
        rig.run(1'800, arm);
        rig.rtt = 20ms;
        rig.run(4'000, arm);
      }
      EXPECT_TRUE(queue_changes(rig).empty()) << "parity " << parity << ": " << describe(rig.changes);
    }
  }

  // ---- the link simulation ----

  struct rng_t {
    std::uint64_t state;

    explicit rng_t(std::uint64_t seed):
        state {seed | 1} {
    }

    double next() {
      state ^= state << 13;
      state ^= state >> 7;
      state ^= state << 17;
      return double(state >> 11) / double(1ull << 53);
    }
  };

  /// A token-bucket link with a drop-tail queue.
  struct link_t {
    /// Link rate in bit/s at a time in seconds.
    std::function<double(double)> rate;
    double queue_ms = 50.;
    double random_loss = 0.;
    /// Gilbert burst loss: chance per packet to enter the bad state, to leave
    /// it, and to lose a packet in it.
    std::optional<std::array<double, 3>> burst;
    bool in_burst = false;
    double delay = 0.002;
    double backlog = 0.;
    double at = 0.;
    rng_t rng {7};

    static link_t with_rate(std::function<double(double)> rate) {
      link_t link;
      link.rate = std::move(rate);
      return link;
    }

    static link_t constant(double mbps) {
      return with_rate([mbps](double) {
        return mbps * 1e6;
      });
    }

    /// `a` until `from` seconds, `b` until `to`, then `a` again.
    static link_t step(double a, double b, double from, double to) {
      return with_rate([=](double t) {
        return (t >= from && t < to ? b : a) * 1e6;
      });
    }

    /// Seconds the queue takes to drain at `t`.
    double queue_delay(double t) const {
      const double r = std::max(rate(t), 1.);
      return std::max(backlog - r / 8. * std::max(t - at, 0.), 0.) * 8. / r;
    }

    /// Arrival time of a datagram of `bytes` offered at `t`, or nothing if the
    /// queue is full (or the packet is lost at random).
    std::optional<double> offer(double t, double bytes) {
      t = std::max(t, at);
      const double mean = rate((at + t) / 2.);
      backlog = std::max(backlog - mean / 8. * (t - at), 0.);
      at = t;
      const double r = rate(t);
      if (backlog + bytes > r / 8. * queue_ms / 1000.) {
        return std::nullopt;
      }
      backlog += bytes;
      if (random_loss > 0. && rng.next() < random_loss) {
        return std::nullopt;
      }
      if (burst) {
        const auto [enter, leave, loss] = *burst;
        const double flip = rng.next();
        in_burst = in_burst ? flip >= leave : flip < enter;
        if (in_burst && rng.next() < loss) {
          return std::nullopt;
        }
      }
      return t + backlog * 8. / r + delay;
    }
  };

  link_t queued(link_t link, double ms) {
    link.queue_ms = ms;
    return link;
  }

  link_t gilbert(double mbps, double enter, double leave, double loss) {
    auto link = link_t::constant(mbps);
    link.burst = std::array {enter, leave, loss};
    return link;
  }

  link_t lossy(link_t link, double share) {
    link.random_loss = share;
    return link;
  }

  struct sim_frame_t {
    double at;
    std::uint32_t kbps;
    bool complete;
    double arrival;
    std::uint32_t packets;
  };

  /// A 60 fps source with 20% FEC on `link`. Like stock Moonlight, the client
  /// reports FEC status only for blocks that lost packets; a block that lost
  /// every packet is never reported. A frame leaves in a burst (`spread`, 5% of
  /// the frame period), every 120th frame is three times the usual size, and
  /// the RTT the controller sees is the base RTT plus the time the queue takes
  /// to drain, sampled with the 100 ms pings.
  struct sim_t {
    static constexpr double period = 0.016667;
    static constexpr std::uint64_t datagram = 1024 + 16 + 86;

    link_rate_t lr;
    link_t link;
    time_point base {clock::now()};
    /// Delay from the last packet of a frame to the host reading the report.
    double feedback;
    /// Share of the frame period the packets of one frame are spread over.
    double spread = 0.05;
    std::vector<sim_frame_t> frames;
    std::vector<std::pair<double, change_t>> changes;
    std::vector<std::pair<double, std::uint32_t>> kbps_log;
    /// The lowest bitrate the simulated encoder produces, however low it is asked.
    std::uint32_t encoder_min_kbps = 0;
    std::uint32_t encoder_kbps;
    /// The encoder applies a new bitrate two frames later.
    std::array<std::uint32_t, 2> applied;
    rng_t noise {99};
    std::vector<std::pair<double, fec_status_t>> inbox;

    sim_t(link_t l, std::uint32_t kbps, double feedback_ms):
        lr {make(kbps)},
        link {std::move(l)},
        feedback {feedback_ms / 1000.},
        encoder_kbps {kbps},
        applied {kbps, kbps} {
    }

    time_point at(double t) const {
      return base + std::chrono::duration_cast<duration>(std::chrono::duration<double> {t});
    }

    /// Data and parity packets per FEC block, as the host packetizes a frame.
    static std::vector<std::pair<std::size_t, std::size_t>> block_layout(std::size_t payload_bytes) {
      const std::size_t slice = 1008;
      const std::size_t total = (payload_bytes + 8 + slice - 1) / slice;
      const std::size_t max_data = 255 * 100 / (100 + fec);
      std::size_t blocks = std::max<std::size_t>((total + max_data - 1) / max_data, 1);
      const bool fec_off = blocks > 4;
      blocks = std::min<std::size_t>(blocks, 4);
      const std::size_t aligned = (total + blocks - 1) / blocks;
      std::vector<std::pair<std::size_t, std::size_t>> layout;
      for (std::size_t block = 0; block < blocks; ++block) {
        const std::size_t data = std::min(total > block * aligned ? total - block * aligned : 0, aligned);
        if (data == 0) {
          continue;
        }
        layout.emplace_back(data, fec_off ? 0 : stream::pacing::fec_expanded_packets(data, fec, 0) - data);
      }
      return layout;
    }

    void send_frame(double t, std::uint32_t index) {
      const auto kbps = std::max(applied[0], encoder_min_kbps);
      applied[0] = applied[1];
      applied[1] = encoder_kbps;
      const double average = double(kbps) * 125. / 60.;
      const double scale = index == 0 ? 2. : index % 120 == 0 ? 3. :
                                                                1.;
      const double size = average * (0.9 + 0.2 * noise.next()) * scale;
      const auto layout = block_layout(std::size_t(size));
      std::size_t packets = 0;
      for (const auto &[data, parity] : layout) {
        packets += data + parity;
      }
      lr.on_sent(index, std::uint32_t(packets), std::uint64_t(packets) * datagram, at(t));
      std::size_t n = 0;
      double last_arrival = t;
      bool complete = true;
      for (std::size_t block = 0; block < layout.size(); ++block) {
        const auto [data, parity] = layout[block];
        std::uint16_t got_data = 0;
        std::uint16_t got_parity = 0;
        for (std::size_t i = 0; i < data + parity; ++i) {
          const double when = t + double(n) / double(packets) * spread * period;
          ++n;
          if (const auto arrival = link.offer(when, double(datagram))) {
            last_arrival = std::max(last_arrival, *arrival);
            if (i < data) {
              ++got_data;
            } else {
              ++got_parity;
            }
          }
        }
        const std::size_t received = std::size_t(got_data) + got_parity;
        if (received < data) {
          complete = false;
        }
        // A block that lost everything is never reported.
        if (received < data + parity && received > 0) {
          fec_status_t s;
          s.frame = index;
          s.data = std::uint16_t(data);
          s.parity = std::uint16_t(parity);
          s.received_data = got_data;
          s.received_parity = got_parity;
          s.percentage = std::uint8_t(fec);
          s.block = std::uint8_t(block);
          s.blocks = std::uint8_t(layout.size());
          inbox.emplace_back(last_arrival + feedback, s);
        }
      }
      frames.push_back({t, kbps, complete, last_arrival, std::uint32_t(packets)});
    }

    void deliver(double t) {
      const auto now = at(t);
      for (std::size_t i = 0; i < inbox.size();) {
        if (inbox[i].first > t) {
          ++i;
          continue;
        }
        const auto s = inbox[i].second;
        inbox.erase(inbox.begin() + std::ptrdiff_t(i));
        lr.on_status(s, now);
        if (std::uint32_t(s.received_data) + s.received_parity < s.data) {
          lr.on_unrecoverable(s.frame, now);
        }
      }
    }

    void run(double seconds) {
      const double base_rtt = 2. * (link.delay + feedback) + 0.001;
      const auto as_duration = [](double s) {
        return std::chrono::duration_cast<duration>(std::chrono::duration<double> {s});
      };
      auto rtt = as_duration(base_rtt);
      const auto total_ms = std::uint64_t(seconds * 1000.);
      std::uint32_t index = 0;
      double next_frame = 0.;
      for (std::uint64_t k = 0; k < total_ms; ++k) {
        const double t = double(k) / 1000.;
        const auto now = at(t);
        if (t >= next_frame) {
          send_frame(t, index);
          ++index;
          next_frame += period;
        }
        deliver(t);
        if (k % 100 == 0) {
          rtt = as_duration(base_rtt + link.queue_delay(t));
          lr.on_ping(now, ping_payload);
        }
        if (k % 8 == 0) {
          if (const auto kbps = lr.tick(now, rtt, fec, framerate, encoder_kbps, true)) {
            encoder_kbps = *kbps;
          }
          if (auto change = lr.take_change()) {
            changes.emplace_back(t, *change);
          }
        }
        if (k % 1000 == 0) {
          kbps_log.emplace_back(t, encoder_kbps);
        }
      }
    }

    template<class F>
    void each_frame(double from, double to, F &&f) const {
      for (const auto &frame : frames) {
        if (frame.at >= from && frame.at < to) {
          f(frame);
        }
      }
    }

    /// Share of frames sent in the window that the client could not rebuild.
    double frame_loss(double from, double to) const {
      std::size_t total = 0;
      std::size_t lost = 0;
      each_frame(from, to, [&](const sim_frame_t &f) {
        ++total;
        lost += f.complete ? 0 : 1;
      });
      return double(lost) / double(std::max<std::size_t>(total, 1));
    }

    /// Longest time between two rebuilt frames arriving in the window.
    double longest_gap(double from, double to) const {
      double last = from;
      double gap = 0.;
      for (const auto &f : frames) {
        if (f.complete && f.arrival >= from && f.arrival < to) {
          gap = std::max(gap, f.arrival - last);
          last = f.arrival;
        }
      }
      return std::max(gap, to - last);
    }

    /// Mean encoder bitrate of the frames sent in the window, kbps.
    double mean_kbps(double from, double to) const {
      double sum = 0.;
      std::size_t total = 0;
      each_frame(from, to, [&](const sim_frame_t &f) {
        sum += f.kbps;
        ++total;
      });
      return sum / double(std::max<std::size_t>(total, 1));
    }

    /// Mean offered wire rate in the window, bit/s.
    double mean_wire_bps(double from, double to) const {
      double packets = 0.;
      each_frame(from, to, [&](const sim_frame_t &f) {
        packets += f.packets;
      });
      return packets * double(datagram) * 8. / std::max(to - from, 1e-9);
    }

    std::size_t count(change_kind_e kind) const {
      return std::size_t(std::count_if(changes.begin(), changes.end(), [&](const auto &c) {
        return c.second.kind == kind;
      }));
    }

    std::size_t cuts() const {
      return count(change_kind_e::step_down);
    }

    /// First time at or after `from` that the encoder runs at `kbps` or more.
    std::optional<double> reaches(double from, std::uint32_t kbps) const {
      for (const auto &f : frames) {
        if (f.at >= from && f.kbps >= kbps) {
          return f.at;
        }
      }
      return std::nullopt;
    }

    std::uint32_t lowest_kbps_after(double from) const {
      std::uint32_t lowest = std::numeric_limits<std::uint32_t>::max();
      for (const auto &[t, kbps] : kbps_log) {
        if (t > from) {
          lowest = std::min(lowest, kbps);
        }
      }
      return lowest;
    }

    std::string describe() const {
      std::string out;
      for (const auto &[t, c] : changes) {
        out += std::to_string(t) + "s " + change_kind_name(c.kind) + '(' + c.reason + ") " +
               std::to_string(c.from_kbps) + "->" + std::to_string(c.to_kbps) + "; ";
      }
      return out;
    }
  };

  TEST(VideoLinkRateSim, AStreamAboveItsLinkSettlesNearTheLinkRateWithLittleLoss) {
    for (const auto [link_mbps, kbps] : std::array<std::pair<double, std::uint32_t>, 4> {{{150., 225'000}, {80., 120'000}, {20., 40'000}, {20., 20'000}}}) {
      for (const double feedback : {2., 20., 80.}) {
        // The host sends a frame in a burst (0.05 of the period); the 5 ms
        // queue only with the packets spread over the whole period.
        for (const auto [queue, spread] : std::array<std::pair<double, double>, 3> {{{50., 0.05}, {50., 1.}, {5., 1.}}}) {
          sim_t sim {queued(link_t::constant(link_mbps), queue), kbps, feedback};
          sim.spread = spread;
          sim.run(75.);
          const auto what = std::to_string(link_mbps) + " Mbps, " + std::to_string(kbps) + " kbps, fb " +
                            std::to_string(feedback) + ", q " + std::to_string(queue) + ", sp " + std::to_string(spread);
          const auto wire = sim.mean_wire_bps(55., 75.) / 1e6;
          EXPECT_GE(wire, link_mbps * 0.65) << what << ": " << sim.describe();
          EXPECT_LE(wire, link_mbps * 1.08) << what;
          // Settled, and staying settled: every 5 s window, not the mean.
          for (int from = 20; from < 75; from += 5) {
            EXPECT_GE(sim.mean_wire_bps(from, from + 5.) / 1e6, link_mbps * 0.65) << what << " from " << from << " s";
          }
          EXPECT_LT(sim.frame_loss(3., 75.), 0.02) << what;
          EXPECT_LT(sim.longest_gap(3., 75.), 0.5) << what;
        }
      }
    }
  }

  TEST(VideoLinkRateSim, AQueueFillingOnALinkTheStreamExceedsCutsBeforeFecGivesOut) {
    // About 7% more on the wire than the link carries, packets spread over the
    // frame period: FEC rebuilds nearly every frame, the RTT climbs by the
    // whole queue, and only the rebuilt blocks, with the RTT, show it.
    sim_t sim {queued(link_t::constant(50.), 50.), 38'800, 20.};
    sim.spread = 1.;
    sim.run(30.);
    ASSERT_FALSE(sim.changes.empty());
    const auto &[at, first] = sim.changes.front();
    EXPECT_EQ(first.kind, change_kind_e::step_down) << sim.describe();
    EXPECT_STREQ(first.reason, "fec_loss_share") << sim.describe();
    EXPECT_LT(at, 5.);
    const auto wire = sim.mean_wire_bps(10., 30.) / 1e6;
    EXPECT_GE(wire, 50. * 0.65);
    EXPECT_LE(wire, 50. * 1.02);
    EXPECT_LT(sim.frame_loss(3., 30.), 0.02);
  }

  TEST(VideoLinkRateSim, AnEncoderAtOneAndAFifthOfTheLinkCutsWithinTwoSecondsAndSettlesUnderIt) {
    sim_t sim {queued(link_t::constant(50.), 50.), 60'000, 20.};
    sim.run(30.);
    ASSERT_FALSE(sim.changes.empty());
    EXPECT_EQ(sim.changes.front().second.kind, change_kind_e::step_down) << sim.describe();
    EXPECT_LT(sim.changes.front().first, 2.);
    const auto wire = sim.mean_wire_bps(10., 30.) / 1e6;
    EXPECT_GE(wire, 50. * 0.65);
    EXPECT_LE(wire, 50. * 1.08);
    EXPECT_LT(sim.frame_loss(3., 30.), 0.02);
  }

  TEST(VideoLinkRateSim, ABigOvershootIsOneCutNotACascade) {
    // 179 Mbps requested on a 138 Mbps link.
    for (const double feedback : {2., 20., 80.}) {
      for (const auto [queue, spread] : std::array<std::pair<double, double>, 3> {{{50., 0.05}, {50., 1.}, {5., 1.}}}) {
        sim_t sim {queued(link_t::constant(138.), queue), 179'000, feedback};
        sim.spread = spread;
        sim.run(75.);
        const auto what = "fb " + std::to_string(feedback) + ", q " + std::to_string(queue) + ", sp " + std::to_string(spread);
        const auto early = std::count_if(sim.changes.begin(), sim.changes.end(), [](const auto &c) {
          return c.first < 10. && c.second.kind == change_kind_e::step_down;
        });
        EXPECT_LE(early, 2) << what << ": " << sim.describe();
        std::uint32_t lowest = std::numeric_limits<std::uint32_t>::max();
        for (std::size_t i = 2; i < sim.kbps_log.size(); ++i) {
          lowest = std::min(lowest, sim.kbps_log[i].second);
        }
        EXPECT_GE(lowest, 55'000u) << what;
        EXPECT_GE(sim.mean_kbps(55., 75.), 80'000.) << what;
        for (int from = 20; from < 75; from += 5) {
          EXPECT_GE(sim.mean_kbps(from, from + 5.), 70'000.) << what << " from " << from << " s";
        }
        EXPECT_LT(sim.frame_loss(3., 75.), 0.02) << what;
      }
    }
  }

  TEST(VideoLinkRateSim, AnEncoderThatCannotGoBelowTheLinkIsNotCutInARunaway) {
    // An encoder that sends at least 14 Mbps however low it is asked, on a
    // 12 Mbps link: cutting again at every fence would end at 2 Mbps.
    for (const double feedback : {2., 20., 80.}) {
      sim_t sim {queued(link_t::constant(12.), 50.), 40'000, feedback};
      sim.encoder_min_kbps = 14'000;
      sim.run(60.);
      const auto what = "fb " + std::to_string(feedback) + ": " + sim.describe();
      const auto found = std::find_if(sim.changes.begin(), sim.changes.end(), [](const auto &c) {
        return c.second.kind == change_kind_e::encoder_floor;
      });
      ASSERT_NE(found, sim.changes.end()) << what;
      EXPECT_EQ(sim.count(change_kind_e::floor), 0u) << what;
      EXPECT_LE(sim.cuts(), 3u) << what;
      const auto found_at = found->first;
      EXPECT_LT(found_at, 15.) << what;
      // Nothing asks for less than the encoder sends, from then on.
      EXPECT_GE(sim.lowest_kbps_after(found_at + 1.), 12'000u) << what;
      bool probed = false;
      for (const auto &[t, c] : sim.changes) {
        if (t <= found_at) {
          continue;
        }
        if (c.kind == change_kind_e::step_down) {
          EXPECT_GE(c.to_kbps, 12'000u) << what;
        }
        probed |= c.kind == change_kind_e::probe;
      }
      // It keeps trying upward from the floor, not from the minimum.
      EXPECT_TRUE(probed) << what;
      EXPECT_GE(sim.lr.snapshot().encoder_floor_kbps, 12'000u) << what;
    }
  }

  TEST(VideoLinkRateSim, AnEncoderThatFollowsItsBitrateNeverGetsAnEncoderFloor) {
    for (const auto [link_mbps, kbps] : std::array<std::pair<double, std::uint32_t>, 2> {{{150., 225'000}, {20., 40'000}}}) {
      sim_t sim {queued(link_t::constant(link_mbps), 50.), kbps, 20.};
      sim.run(75.);
      EXPECT_EQ(sim.count(change_kind_e::encoder_floor), 0u) << link_mbps << ": " << sim.describe();
    }
  }

  TEST(VideoLinkRateSim, ALinkThatDropsToAnEighthRecoversWithinTwentyFiveSeconds) {
    // 80 -> 10 -> 80 Mbps at a 40 Mbps request; the link returns at 40 s.
    for (const auto [feedback, queue, spread] : std::array<std::array<double, 3>, 6> {{{2., 50., 0.05}, {20., 50., 0.05}, {20., 50., 1.}, {20., 5., 1.}, {80., 50., 0.05}, {80., 50., 1.}}}) {
      sim_t sim {queued(link_t::step(80., 10., 20., 40.), queue), 40'000, feedback};
      sim.spread = spread;
      sim.run(75.);
      const auto what = "fb " + std::to_string(feedback) + ", q " + std::to_string(queue) + ", sp " + std::to_string(spread) + ": " + sim.describe();
      // An eighth of the rate takes two cuts: the first is held to half, the
      // second, right after it, goes to the delivered rate.
      EXPECT_LE(sim.longest_gap(19., 45.), 1.5) << what;
      EXPECT_LE(sim.frame_loss(3., 75.), 0.03) << what;
      const auto back = sim.reaches(40., 36'000);
      ASSERT_TRUE(back) << what;
      // The last probe that failed on the dip keeps probes under its rate for
      // the quiet hold, and a step at a time past it.
      EXPECT_LE(*back, 65.) << what;
      EXPECT_LE(sim.cuts(), 2u) << what;
    }
  }

  TEST(VideoLinkRateSim, ALinkThatDropsToATenthRecoversWithinTwentySeconds) {
    for (const auto [feedback, queue, spread] : std::array<std::array<double, 3>, 5> {{{2., 50., 0.05}, {20., 50., 0.05}, {20., 5., 1.}, {80., 50., 0.05}, {80., 50., 1.}}}) {
      sim_t sim {queued(link_t::step(300., 30., 20., 40.), queue), 40'000, feedback};
      sim.spread = spread;
      sim.run(75.);
      const auto what = "fb " + std::to_string(feedback) + ", q " + std::to_string(queue) + ", sp " + std::to_string(spread) + ": " + sim.describe();
      EXPECT_LE(sim.longest_gap(19., 45.), 1.5) << what;
      EXPECT_LE(sim.frame_loss(3., 75.), 0.03) << what;
      const auto back = sim.reaches(40., 36'000);
      ASSERT_TRUE(back) << what;
      EXPECT_LE(*back, 60.) << what;
      EXPECT_EQ(sim.cuts(), 1u) << what;
    }
  }

  TEST(VideoLinkRateSim, AHealthyLinkNeverChangesAnything) {
    struct case_t {
      const char *name;
      std::function<link_t()> make;
      std::uint32_t kbps;
    };

    const std::vector<case_t> links {
      {"1 GbE 20", [] {
         return link_t::constant(1000.);
       },
       20'000},
      {"1 GbE 40", [] {
         return link_t::constant(1000.);
       },
       40'000},
      {"1 GbE 80", [] {
         return link_t::constant(1000.);
       },
       80'000},
      {"150 Mbps 80", [] {
         return link_t::constant(150.);
       },
       80'000},
      {"300 Mbps 40", [] {
         return link_t::constant(300.);
       },
       40'000},
      // Wi-Fi: 0.5% and 1% random loss, repaired by FEC.
      {"wifi 0.5%", [] {
         return lossy(link_t::constant(1000.), 0.005);
       },
       40'000},
      {"wifi 1%", [] {
         return lossy(link_t::constant(1000.), 0.01);
       },
       80'000},
      // Burst loss: a Gilbert channel, mild and hard, repaired by FEC.
      {"gilbert mild", [] {
         return gilbert(1000., 0.002, 0.2, 0.6);
       },
       40'000},
      {"gilbert hard", [] {
         return gilbert(1000., 0.004, 0.15, 0.7);
       },
       80'000},
      {"gilbert 150", [] {
         return gilbert(150., 0.002, 0.2, 0.6);
       },
       80'000},
      // A dip the stream never feels.
      {"dip at 20 Mbps", [] {
         return link_t::step(300., 30., 20., 40.);
       },
       20'000},
    };
    for (const auto &link : links) {
      for (const double feedback : {2., 20., 80.}) {
        sim_t sim {link.make(), link.kbps, feedback};
        sim.run(75.);
        const auto what = std::string {link.name} + " fb " + std::to_string(feedback);
        EXPECT_TRUE(sim.changes.empty()) << what << ": " << sim.describe();
        // A burst can take a frame whole; two in a thousand is the limit.
        const double allowed = std::string_view {link.name}.starts_with("gilbert") ? 0.002 : 0.;
        EXPECT_LE(sim.frame_loss(0., 75.), allowed) << what;
        EXPECT_TRUE(std::all_of(sim.kbps_log.begin(), sim.kbps_log.end(), [&](const auto &entry) {
          return entry.second == link.kbps;
        })) << what;
      }
    }
  }

  TEST(VideoLinkRateSim, ProbesStopUnderTheRateThatFailedForTheQuietHold) {
    // A probe that failed at the link's edge must not be followed by climbs
    // back to the same edge and a freeze at each.
    for (const auto [feedback, spread] : std::array<std::pair<double, double>, 4> {{{20., 0.05}, {20., 1.}, {80., 0.05}, {80., 1.}}}) {
      sim_t sim {queued(link_t::constant(50.), 50.), 60'000, feedback};
      sim.spread = spread;
      sim.run(60.);
      const auto what = "fb " + std::to_string(feedback) + ", sp " + std::to_string(spread) + ": " + sim.describe();
      const auto failed = std::find_if(sim.changes.begin(), sim.changes.end(), [](const auto &c) {
        return c.second.kind == change_kind_e::probe_failed;
      });
      ASSERT_NE(failed, sim.changes.end()) << what;
      const auto failed_at = failed->first;
      const auto failed_kbps = failed->second.from_kbps;
      // For the quiet hold nothing goes back to the rate that failed.
      std::uint32_t top = 0;
      for (const auto &[t, kbps] : sim.kbps_log) {
        if (t > failed_at + 1. && t < failed_at + 19.) {
          top = std::max(top, kbps);
        }
      }
      EXPECT_LT(top, failed_kbps) << what;
      // A probe past the knee that builds a lossless queue is cut on its RTT
      // (twice at most): a failure without the lost frames of one.
      std::size_t failures = 0;
      std::size_t loud = 0;
      for (const auto &[t, c] : sim.changes) {
        if (t > 2. && (c.kind == change_kind_e::probe_failed || c.kind == change_kind_e::step_down)) {
          ++failures;
          loud += std::string_view {c.reason} == "queue" ? 0 : 1;
        }
      }
      EXPECT_LE(loud, 3u) << what;
      EXPECT_LE(failures, 5u) << what;
      EXPECT_LT(sim.frame_loss(3., 60.), 0.02) << what;
    }
  }

  TEST(VideoLinkRateSim, ALinkThatImprovesClearsTheKneeAndTheRateClimbsBack) {
    // 50 Mbps for 15 s, then 150: the probe that failed at 50 keeps the stream
    // under it for the quiet hold, then the usual climb finds the room.
    for (const double feedback : {20., 80.}) {
      sim_t sim {queued(link_t::with_rate([](double t) {
                          return t < 15. ? 50e6 : 150e6;
                        }),
                        50.),
                 60'000,
                 feedback};
      sim.run(75.);
      const auto what = "fb " + std::to_string(feedback) + ": " + sim.describe();
      const auto back = sim.reaches(15., 54'000);
      ASSERT_TRUE(back) << what;
      EXPECT_LE(*back, 40.) << what;
      EXPECT_LT(sim.frame_loss(40., 75.), 0.005) << what;
    }
  }

  // ---- parts ----

  TEST(VideoLinkRateBudget, AnEncoderBitrateSurvivesTheRoundTripThroughTheWireRate) {
    const auto b = budget();
    for (const std::uint32_t kbps : {2'000u, 14'988u, 40'000u, 150'000u}) {
      const auto total = b.total_bps(kbps, fec, framerate);
      EXPECT_GT(total, b.video_bps(kbps, fec, framerate) + b.audio_bps);
      EXPECT_GE(b.encoder_kbps(total, max_kbps, fec, framerate), kbps);
      EXPECT_LE(b.encoder_kbps(total, kbps, fec, framerate), kbps) << "held to the ceiling";
    }
    EXPECT_EQ(b.encoder_kbps(0, max_kbps, fec, framerate), 1u);
    // More FEC leaves less for video.
    EXPECT_LT(b.encoder_kbps(b.total_bps(40'000, 20, framerate), max_kbps, 30, framerate), 40'000u);
  }

  TEST(VideoLinkRateBudget, TheAudioReserveFollowsTheNegotiatedChannels) {
    EXPECT_EQ(wire_budget_t::audio_reserve_bps(2, false), 2u * 96 * 1500 + 500'000);
    EXPECT_EQ(wire_budget_t::audio_reserve_bps(8, true), 8u * 256 * 1500 + 500'000);
    EXPECT_EQ(wire_budget_t::audio_reserve_bps(0, true), 500'000u);
  }

  TEST(VideoLinkRateBusy, BusyNeedsSixtyPercentOfTheFramesAndTheBytes) {
    busy_meter_t meter;
    const auto t0 = clock::now();
    EXPECT_FALSE(meter.update(t0, 0, 0, 60, 20'000));
    // 500 ms at 60 fps and 20 Mbps: 30 frames and 1.25 MB expected.
    EXPECT_TRUE(meter.update(t0 + 500ms, 30, 1'250'000, 60, 20'000));
    // A static desktop: every frame, almost no bytes.
    EXPECT_FALSE(meter.update(t0 + 1'000ms, 60, 1'260'000, 60, 20'000));
    // Under 500 ms keeps the last answer.
    EXPECT_FALSE(meter.update(t0 + 1'200ms, 1'000, 100'000'000, 60, 20'000));
    // A sample older than 2 s says nothing.
    EXPECT_FALSE(meter.update(t0 + 4'000ms, 1'000, 100'000'000, 60, 20'000));
  }

  TEST(VideoLinkRatePing, OnlyAStandardPingIsFreshFeedback) {
    // Without fresh pings it never probes; a short or odd payload is no ping.
    for (const std::string payload : {std::string {"\x04\0\0\0", 4}, std::string {"\x05\0\0\0\0\0\0\0", 8}}) {
      auto lr = make(40'000);
      const auto t0 = clock::now();
      std::uint32_t frame = 0;
      std::uint32_t kbps = 40'000;
      std::size_t probes = 0;
      for (std::uint64_t ms = 1; ms < 20'000; ++ms) {
        const auto t = t0 + ms_t(ms);
        if (ms % period_ms == 0) {
          ++frame;
          lr.on_sent(frame, frame_packets * kbps / 40'000, frame_wire_bytes * kbps / 40'000, t);
          if (ms > 2'000 && ms < 2'060) {
            unrecoverable(lr, frame, t);
          }
        }
        if (ms % 100 == 0) {
          lr.on_ping(t, payload);
        }
        if (ms % 8 == 0) {
          if (const auto next = lr.tick(t, 20ms, fec, framerate, kbps, true)) {
            kbps = *next;
          }
        }
        if (const auto change = lr.take_change()) {
          probes += change->kind == change_kind_e::probe ? 1 : 0;
        }
      }
      EXPECT_EQ(lr.state(), state_e::limited);
      EXPECT_EQ(probes, 0u);
    }
  }
}  // namespace
