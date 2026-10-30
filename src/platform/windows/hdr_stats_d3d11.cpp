/**
 * @file src/platform/windows/hdr_stats_d3d11.cpp
 * @brief The Direct3D 11 statistics pass behind HDR10+ dynamic metadata.
 */
// standard includes
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>

// platform includes
#include <d3dcompiler.h>

// local includes
#include "hdr_stats_d3d11.h"

namespace platf::dxgi {
  namespace {
    namespace hdr = ::video::hdr_dynamic;

    /// stats_params_cbuffer in hdr_stats_cs.hlsl.
    struct alignas(16) params_t {
      std::uint32_t grid[2];
      std::uint32_t source_size[2];
      std::uint32_t transfer;
      float scale;
      float sdr_white_nits;
      float padding;
    };

    static_assert(sizeof(params_t) % 16 == 0, "Constant buffers are sized in 16-byte units");

    constexpr std::uint32_t readout_bytes = sizeof(std::uint32_t) * hdr::readout_words;

    /// The cells of one 16x16 thread group.
    constexpr std::uint32_t group_size = 16;

    std::string failed(const char *what, HRESULT status) {
      char code[16];
      std::snprintf(code, sizeof(code), "0x%08lx", static_cast<unsigned long>(status));
      return std::string {what} + " [" + code + ']';
    }
  }  // namespace

  std::expected<hdr_stats_d3d11_t, std::string> hdr_stats_d3d11_t::create(ID3D11Device *device, const void *shader_code, std::size_t shader_size) {
    if (!device || !shader_code || shader_size == 0) {
      return std::unexpected("HDR statistics pass needs a device and a compiled shader");
    }

    hdr_stats_d3d11_t pass;
    auto status = device->CreateComputeShader(shader_code, shader_size, nullptr, &pass.shader);
    if (FAILED(status)) {
      return std::unexpected(failed("Could not create the HDR statistics shader", status));
    }

    D3D11_BUFFER_DESC params_desc {};
    params_desc.ByteWidth = sizeof(params_t);
    params_desc.Usage = D3D11_USAGE_DEFAULT;
    params_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    status = device->CreateBuffer(&params_desc, nullptr, &pass.params);
    if (FAILED(status)) {
      return std::unexpected(failed("Could not create the HDR statistics parameters", status));
    }

    D3D11_BUFFER_DESC words_desc {};
    words_desc.ByteWidth = readout_bytes;
    words_desc.Usage = D3D11_USAGE_DEFAULT;
    words_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    words_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    words_desc.StructureByteStride = sizeof(std::uint32_t);
    status = device->CreateBuffer(&words_desc, nullptr, &pass.words);
    if (FAILED(status)) {
      return std::unexpected(failed("Could not create the HDR statistics buffer", status));
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc {};
    uav_desc.Format = DXGI_FORMAT_UNKNOWN;
    uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav_desc.Buffer.FirstElement = 0;
    uav_desc.Buffer.NumElements = hdr::readout_words;
    status = device->CreateUnorderedAccessView(pass.words.Get(), &uav_desc, &pass.words_uav);
    if (FAILED(status)) {
      return std::unexpected(failed("Could not create the HDR statistics view", status));
    }

    D3D11_BUFFER_DESC staging_desc {};
    staging_desc.ByteWidth = readout_bytes;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto &slot : pass.ring) {
      status = device->CreateBuffer(&staging_desc, nullptr, &slot.staging);
      if (FAILED(status)) {
        return std::unexpected(failed("Could not create an HDR statistics readback buffer", status));
      }
    }

    return pass;
  }

