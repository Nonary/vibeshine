/**
 * @file src/hdr_dynamic_metadata.cpp
 * @brief Per-frame HDR10+ (SMPTE ST 2094-40) dynamic metadata.
 * @details Ported from Foundation Sunshine
 *          (https://github.com/AlkaidLab/foundation-sunshine, commit 880f1ef:
 *          video_hdr_metadata.cpp and video_hdr_bitstream.cpp).
 *          Copyright (c) Foundation Sunshine authors, GPL-3.0.
 */
#include "hdr_dynamic_metadata.h"

// standard includes
#include <algorithm>
#include <bit>
#include <cmath>
#include <initializer_list>
#include <numeric>
#include <sstream>

namespace video::hdr_dynamic {
  namespace {
    constexpr std::size_t p10 = 2;
    constexpr std::size_t p90 = 6;

    // ST 2094-40 8.5.4 reserves the 5% and 10% slots in application version 1 at
    // V1 = 0.00000 and V2 = 0.00255. A consumer that finds a nonzero V1 reads it as the
    // frame peak, so a real 5th percentile there would report the darkest part of the
    // frame as its peak. V1 = 0 tells it to use maxSCL.
    constexpr std::array<std::pair<std::size_t, std::uint32_t>, 2> reserved {{{1, 0}, {2, 255}}};

    constexpr double pq_m1 = 2610.0 / 16384.0;
    constexpr double pq_m2 = 2523.0 / 4096.0 * 128.0;
    constexpr double pq_c1 = 3424.0 / 4096.0;
    constexpr double pq_c2 = 2413.0 / 4096.0 * 32.0;
    constexpr double pq_c3 = 2392.0 / 4096.0 * 32.0;

    /// Luminance fields are 17 bits in 1/100000 of 10000 nits.
    constexpr std::uint32_t max_luminance_field = 100000;

    constexpr std::uint8_t hevc_prefix_sei = 39;
    constexpr std::uint8_t hevc_last_vcl = 31;
    constexpr std::size_t sei_user_data_registered_itu_t_t35 = 4;
    constexpr std::uint8_t obu_frame_header = 3;
    constexpr std::uint8_t obu_tile_group = 4;
    constexpr std::uint8_t obu_metadata = 5;
    constexpr std::uint8_t obu_frame = 6;
    constexpr std::uint8_t obu_redundant_frame_header = 7;
    constexpr std::size_t metadata_type_itut_t35 = 4;

    bool unit_range(float value) {
      return value >= 0.0f && value <= 1.0f;
    }

    /// MSB-first bit writer; the last byte is padded with zeros.
    class bit_writer_t {
    public:
      explicit bit_writer_t(std::initializer_list<std::uint8_t> prefix):
          bytes {prefix} {
      }

      void put(unsigned count, std::uint32_t value) {
        for (unsigned shift = count; shift-- > 0;) {
          if (used == 0) {
            bytes.push_back(0);
          }
          bytes.back() |= static_cast<std::uint8_t>(((value >> shift) & 1u) << (7 - used));
          used = (used + 1) % 8;
        }
      }

      std::vector<std::uint8_t> bytes;

    private:
      unsigned used = 0;
    };

    void push_ff_coded(std::size_t value, std::vector<std::uint8_t> &out) {
      while (value >= 0xFF) {
        out.push_back(0xFF);
        value -= 0xFF;
      }
      out.push_back(static_cast<std::uint8_t>(value));
    }

    void push_leb128(std::size_t value, std::vector<std::uint8_t> &out) {
      while (true) {
        const auto byte = static_cast<std::uint8_t>(value & 0x7F);
        value >>= 7;
        if (value == 0) {
          out.push_back(byte);
          return;
        }
        out.push_back(byte | 0x80);
      }
    }

    std::optional<std::uint64_t> read_leb128(std::span<const std::uint8_t> data, std::size_t &at) {
      std::uint64_t value = 0;
      for (unsigned index = 0; index < 8; ++index) {
        if (at >= data.size()) {
          return std::nullopt;
        }
        const auto byte = data[at++];
        value |= static_cast<std::uint64_t>(byte & 0x7F) << (index * 7);
        if (!(byte & 0x80)) {
          return value;
        }
      }
      return std::nullopt;
    }

