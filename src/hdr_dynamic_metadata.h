/**
 * @file src/hdr_dynamic_metadata.h
 * @brief Per-frame HDR10+ (SMPTE ST 2094-40) dynamic metadata: frame statistics, the
 *        registered ITU-T T.35 payload, its carriage as an HEVC prefix SEI or an AV1
 *        metadata OBU, and the splice that puts it in an encoded frame.
 * @details Field semantics follow FFmpeg's parsers (libavutil/hdr_dynamic_metadata.c,
 *          libavcodec/h2645_sei.c, libavcodec/libdav1d.c). The payload writers, the
 *          statistics and the smoothing are ported from Foundation Sunshine
 *          (https://github.com/AlkaidLab/foundation-sunshine, commit 880f1ef:
 *          video_hdr_metadata.h, video_hdr_metadata.cpp and video_hdr_bitstream.cpp).
 *          Copyright (c) Foundation Sunshine authors, GPL-3.0.
 *
 *          Only HDR10+ is written. Other T.35 payloads (HDR Vivid) would go beside it in
 *          frame_units(), which already concatenates units for one splice.
 */
#pragma once

// standard includes
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace video::hdr_dynamic {
  /// The maxRGB percentages HDR10+ carries; frame_stats_t::percentiles_pq lines up with it.
  inline constexpr std::array<std::uint8_t, 9> percentages {1, 5, 10, 25, 50, 75, 90, 95, 99};

  /// Equal PQ bins over 0..1 in the GPU statistics readout.
  inline constexpr std::size_t histogram_bins = 1024;

  /// Words in the GPU statistics readout: the samples' minimum, their maximum, then the bins.
  inline constexpr std::size_t readout_words = 2 + histogram_bins;

  /// The most sample cells across and down: 16x16 pixel cells on a 4K source.
  inline constexpr std::uint32_t max_grid_width = 240;
  inline constexpr std::uint32_t max_grid_height = 135;

  /**
   * @brief SMPTE ST 2084 (PQ) signal, 0..1, for absolute luminance in nits.
   */
  float nits_to_pq(float nits);

  /**
   * @brief Absolute luminance in nits for a PQ signal value.
   */
  float pq_to_nits(float pq);

  /**
   * @brief The stream's static HDR10 light levels, in nits; 0 where unknown.
   */
  struct static_levels_t {
    std::uint16_t max_cll = 0;  ///< MaxCLL.
    std::uint16_t max_display_nits = 0;  ///< The mastering display's peak.
    std::uint16_t max_fall = 0;  ///< MaxFALL.
  };

  /**
   * @brief One frame's maxRGB statistics, as the GPU analysis hands them to the writer.
   * @details Everything but mean_nits is in the PQ signal domain (0..1); HDR10+ converts
   *          back to linear light.
   */
  struct frame_stats_t {
    float min_pq = 0.0f;
    float mean_pq = 0.0f;  ///< Mean of PQ(maxRGB).
    float max_pq = 0.0f;
    /// Linear-light mean of maxRGB in nits, which HDR10+ carries. It differs widely from
    /// the PQ mean on dark frames with small highlights.
    float mean_nits = 0.0f;
    std::array<float, percentages.size()> percentiles_pq {};  ///< maxRGB at each of `percentages`, PQ.

    /**
     * @brief Statistics from a histogram of PQ(maxRGB) in equal bins over 0..1 and the
     *        minimum and maximum of the same samples.
     * @details Means come from bin centres, percentiles from the upper edge of the bin that
     *          reaches them, all kept inside the minimum and maximum. The GPU pass samples a
     *          grid, so a highlight between the samples is missed.
     * @return Empty for an empty histogram or bounds outside 0..1.
     */
    static std::optional<frame_stats_t> from_histogram(std::span<const std::uint32_t> bins, float min_pq, float max_pq);

    /**
     * @brief A stand-in for frames no measurement covers yet, from the static levels.
     * @details maxRGB spread evenly in PQ from black to the content peak (MaxCLL, else the
     *          mastering display's peak, else 1000 nits), with MaxFALL as the linear mean
     *          when it is set and not above the peak. A display that tone-maps from it
     *          treats the frame as it would with static HDR10 alone. The first frames of a
     *          stream leave the encoder before their statistics pass has finished, and
     *          HDR10+ wants metadata on every frame.
     */
    static frame_stats_t from_static(const static_levels_t &levels);

    /**
     * @brief Every value finite and in range: PQ values in 0..1, mean_nits in 0..10000.
     */
    bool is_valid() const;

    bool operator==(const frame_stats_t &) const = default;
  };

  /**
   * @brief One window of HDR10+ profile A metadata as transmitted.
   * @details The frame's statistics with targeted_system_display_maximum_luminance 0 and no
   *          tone-mapping curve, so the display tone-maps for itself. Every luminance field is
   *          normalized to the 10000-nit PQ peak in units of 1/100000.
   */
  struct hdr10plus_t {
    std::uint32_t maxscl = 0;  ///< Carried for all three components.
    std::uint32_t average_maxrgb = 0;
    std::array<std::uint32_t, percentages.size()> distribution {};  ///< distribution_maxrgb at each of `percentages`.

    /**
     * @return Empty for invalid statistics.
     */
    static std::optional<hdr10plus_t> from_stats(const frame_stats_t &stats);

    /**
     * @brief The complete T.35 payload, from the country code on.
     */
    std::vector<std::uint8_t> to_t35() const;

    bool operator==(const hdr10plus_t &) const = default;
  };

  /**
   * @brief The codecs that carry dynamic HDR metadata.
   */
  enum class codec_e {
    hevc,
    av1,
  };

  /**
   * @brief The codec of a `config_t::videoFormat` (1 HEVC, 2 AV1). H.264 gets none.
   */
  std::optional<codec_e> codec_for(int video_format);

  /**
   * @brief An Annex B prefix SEI NAL unit (four-byte start code, layer 0, TemporalId 0)
   *        holding one user_data_registered_itu_t_t35 message, emulation prevention applied.
   */
  std::vector<std::uint8_t> hevc_t35_sei(std::span<const std::uint8_t> t35);

  /**
   * @brief An AV1 metadata OBU (metadata_type ITU-T T.35) with a size field and no extension.
   */
  std::vector<std::uint8_t> av1_t35_obu(std::span<const std::uint8_t> t35);

  /**
   * @brief One T.35 payload in the codec's carriage.
   */
  std::vector<std::uint8_t> t35_unit(codec_e codec, std::span<const std::uint8_t> t35);

  /**
   * @brief Every dynamic-metadata unit for one frame, concatenated for one splice.
   * @return Empty for invalid statistics.
   */
  std::optional<std::vector<std::uint8_t>> frame_units(codec_e codec, const frame_stats_t &stats);

  /**
   * @brief Emulation prevention: an 0x03 after every two zero bytes that precede a byte <= 3.
   */
  void escape(std::span<const std::uint8_t> rbsp, std::vector<std::uint8_t> &out);

  /**
   * @brief Where metadata units belong in one encoded frame.
   * @details HEVC (Annex B): before the first VCL NAL's start code, after any AUD, parameter
   *          sets and prefix SEIs. AV1 (OBUs with size fields): before the first frame, frame
   *          header or tile group OBU, after the temporal delimiter and sequence header.
   * @return Empty when the frame has no such point or cannot be walked (no start codes,
   *         forbidden bit set, OBUs without sizes, truncated sizes).
   */
  std::optional<std::size_t> insert_offset(codec_e codec, std::span<const std::uint8_t> frame);

  /**
   * @brief Insert `units` at insert_offset().
   * @return False, with `frame` unchanged, when there is no insertion point or nothing to insert.
   */
  bool splice(codec_e codec, std::vector<std::uint8_t> &frame, std::span<const std::uint8_t> units);

  /**
   * @brief A copy of `frame` with `units` at insert_offset(), in one allocation.
   * @return Empty when there is no insertion point or nothing to insert.
   */
  std::optional<std::vector<std::uint8_t>> spliced(codec_e codec, std::span<const std::uint8_t> frame, std::span<const std::uint8_t> units);

  /**
   * @brief Sample cells across and down a source, one per pixel on a small one.
   */
  std::pair<std::uint32_t, std::uint32_t> sample_grid(std::uint32_t width, std::uint32_t height);

  /**
   * @brief The statistics of one GPU readout of `samples` grid cells.
   * @details Word 0 is the samples' minimum PQ(maxRGB) as inverted float bits (so a zeroed
   *          buffer finds it with an atomic maximum), word 1 their maximum as float bits,
   *          then histogram_bins counts. A readout that disagrees with itself is an error,
   *          not a frame without statistics.
   */
  std::expected<frame_stats_t, std::string> parse_readout(std::span<const std::uint32_t> words, std::uint64_t samples);

  /**
   * @brief Smooths the statistics over time so the metadata does not step at every sample
   *        and make the picture pump, and snaps to a new scene at a cut. One per stream.
   * @details The maximum is never smoothed: maxSCL follows each sample's own.
   */
  class stats_smoother_t {
  public:
    /// Weight of each new sample between cuts.
    static constexpr float alpha = 0.15f;

    struct smoothed_t {
      frame_stats_t stats;
      bool scene_cut = false;  ///< The first sample or a cut, taken as it is.
    };

    /**
     * @brief Feed one new analysis sample, not one per encoded frame: repeating a sample
     *        would speed the smoothing up.
     * @return Empty for invalid statistics, which leave the state alone.
     */
    std::optional<smoothed_t> update(const frame_stats_t &raw);

    void reset();

  private:
    std::optional<frame_stats_t> smoothed;
    std::optional<frame_stats_t> last;
  };

  /**
   * @brief One stream's units: each new sample, smoothed, becomes the units every frame
   *        carries until the next. Frames before the first valid sample carry the stand-in
   *        of frame_stats_t::from_static().
   */
  class writer_t {
  public:
    explicit writer_t(codec_e codec, const static_levels_t &levels = {});

    codec_e codec() const {
      return codec_;
    }

    /**
     * @brief The stream's static levels. Until the first sample the units are rebuilt from
     *        them; after it the measurements stand.
     */
    void set_static_levels(const static_levels_t &levels);

    /**
     * @brief Feed one new analysis sample.
     * @return Empty for invalid statistics, which keep the current units.
     */
    std::optional<stats_smoother_t::smoothed_t> sample(const frame_stats_t &raw);

    /// The units come from a sample, not the static stand-in.
    bool measured() const {
      return measured_;
    }

    const std::vector<std::uint8_t> &units() const {
      return units_;
    }

  private:
    codec_e codec_;
    stats_smoother_t smoother;
    bool measured_ = false;
    std::vector<std::uint8_t> units_;
  };

  /**
   * @brief One stream's HDR10+ metadata between the statistics pass and the encoded packets.
   * @details The pass queues each sample with the index of the frame it measured; a packet
   *          takes the units of the newest sample measured on its own frame or an earlier
   *          one, so a scene cut never reaches the frames before it. Thread-safe.
   */
  class stream_t {
  public:
    explicit stream_t(codec_e codec, const static_levels_t &levels = {});

    codec_e codec() const {
      return codec_;
    }

    /**
     * @brief Queue the statistics of frame `frame_index`. A sample for a frame no newer than
     *        the last queued one is dropped.
     */
    void push(std::int64_t frame_index, const frame_stats_t &stats);

    /**
     * @brief See writer_t::set_static_levels().
     */
    void set_static_levels(const static_levels_t &levels);

    /**
     * @brief Stop splicing for the rest of the stream, after the statistics pass failed.
     */
    void stop();

    bool stopped() const;

    enum class splice_e {
      spliced,  ///< `out` holds the frame with the units.
      empty,  ///< An empty packet is not a frame.
      no_insert_point,  ///< The frame cannot carry units; it goes out unchanged.
      stopped,  ///< stop() was called; the frame goes out unchanged.
    };

    /**
     * @brief The packet of frame `frame_index` with this stream's units, in `out`.
     */
    splice_e splice(std::int64_t frame_index, std::span<const std::uint8_t> frame, std::vector<std::uint8_t> &out);

    /// Where a frame takes its units, and the units.
    struct insertion_t {
      std::size_t offset = 0;
      std::vector<std::uint8_t> units;
    };

    /**
     * @brief splice() without the copy: the units for frame `frame_index` and the offset in
     *        `frame` they go at, for a caller that builds the packet itself. The frame is
     *        never copied, and only its headers are read.
     */
    splice_e prepare(std::int64_t frame_index, std::span<const std::uint8_t> frame, insertion_t &out);

    struct counters_t {
      std::uint64_t frames = 0;  ///< Frames that carried units.
      std::uint64_t stand_in = 0;  ///< Of those, frames before the first sample.
      std::uint64_t samples = 0;  ///< Samples taken into the units.
      std::uint64_t invalid_samples = 0;  ///< Samples the writer rejected.
      std::uint64_t no_insert_point = 0;  ///< Frames that could not carry units.

      bool operator==(const counters_t &) const = default;
    };

    counters_t counters() const;

    /// Samples waiting for their frame's packet, at most.
    static constexpr std::size_t max_pending = 64;

  private:
    codec_e codec_;
    mutable std::mutex mutex;
    writer_t writer;
    std::deque<std::pair<std::int64_t, frame_stats_t>> pending;
    std::optional<std::int64_t> last_pushed;
    bool stopped_ = false;
    counters_t counts;
  };
}  // namespace video::hdr_dynamic