  bool hdr_stats_d3d11_t::queue(ID3D11DeviceContext *context, const source_t &source, std::optional<std::int64_t> frame_index) {
    // A pass still waiting for its frame belongs to a conversion that was never encoded.
    drop_untagged();

    auto &slot = ring[next];
    if (slot.state != state_e::free || !source.view) {
      return false;
    }

    const auto [grid_width, grid_height] = hdr::sample_grid(source.width, source.height);
    const params_t values {
      {grid_width, grid_height},
      {source.width, source.height},
      static_cast<std::uint32_t>(source.transfer),
      source.scale,
      source.sdr_white_nits,
      0.0f,
    };
    context->UpdateSubresource(params.Get(), 0, nullptr, &values, 0, 0);

    const UINT zeros[4] {};
    context->ClearUnorderedAccessViewUint(words_uav.Get(), zeros);

    context->CSSetShader(shader.Get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, params.GetAddressOf());
    context->CSSetShaderResources(0, 1, &source.view);
    context->CSSetUnorderedAccessViews(0, 1, words_uav.GetAddressOf(), nullptr);
    context->Dispatch((grid_width + group_size - 1) / group_size, (grid_height + group_size - 1) / group_size, 1);

    ID3D11ShaderResourceView *no_view = nullptr;
    ID3D11UnorderedAccessView *no_uav = nullptr;
    ID3D11Buffer *no_buffer = nullptr;
    context->CSSetShaderResources(0, 1, &no_view);
    context->CSSetUnorderedAccessViews(0, 1, &no_uav, nullptr);
    context->CSSetConstantBuffers(0, 1, &no_buffer);
    context->CSSetShader(nullptr, nullptr, 0);

    context->CopyResource(slot.staging.Get(), words.Get());

    slot.frame_index = frame_index.value_or(0);
    slot.samples = static_cast<std::uint64_t>(grid_width) * grid_height;
    slot.state = frame_index ? state_e::tagged : state_e::untagged;
    next = (next + 1) % ring.size();
    return true;
  }

  bool hdr_stats_d3d11_t::tag(std::int64_t frame_index) {
    for (auto &slot : ring) {
      if (slot.state == state_e::untagged) {
        slot.frame_index = frame_index;
        slot.state = state_e::tagged;
        return true;
      }
    }
    return false;
  }

  void hdr_stats_d3d11_t::drop_untagged() {
    for (auto &slot : ring) {
      if (slot.state == state_e::untagged) {
        slot.state = state_e::dropped;
      }
    }
  }