    std::optional<std::size_t> find_start_code(std::span<const std::uint8_t> data, std::size_t from) {
      for (std::size_t at = from; at + 3 <= data.size(); ++at) {
        if (data[at] == 0 && data[at + 1] == 0 && data[at + 2] == 1) {
          return at;
        }
      }
      return std::nullopt;
    }

    std::optional<std::size_t> hevc_insert_offset(std::span<const std::uint8_t> frame) {
      std::size_t from = 0;
      while (const auto code = find_start_code(frame, from)) {
        if (*code + 3 >= frame.size()) {
          return std::nullopt;
        }
        const auto header = frame[*code + 3];
        if (header & 0x80) {
          return std::nullopt;  // forbidden_zero_bit: not an HEVC NAL unit
        }
        if (((header >> 1) & 0x3F) <= hevc_last_vcl) {
          // Leading zero_byte included, so the slice keeps its four-byte start code.
          auto at = *code;
          while (at > 0 && frame[at - 1] == 0) {
            --at;
          }
          return at;
        }
        from = *code + 4;
      }
      return std::nullopt;
    }

    std::optional<std::size_t> av1_insert_offset(std::span<const std::uint8_t> frame) {
      std::size_t at = 0;
      while (at < frame.size()) {
        const auto header = frame[at];
        if (header & 0x80) {
          return std::nullopt;  // obu_forbidden_bit
        }
        const auto type = static_cast<std::uint8_t>((header >> 3) & 0x0F);
        if (type == obu_frame_header || type == obu_tile_group || type == obu_frame || type == obu_redundant_frame_header) {
          return at;
        }
        if (!(header & 0x02)) {
          return std::nullopt;  // no obu_has_size_field: the sizes live in a container
        }
        auto next = at + 1 + ((header & 0x04) ? 1 : 0);
        const auto size = read_leb128(frame, next);
        if (!size || *size > frame.size() - std::min(next, frame.size())) {
          return std::nullopt;
        }
        at = next + static_cast<std::size_t>(*size);
      }
      return std::nullopt;
    }

    /// A cut needs the mean, the low or high end, or the whole distribution to move in PQ.
    /// A lone peak is not enough: cursors, subtitles and compositor overshoot all make one.
    bool is_scene_cut(const frame_stats_t &previous, const frame_stats_t &current) {
      const auto delta = [&](std::size_t slot) {
        return std::abs(current.percentiles_pq[slot] - previous.percentiles_pq[slot]);
      };
      const float mean = std::abs(current.mean_pq - previous.mean_pq);
      const float ends = std::max(delta(p10), delta(p90));
      float sum = 0.0f;
      float largest = 0.0f;
      for (std::size_t slot = 0; slot < percentages.size(); ++slot) {
        sum += delta(slot);
        largest = std::max(largest, delta(slot));
      }
      const float spread = sum / static_cast<float>(percentages.size());
      return mean >= 0.10f ||
             ends >= 0.18f ||
             (spread >= 0.10f && largest >= 0.14f) ||
             (mean >= 0.06f && spread >= 0.06f);
    }

    frame_stats_t blend(const frame_stats_t &previous, const frame_stats_t &raw, float alpha) {
      const auto mix = [alpha](float old_value, float new_value) {
        return old_value + alpha * (new_value - old_value);
      };
      frame_stats_t stats;
      stats.min_pq = mix(previous.min_pq, raw.min_pq);
      stats.mean_pq = mix(previous.mean_pq, raw.mean_pq);
      // Not smoothed: maxSCL carries each sample's own maximum, up and down.
      stats.max_pq = raw.max_pq;
      stats.mean_nits = mix(previous.mean_nits, raw.mean_nits);
      for (std::size_t slot = 0; slot < percentages.size(); ++slot) {
        stats.percentiles_pq[slot] = mix(previous.percentiles_pq[slot], raw.percentiles_pq[slot]);
      }
      return stats;
    }

    std::vector<std::uint8_t> stand_in_units(codec_e codec, const static_levels_t &levels) {
      // Static levels always make valid statistics.
      return frame_units(codec, frame_stats_t::from_static(levels)).value_or(std::vector<std::uint8_t> {});
    }
  }  // namespace

