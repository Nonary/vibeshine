/**
 * @file src/platform/windows/hdr_stats_d3d11.h
 * @brief The Direct3D 11 statistics pass behind HDR10+ dynamic metadata.
 *
 * hdr_stats_cs.hlsl histograms PQ(maxRGB) of the frame a conversion read, on the device
 * context the conversion used, and the readout is copied into one of a ring of staging
 * buffers. poll() maps only finished buffers (D3D11_MAP_FLAG_DO_NOT_WAIT), so nothing on
 * the capture-to-send path ever waits for the pass: a frame whose statistics are late keeps
 * the previous ones current.
 *
 * The pass follows Foundation Sunshine's hdr_luminance_analysis_cs.hlsl
 * (https://github.com/AlkaidLab/foundation-sunshine, commit 880f1ef).
 * Copyright (c) Foundation Sunshine authors, GPL-3.0.
 *
 * This file has no Sunshine dependencies besides src/hdr_dynamic_metadata.h so a standalone
 * test can drive it. src/platform/windows/display_vram.cpp wires it to the encode devices.
 */
#pragma once

// standard includes
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// platform includes
#include <d3d11.h>
#include <wrl/client.h>

// local includes
#include "src/hdr_dynamic_metadata.h"

namespace platf::dxgi {
  class hdr_stats_d3d11_t {
  public:
    /// The converters' transfer functions, as hdr_stats_cs.hlsl numbers them.
    enum class transfer_e : std::uint32_t {
      scrgb = 0,  ///< FP16 scRGB to Rec. 2100 PQ, after `scale`.
      sdr_to_pq = 1,  ///< UNORM sRGB with SDR white at `sdr_white_nits`.
      encoded = 2,  ///< UNORM values that already are the signal.
    };

    /// The frame a pass measures, as the conversion read it.
    struct source_t {
      ID3D11ShaderResourceView *view = nullptr;
      std::uint32_t width = 0;
      std::uint32_t height = 0;
      transfer_e transfer = transfer_e::scrgb;
      float scale = 1.0f;  ///< scRGB multiplier (TrueHDR peak expansion).
      float sdr_white_nits = 100.0f;
    };

    /// One finished pass.
    struct readout_t {
      std::int64_t frame_index = 0;
      ::video::hdr_dynamic::frame_stats_t stats;
    };

    /// Readback buffers in flight at most. A frame that finds them all busy goes unmeasured.
    static constexpr std::size_t ring_size = 4;

    /**
     * @brief Create the pass on `device` from hdr_stats_cs.hlsl compiled for cs_5_0.
     */
    static std::expected<hdr_stats_d3d11_t, std::string> create(ID3D11Device *device, const void *shader, std::size_t shader_size);

    /**
     * @brief Record the statistics pass of `source` and its readback copy on `context`.
     *        Nothing is submitted or waited for.
     * @param frame_index The frame the encoder took these pixels as; when it is not known yet,
     *        tag() names it. A pass still waiting for its tag is dropped by the next queue().
     * @return False, with nothing queued, when every readback buffer is still in flight.
     */
    bool queue(ID3D11DeviceContext *context, const source_t &source, std::optional<std::int64_t> frame_index = std::nullopt);

    /**
     * @brief Name the frame of the last pass queued without one.
     * @return False when no pass waits for its frame.
     */
    bool tag(std::int64_t frame_index);

    /**
     * @brief Drop a pass still waiting for its tag: the pixels it read will not be encoded.
     */
    void drop_untagged();

    /**
     * @brief The readouts that have finished since the last call, oldest first. Never waits.
     * @return An error when a readback cannot be mapped or disagrees with itself.
     */
    std::expected<std::vector<readout_t>, std::string> poll(ID3D11DeviceContext *context);

    /**
     * @brief Passes queued and not read back yet.
     */
    std::size_t in_flight() const;

  private:
    enum class state_e {
      free,
      untagged,  ///< Waiting for tag().
      tagged,
      dropped,  ///< Never tagged; read back only to free the buffer.
    };

    struct slot_t {
      Microsoft::WRL::ComPtr<ID3D11Buffer> staging;
      std::int64_t frame_index = 0;
      std::uint64_t samples = 0;
      state_e state = state_e::free;
    };

    Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> params;
    Microsoft::WRL::ComPtr<ID3D11Buffer> words;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> words_uav;
    std::array<slot_t, ring_size> ring;
    std::size_t next = 0;  ///< The slot the next pass writes; the oldest in flight when busy.
  };

  /**
   * @brief hdr_stats_cs.hlsl at `path`, compiled for cs_5_0; kept for later streams.
   */
  std::expected<Microsoft::WRL::ComPtr<ID3DBlob>, std::string> compile_hdr_stats_shader(const std::filesystem::path &path);

  /**
   * @brief One encode device's statistics pass for the HDR10+ metadata of its stream.
   * @details A source the conversion reads under the capture texture's keyed mutex is
   *          measured inside that same hold: converted() records the pass right after the
   *          conversion's draw, before the mutex is released, and submitted() names its frame
   *          once the encoder took it. The keyed mutex is taken once per frame, by the
   *          conversion, never again for the statistics. A source the device owns (the
   *          TrueHDR output) is measured after the encoder submit instead.
   *
   *          Nothing is compiled or created until a stream that carries the metadata is
   *          attached, at its start; the compiled shader is kept for later streams. A failure
   *          then, or later, stops the stream's metadata, never its frames.
   */
  class hdr_stats_session_t {
  public:
    explicit hdr_stats_session_t(std::filesystem::path shader_path);

    /**
     * @brief Measure frames for `stream` from now on, creating the pass on `device`; null
     *        measures none.
     * @return Why the pass could not be made; nothing is measured then.
     */
    std::optional<std::string> attach(ID3D11Device *device, std::shared_ptr<::video::hdr_dynamic::stream_t> stream);

    /**
     * @brief Call from the conversion, after its draw read `source`.
     * @param held The conversion holds the keyed mutex `source` is read under: the pass is
     *        recorded now. Otherwise `source` is the device's own and is measured after the
     *        encoder submit.
     */
    void converted(ID3D11DeviceContext *context, const hdr_stats_d3d11_t::source_t &source, bool held);

    /**
     * @brief Call from a conversion whose source is not measured, such as a blank frame: it
     *        goes unmeasured, and no pass from an earlier conversion is named after it.
     */
    void converted_unmeasured();

    /**
     * @brief Call once the encoder took the last converted frame as `frame_index`. Never waits.
     * @return Why the stream's metadata stopped, when it stopped now.
     */
    std::optional<std::string> submitted(ID3D11DeviceContext *context, std::int64_t frame_index);

    /**
     * @brief The pass exists: a stream is attached.
     */
    bool has_pass() const {
      return pass.has_value();
    }

    std::size_t in_flight() const {
      return pass ? pass->in_flight() : 0;
    }

    /**
     * @brief A stream is attached with its pass and its metadata has not stopped.
     */
    bool active() const;

  private:
    struct deferred_t {
      Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
      hdr_stats_d3d11_t::source_t source;
    };

    std::string stop(std::string reason);

    std::filesystem::path shader_path;
    std::shared_ptr<::video::hdr_dynamic::stream_t> stream;
    std::optional<hdr_stats_d3d11_t> pass;
    std::optional<deferred_t> deferred;
  };
}  // namespace platf::dxgi