  std::expected<std::vector<hdr_stats_d3d11_t::readout_t>, std::string> hdr_stats_d3d11_t::poll(ID3D11DeviceContext *context) {
    std::vector<readout_t> readouts;
    // Slots fill in ring order from `next`, so the oldest in flight comes first, and the
    // GPU finishes them in that order: the first one still running ends the walk.
    for (std::size_t offset = 0; offset < ring.size(); ++offset) {
      auto &slot = ring[(next + offset) % ring.size()];
      if (slot.state == state_e::free) {
        continue;
      }
      if (slot.state == state_e::untagged) {
        break;  // The newest pass; its frame is named after the encoder took it.
      }

      D3D11_MAPPED_SUBRESOURCE mapped {};
      const auto status = context->Map(slot.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
      if (status == DXGI_ERROR_WAS_STILL_DRAWING) {
        break;
      }
      const bool dropped = slot.state == state_e::dropped;
      slot.state = state_e::free;
      if (FAILED(status)) {
        return std::unexpected(failed("Could not map an HDR statistics readback buffer", status));
      }

      std::array<std::uint32_t, hdr::readout_words> copy;
      std::memcpy(copy.data(), mapped.pData, readout_bytes);
      context->Unmap(slot.staging.Get(), 0);
      if (dropped) {
        continue;
      }

      auto stats = hdr::parse_readout(copy, slot.samples);
      if (!stats) {
        return std::unexpected(stats.error());
      }
      readouts.push_back({slot.frame_index, *stats});
    }
    return readouts;
  }

  std::size_t hdr_stats_d3d11_t::in_flight() const {
    std::size_t count = 0;
    for (const auto &slot : ring) {
      count += slot.state != state_e::free ? 1 : 0;
    }
    return count;
  }

  std::expected<Microsoft::WRL::ComPtr<ID3DBlob>, std::string> compile_hdr_stats_shader(const std::filesystem::path &path) {
    static std::mutex mutex;
    static std::map<std::filesystem::path, Microsoft::WRL::ComPtr<ID3DBlob>> compiled;
    std::lock_guard lock {mutex};
    if (const auto it = compiled.find(path); it != compiled.end()) {
      return it->second;
    }

    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifndef NDEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    Microsoft::WRL::ComPtr<ID3DBlob> code;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const auto status = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main_cs", "cs_5_0", flags, 0, &code, &errors);
    if (FAILED(status)) {
      const auto name = path.u8string();
      auto message = failed(("Could not compile " + std::string {name.begin(), name.end()}).c_str(), status);
      if (errors && errors->GetBufferSize() > 0) {
        std::string text {static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize()};
        while (!text.empty() && (text.back() == '\0' || text.back() == '\n' || text.back() == '\r')) {
          text.pop_back();
        }
        message += ": " + text;
      }
      return std::unexpected(std::move(message));
    }
    compiled.emplace(path, code);
    return code;
  }

  hdr_stats_session_t::hdr_stats_session_t(std::filesystem::path shader_path):
      shader_path {std::move(shader_path)} {
  }

  std::optional<std::string> hdr_stats_session_t::attach(ID3D11Device *device, std::shared_ptr<hdr::stream_t> stream_) {
    stream.reset();
    pass.reset();
    deferred.reset();
    if (!stream_) {
      return std::nullopt;
    }

    // At the stream's start, so no frame waits for the compile.
    const auto shader = compile_hdr_stats_shader(shader_path);
    if (!shader) {
      return shader.error();
    }
    auto created = hdr_stats_d3d11_t::create(device, (*shader)->GetBufferPointer(), (*shader)->GetBufferSize());
    if (!created) {
      return created.error();
    }
    pass.emplace(std::move(*created));
    stream = std::move(stream_);
    return std::nullopt;
  }

  bool hdr_stats_session_t::active() const {
    return pass && stream && !stream->stopped();
  }

  std::string hdr_stats_session_t::stop(std::string reason) {
    stream->stop();
    pass.reset();
    deferred.reset();
    return reason;
  }

  void hdr_stats_session_t::converted(ID3D11DeviceContext *context, const hdr_stats_d3d11_t::source_t &source, bool held) {
    if (!active() || !source.view) {
      converted_unmeasured();
      return;
    }
    deferred.reset();
    if (!held) {
      // Measured after the submit instead of any pass an earlier conversion left waiting.
      pass->drop_untagged();
      deferred = deferred_t {source.view, source};
      return;
    }
    // Only recorded, right behind the conversion's draw; nothing is submitted or waited for.
    pass->queue(context, source);
  }

  void hdr_stats_session_t::converted_unmeasured() {
    deferred.reset();
    if (pass) {
      pass->drop_untagged();
    }
  }

  std::optional<std::string> hdr_stats_session_t::submitted(ID3D11DeviceContext *context, std::int64_t frame_index) {
    const auto source = std::exchange(deferred, std::nullopt);
    if (!active()) {
      return std::nullopt;
    }

    // A device-owned source is recorded now; a pass recorded during the conversion is named.
    const bool queued = source ? pass->queue(context, source->source, frame_index) : pass->tag(frame_index);
    if (queued) {
      // The encoder already has its frame; this only gets the pass to the GPU before the
      // next conversion, so the readback is ready a frame or two later.
      context->Flush();
    }

    auto readouts = pass->poll(context);
    if (!readouts) {
      return stop(readouts.error());
    }
    for (const auto &readout : *readouts) {
      stream->push(readout.frame_index, readout.stats);
    }
    return std::nullopt;
  }
}  // namespace platf::dxgi