  float nits_to_pq(float nits) {
    if (std::isnan(nits) || nits <= 0.0f) {
      return 0.0f;
    }
    const double y = std::pow(std::min(static_cast<double>(nits) / 10000.0, 1.0), pq_m1);
    return static_cast<float>(std::pow((pq_c1 + pq_c2 * y) / (1.0 + pq_c3 * y), pq_m2));
  }

  float pq_to_nits(float pq) {
    if (std::isnan(pq) || pq <= 0.0f) {
      return 0.0f;
    }
    const double e = std::pow(std::min(static_cast<double>(pq), 1.0), 1.0 / pq_m2);
    const double y = std::max(e - pq_c1, 0.0) / (pq_c2 - pq_c3 * e);
    return static_cast<float>(10000.0 * std::pow(y, 1.0 / pq_m1));
  }

  std::optional<frame_stats_t> frame_stats_t::from_histogram(std::span<const std::uint32_t> bins, float min_pq, float max_pq) {
    const std::uint64_t total = std::accumulate(bins.begin(), bins.end(), std::uint64_t {0});
    if (total == 0 || !unit_range(min_pq) || !(max_pq >= min_pq && max_pq <= 1.0f)) {
      return std::nullopt;
    }
    const double width = 1.0 / static_cast<double>(bins.size());
    double pq_sum = 0.0;
    double nits_sum = 0.0;
    for (std::size_t bin = 0; bin < bins.size(); ++bin) {
      if (bins[bin] == 0) {
        continue;
      }
      const double centre = (static_cast<double>(bin) + 0.5) * width;
      pq_sum += centre * bins[bin];
      nits_sum += static_cast<double>(pq_to_nits(static_cast<float>(centre))) * bins[bin];
    }

    frame_stats_t stats;
    std::size_t bin = 0;
    std::uint64_t below = 0;
    for (std::size_t slot = 0; slot < percentages.size(); ++slot) {
      const std::uint64_t rank = (total * percentages[slot] + 99) / 100;
      while (below + bins[bin] < rank) {
        below += bins[bin];
        ++bin;
      }
      stats.percentiles_pq[slot] = std::clamp(static_cast<float>(static_cast<double>(bin + 1) * width), min_pq, max_pq);
    }
    stats.min_pq = min_pq;
    stats.max_pq = max_pq;
    stats.mean_pq = std::clamp(static_cast<float>(pq_sum / static_cast<double>(total)), min_pq, max_pq);
    stats.mean_nits = std::clamp(static_cast<float>(nits_sum / static_cast<double>(total)), pq_to_nits(min_pq), pq_to_nits(max_pq));
    return stats;
  }

  frame_stats_t frame_stats_t::from_static(const static_levels_t &levels) {
    float peak = 1000.0f;
    if (levels.max_cll > 0) {
      peak = std::min(static_cast<float>(levels.max_cll), 10000.0f);
    } else if (levels.max_display_nits > 0) {
      peak = std::min(static_cast<float>(levels.max_display_nits), 10000.0f);
    }

    frame_stats_t stats;
    stats.max_pq = nits_to_pq(peak);
    for (std::size_t slot = 0; slot < percentages.size(); ++slot) {
      stats.percentiles_pq[slot] = stats.max_pq * std::min(static_cast<float>(percentages[slot]) / 99.0f, 1.0f);
    }
    stats.mean_pq = stats.max_pq / 2.0f;

    // The linear mean of the even PQ spread, from 64 equal steps.
    float spread_nits = 0.0f;
    for (int step = 0; step < 64; ++step) {
      spread_nits += pq_to_nits(stats.max_pq * (static_cast<float>(step) + 0.5f) / 64.0f);
    }
    spread_nits /= 64.0f;
    const auto fall = static_cast<float>(levels.max_fall);
    stats.mean_nits = fall > 0.0f && fall <= peak ? fall : spread_nits;
    return stats;
  }

  bool frame_stats_t::is_valid() const {
    return unit_range(min_pq) &&
           unit_range(mean_pq) &&
           unit_range(max_pq) &&
           std::all_of(percentiles_pq.begin(), percentiles_pq.end(), unit_range) &&
           mean_nits >= 0.0f && mean_nits <= 10000.0f;
  }

  std::optional<hdr10plus_t> hdr10plus_t::from_stats(const frame_stats_t &stats) {
    if (!stats.is_valid()) {
      return std::nullopt;
    }
    const auto normalize = [](float nits) {
      return static_cast<std::uint32_t>(std::lround(std::clamp(nits / 10000.0f, 0.0f, 1.0f) * 100000.0f));
    };

    hdr10plus_t metadata;
    for (std::size_t slot = 0; slot < percentages.size(); ++slot) {
      metadata.distribution[slot] = normalize(pq_to_nits(stats.percentiles_pq[slot]));
    }
    metadata.average_maxrgb = normalize(stats.mean_nits);
    // ST 2094-40 maxSCL is each component's maximum. The pass measures maxRGB, so the
    // sample's own maximum (stats_smoother_t leaves it unsmoothed) goes into all three:
    // exact for the brightest component and never below the others, so no sampled pixel
    // sits above it. It is raised only where the smoothed average or a smoothed percentile
    // carried beside it is higher, as while a scene darkens between cuts; the reserved slots
    // are not measurements.
    metadata.maxscl = std::max(normalize(pq_to_nits(stats.max_pq)), metadata.average_maxrgb);
    for (std::size_t slot = 0; slot < percentages.size(); ++slot) {
      const bool is_reserved = std::any_of(reserved.begin(), reserved.end(), [slot](const auto &entry) {
        return entry.first == slot;
      });
      if (!is_reserved) {
        metadata.maxscl = std::max(metadata.maxscl, metadata.distribution[slot]);
      }
    }
    for (const auto &[slot, value] : reserved) {
      metadata.distribution[slot] = value;
    }
    return metadata;
  }

  std::vector<std::uint8_t> hdr10plus_t::to_t35() const {
    // Country US, provider 0x003C, provider-oriented code 0x0001, application
    // identifier 4 (ST 2094-40), application version 1.
    bit_writer_t bits {0xB5, 0x00, 0x3C, 0x00, 0x01, 4, 1};
    bits.put(2, 1);  // num_windows
    bits.put(27, 0);  // targeted_system_display_maximum_luminance: profile A has none
    bits.put(1, 0);  // targeted_system_display_actual_peak_luminance_flag
    for (int component = 0; component < 3; ++component) {
      bits.put(17, std::min(maxscl, max_luminance_field));
    }
    bits.put(17, std::min(average_maxrgb, max_luminance_field));
    bits.put(4, static_cast<std::uint32_t>(percentages.size()));
    for (std::size_t slot = 0; slot < percentages.size(); ++slot) {
      bits.put(7, percentages[slot]);
      bits.put(17, std::min(distribution[slot], max_luminance_field));
    }
    bits.put(10, 0);  // fraction_bright_pixels
    bits.put(1, 0);  // mastering_display_actual_peak_luminance_flag
    bits.put(1, 0);  // tone_mapping_flag: profile A carries no Bezier curve
    bits.put(1, 0);  // color_saturation_mapping_flag
    return std::move(bits.bytes);
  }

  std::optional<codec_e> codec_for(int video_format) {
    switch (video_format) {
      case 1:
        return codec_e::hevc;
      case 2:
        return codec_e::av1;
      default:
        return std::nullopt;
    }
  }

  void escape(std::span<const std::uint8_t> rbsp, std::vector<std::uint8_t> &out) {
    int zeros = 0;
    for (const auto byte : rbsp) {
      if (zeros == 2 && byte <= 3) {
        out.push_back(3);
        zeros = 0;
      }
      out.push_back(byte);
      zeros = byte == 0 ? zeros + 1 : 0;
    }
  }

  std::vector<std::uint8_t> hevc_t35_sei(std::span<const std::uint8_t> t35) {
    std::vector<std::uint8_t> rbsp;
    rbsp.reserve(t35.size() + 8);
    push_ff_coded(sei_user_data_registered_itu_t_t35, rbsp);
    push_ff_coded(t35.size(), rbsp);
    rbsp.insert(rbsp.end(), t35.begin(), t35.end());
    rbsp.push_back(0x80);  // rbsp_trailing_bits
    std::vector<std::uint8_t> nal {0, 0, 0, 1, static_cast<std::uint8_t>(hevc_prefix_sei << 1), 0x01};
    escape(rbsp, nal);
    return nal;
  }

  std::vector<std::uint8_t> av1_t35_obu(std::span<const std::uint8_t> t35) {
    std::vector<std::uint8_t> payload;
    payload.reserve(t35.size() + 2);
    push_leb128(metadata_type_itut_t35, payload);
    payload.insert(payload.end(), t35.begin(), t35.end());
    payload.push_back(0x80);  // trailing_bits
    std::vector<std::uint8_t> obu {static_cast<std::uint8_t>((obu_metadata << 3) | 0x02)};
    push_leb128(payload.size(), obu);
    obu.insert(obu.end(), payload.begin(), payload.end());
    return obu;
  }

  std::vector<std::uint8_t> t35_unit(codec_e codec, std::span<const std::uint8_t> t35) {
    return codec == codec_e::hevc ? hevc_t35_sei(t35) : av1_t35_obu(t35);
  }

  std::optional<std::vector<std::uint8_t>> frame_units(codec_e codec, const frame_stats_t &stats) {
    const auto hdr10plus = hdr10plus_t::from_stats(stats);
    if (!hdr10plus) {
      return std::nullopt;
    }
    return t35_unit(codec, hdr10plus->to_t35());
  }

  std::optional<std::size_t> insert_offset(codec_e codec, std::span<const std::uint8_t> frame) {
    return codec == codec_e::hevc ? hevc_insert_offset(frame) : av1_insert_offset(frame);
  }

  bool splice(codec_e codec, std::vector<std::uint8_t> &frame, std::span<const std::uint8_t> units) {
    if (units.empty()) {
      return false;
    }
    const auto at = insert_offset(codec, frame);
    if (!at) {
      return false;
    }
    frame.insert(frame.begin() + static_cast<std::ptrdiff_t>(*at), units.begin(), units.end());
    return true;
  }

  std::optional<std::vector<std::uint8_t>> spliced(codec_e codec, std::span<const std::uint8_t> frame, std::span<const std::uint8_t> units) {
    if (units.empty()) {
      return std::nullopt;
    }
    const auto at = insert_offset(codec, frame);
    if (!at) {
      return std::nullopt;
    }
    std::vector<std::uint8_t> out;
    out.reserve(frame.size() + units.size());
    out.insert(out.end(), frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(*at));
    out.insert(out.end(), units.begin(), units.end());
    out.insert(out.end(), frame.begin() + static_cast<std::ptrdiff_t>(*at), frame.end());
    return out;
  }

  std::pair<std::uint32_t, std::uint32_t> sample_grid(std::uint32_t width, std::uint32_t height) {
    return {std::clamp(width, 1u, max_grid_width), std::clamp(height, 1u, max_grid_height)};
  }

  std::expected<frame_stats_t, std::string> parse_readout(std::span<const std::uint32_t> words, std::uint64_t samples) {
    if (words.size() != readout_words) {
      return std::unexpected("HDR statistics readout has " + std::to_string(words.size()) + " words, not " + std::to_string(readout_words));
    }
    const float min_pq = std::bit_cast<float>(~words[0]);
    const float max_pq = std::bit_cast<float>(words[1]);
    const auto histogram = words.subspan(2);
    const std::uint64_t counted = std::accumulate(histogram.begin(), histogram.end(), std::uint64_t {0});
    // The shader's own binning: scaling by the bin count is exact and the conversion truncates.
    const auto bin_of = [](float pq) {
      return std::min(static_cast<std::size_t>(pq * static_cast<float>(histogram_bins)), histogram_bins - 1);
    };
    const auto first = std::find_if(histogram.begin(), histogram.end(), [](std::uint32_t count) {
      return count > 0;
    });
    const auto last = std::find_if(histogram.rbegin(), histogram.rend(), [](std::uint32_t count) {
      return count > 0;
    });
    const bool consistent = counted == samples &&
                            unit_range(min_pq) &&
                            max_pq >= min_pq && max_pq <= 1.0f &&
                            first != histogram.end() &&
                            static_cast<std::size_t>(first - histogram.begin()) == bin_of(min_pq) &&
                            static_cast<std::size_t>(histogram.rend() - last - 1) == bin_of(max_pq);
    if (!consistent) {
      std::ostringstream error;
      error << "HDR statistics readout is inconsistent: " << counted << " of " << samples
            << " samples, PQ " << min_pq << ".." << max_pq;
      return std::unexpected(error.str());
    }
    auto stats = frame_stats_t::from_histogram(histogram, min_pq, max_pq);
    if (!stats) {
      return std::unexpected("HDR statistics readout makes no statistics");
    }
    return *stats;
  }

  std::optional<stats_smoother_t::smoothed_t> stats_smoother_t::update(const frame_stats_t &raw) {
    if (!raw.is_valid()) {
      return std::nullopt;
    }
    const bool scene_cut = !last || is_scene_cut(*last, raw);
    const frame_stats_t stats = smoothed && !scene_cut ? blend(*smoothed, raw, alpha) : raw;
    last = raw;
    smoothed = stats;
    return smoothed_t {stats, scene_cut};
  }

  void stats_smoother_t::reset() {
    smoothed.reset();
    last.reset();
  }

  writer_t::writer_t(codec_e codec, const static_levels_t &levels):
      codec_ {codec},
      units_ {stand_in_units(codec, levels)} {
  }

  void writer_t::set_static_levels(const static_levels_t &levels) {
    if (!measured_) {
      units_ = stand_in_units(codec_, levels);
    }
  }

  std::optional<stats_smoother_t::smoothed_t> writer_t::sample(const frame_stats_t &raw) {
    const auto smoothed = smoother.update(raw);
    if (!smoothed) {
      return std::nullopt;
    }
    auto units = frame_units(codec_, smoothed->stats);
    if (!units) {
      return std::nullopt;
    }
    units_ = std::move(*units);
    measured_ = true;
    return smoothed;
  }

  stream_t::stream_t(codec_e codec, const static_levels_t &levels):
      codec_ {codec},
      writer {codec, levels} {
  }

  void stream_t::push(std::int64_t frame_index, const frame_stats_t &stats) {
    std::lock_guard lock {mutex};
    if (last_pushed && frame_index <= *last_pushed) {
      return;
    }
    last_pushed = frame_index;
    pending.emplace_back(frame_index, stats);
    if (pending.size() > max_pending) {
      pending.pop_front();
    }
  }

  void stream_t::set_static_levels(const static_levels_t &levels) {
    std::lock_guard lock {mutex};
    writer.set_static_levels(levels);
  }

  void stream_t::stop() {
    std::lock_guard lock {mutex};
    stopped_ = true;
    pending.clear();
  }

  bool stream_t::stopped() const {
    std::lock_guard lock {mutex};
    return stopped_;
  }

  stream_t::splice_e stream_t::prepare(std::int64_t frame_index, std::span<const std::uint8_t> frame, insertion_t &out) {
    if (frame.empty()) {
      return splice_e::empty;
    }
    // Only the frame's headers are read, and outside the lock.
    const auto offset = insert_offset(codec_, frame);

    std::lock_guard lock {mutex};
    if (stopped_) {
      return splice_e::stopped;
    }
    while (!pending.empty() && pending.front().first <= frame_index) {
      if (writer.sample(pending.front().second)) {
        ++counts.samples;
      } else {
        ++counts.invalid_samples;
      }
      pending.pop_front();
    }
    if (!offset || writer.units().empty()) {
      ++counts.no_insert_point;
      return splice_e::no_insert_point;
    }
    out.offset = *offset;
    out.units = writer.units();
    ++counts.frames;
    if (!writer.measured()) {
      ++counts.stand_in;
    }
    return splice_e::spliced;
  }

  stream_t::splice_e stream_t::splice(std::int64_t frame_index, std::span<const std::uint8_t> frame, std::vector<std::uint8_t> &out) {
    insertion_t insertion;
    const auto result = prepare(frame_index, frame, insertion);
    if (result == splice_e::spliced) {
      const auto at = frame.begin() + static_cast<std::ptrdiff_t>(insertion.offset);
      out.clear();
      out.reserve(frame.size() + insertion.units.size());
      out.insert(out.end(), frame.begin(), at);
      out.insert(out.end(), insertion.units.begin(), insertion.units.end());
      out.insert(out.end(), at, frame.end());
    }
    return result;
  }

  stream_t::counters_t stream_t::counters() const {
    std::lock_guard lock {mutex};
    return counts;
  }
}  // namespace video::hdr_dynamic
