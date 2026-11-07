/**
 * @file tests/unit/platform/windows/test_hdr_stats_d3d11.cpp
 * @brief The HDR10+ statistics shader and its readback ring, run on the WARP software
 *        rasterizer so no GPU is involved.
 */
#include "../../../tests_common.h"
#include "src/hdr_dynamic_metadata.h"
#include "src/platform/windows/hdr_stats_d3d11.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using platf::dxgi::hdr_stats_d3d11_t;
namespace hdr = video::hdr_dynamic;

namespace {
  // Half floats of scRGB values: 1.0 is 80 nits.
  constexpr std::uint16_t half_0 = 0x0000;
  constexpr std::uint16_t half_1 = 0x3C00;  // 80 nits
  constexpr std::uint16_t half_2_5 = 0x4100;  // 200 nits
  constexpr std::uint16_t half_12_5 = 0x4A40;  // 1000 nits

  ComPtr<ID3DBlob> compile_stats_shader() {
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const std::filesystem::path path {HDR_STATS_TEST_SHADER_PATH};
    const auto status = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main_cs", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors);
    if (FAILED(status)) {
      ADD_FAILURE() << "hdr_stats_cs.hlsl failed to compile: "
                    << (errors ? std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize()) : std::string {});
      return nullptr;
    }
    return code;
  }

  class HdrStatsD3D11: public testing::Test {
  protected:
    void SetUp() override {
      const auto status = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
      if (FAILED(status)) {
        GTEST_SKIP() << "No WARP device";
      }
      shader = compile_stats_shader();
      ASSERT_TRUE(shader);
      auto created = hdr_stats_d3d11_t::create(device.Get(), shader->GetBufferPointer(), shader->GetBufferSize());
      ASSERT_TRUE(created) << created.error();
      pass.emplace(std::move(*created));
    }

    /// A texture of `pixels`, row by row, and a view of it.
    template<class T>
    ComPtr<ID3D11ShaderResourceView> texture(DXGI_FORMAT format, std::uint32_t width, std::uint32_t height, const std::vector<T> &pixels) {
      D3D11_TEXTURE2D_DESC desc {};
      desc.Width = width;
      desc.Height = height;
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = format;
      desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      const D3D11_SUBRESOURCE_DATA data {pixels.data(), static_cast<UINT>(sizeof(T) * pixels.size() / height), 0};
      ComPtr<ID3D11Texture2D> texture;
      EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, &data, &texture)));
      ComPtr<ID3D11ShaderResourceView> view;
      if (texture) {
        EXPECT_TRUE(SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &view)));
      }
      return view;
    }

    /// Poll until `count` readouts arrived, or give up after a few seconds.
    std::vector<hdr_stats_d3d11_t::readout_t> drain(std::size_t count) {
      std::vector<hdr_stats_d3d11_t::readout_t> readouts;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds {10};
      while (readouts.size() < count && std::chrono::steady_clock::now() < deadline) {
        auto polled = pass->poll(context.Get());
        if (!polled) {
          ADD_FAILURE() << polled.error();
          break;
        }
        readouts.insert(readouts.end(), polled->begin(), polled->end());
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
      }
      return readouts;
    }

    hdr::frame_stats_t measure(const hdr_stats_d3d11_t::source_t &source) {
      EXPECT_TRUE(pass->queue(context.Get(), source, 7));
      context->Flush();
      const auto readouts = drain(1);
      EXPECT_EQ(readouts.size(), 1u);
      if (readouts.empty()) {
        return {};
      }
      EXPECT_EQ(readouts.front().frame_index, 7);
      return readouts.front().stats;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3DBlob> shader;
    std::optional<hdr_stats_d3d11_t> pass;
  };

  /// Quarter-frame bands of gray at 0, 80, 200 and 1000 nits, one sample per pixel.
  std::vector<std::uint64_t> gray_bands(std::uint32_t width, std::uint32_t height) {
    const std::uint16_t bands[] {half_0, half_1, half_2_5, half_12_5};
    std::vector<std::uint64_t> pixels;
    for (std::uint32_t y = 0; y < height; ++y) {
      for (std::uint32_t x = 0; x < width; ++x) {
        const std::uint64_t value = bands[x * 4 / width];
        pixels.push_back(value | value << 16 | value << 32 | std::uint64_t {half_1} << 48);
      }
    }
    return pixels;
  }
}  // namespace

TEST_F(HdrStatsD3D11, MeasuresScRgbAsTheConverterEncodesIt) {
  const auto view = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, 64, 36, gray_bands(64, 36));
  const auto stats = measure({view.Get(), 64, 36, hdr_stats_d3d11_t::transfer_e::scrgb});

  EXPECT_NEAR(stats.min_pq, hdr::nits_to_pq(0.0f), 1e-4f);
  EXPECT_NEAR(stats.max_pq, hdr::nits_to_pq(1000.0f), 1e-3f);
  EXPECT_NEAR(stats.mean_nits, (0.0f + 80.0f + 200.0f + 1000.0f) / 4.0f, 10.0f);
  // Half the samples are at 80 nits or below: the median lands in the 80-nit bin.
  EXPECT_NEAR(stats.percentiles_pq[4], hdr::nits_to_pq(80.0f), 2.0f / hdr::histogram_bins);
}

TEST_F(HdrStatsD3D11, SamplesAGridOnLargeSources) {
  // 1920x1080 has 240x135 cells; parse_readout checks that every one was counted.
  const std::vector<std::uint64_t> pixels(1920 * 1080, std::uint64_t {half_2_5} * 0x0000000100010001ull | std::uint64_t {half_1} << 48);
  const auto view = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, 1920, 1080, pixels);
  const auto stats = measure({view.Get(), 1920, 1080, hdr_stats_d3d11_t::transfer_e::scrgb});

  EXPECT_NEAR(stats.min_pq, hdr::nits_to_pq(200.0f), 1e-3f);
  EXPECT_NEAR(stats.max_pq, hdr::nits_to_pq(200.0f), 1e-3f);
}

TEST_F(HdrStatsD3D11, AppliesTheTrueHdrScale) {
  const std::vector<std::uint64_t> pixels(16 * 16, std::uint64_t {half_1} * 0x0001000100010001ull);
  const auto view = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, 16, 16, pixels);
  hdr_stats_d3d11_t::source_t source {view.Get(), 16, 16, hdr_stats_d3d11_t::transfer_e::scrgb};
  source.scale = 2.0f;
  const auto stats = measure(source);

  EXPECT_NEAR(stats.max_pq, hdr::nits_to_pq(160.0f), 1e-3f);
}

TEST_F(HdrStatsD3D11, PlacesSdrWhiteForSdrToPq) {
  const std::vector<std::uint32_t> pixels(16 * 16, 0xffffffffu);
  const auto view = texture(DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, pixels);
  hdr_stats_d3d11_t::source_t source {view.Get(), 16, 16, hdr_stats_d3d11_t::transfer_e::sdr_to_pq};
  source.sdr_white_nits = 203.0f;
  const auto stats = measure(source);

  EXPECT_NEAR(stats.max_pq, hdr::nits_to_pq(203.0f), 1e-3f);
}

TEST_F(HdrStatsD3D11, ReadsEncodedValuesAsTheSignal) {
  // R10G10B10A2: red 512 of 1023, green and blue 0.
  const std::vector<std::uint32_t> pixels(16 * 16, 512u | 3u << 30);
  const auto view = texture(DXGI_FORMAT_R10G10B10A2_UNORM, 16, 16, pixels);
  const auto stats = measure({view.Get(), 16, 16, hdr_stats_d3d11_t::transfer_e::encoded});

  EXPECT_NEAR(stats.max_pq, 512.0f / 1023.0f, 1e-3f);
  EXPECT_NEAR(stats.min_pq, 512.0f / 1023.0f, 1e-3f);
}

TEST_F(HdrStatsD3D11, NeverQueuesMoreThanTheRing) {
  const auto view = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, 64, 36, gray_bands(64, 36));
  const hdr_stats_d3d11_t::source_t source {view.Get(), 64, 36, hdr_stats_d3d11_t::transfer_e::scrgb};

  for (std::int64_t frame = 0; frame < static_cast<std::int64_t>(hdr_stats_d3d11_t::ring_size); ++frame) {
    EXPECT_TRUE(pass->queue(context.Get(), source, frame));
  }
  EXPECT_FALSE(pass->queue(context.Get(), source, 100));
  EXPECT_EQ(pass->in_flight(), hdr_stats_d3d11_t::ring_size);
  context->Flush();

  const auto readouts = drain(hdr_stats_d3d11_t::ring_size);
  ASSERT_EQ(readouts.size(), hdr_stats_d3d11_t::ring_size);
  for (std::size_t i = 0; i < readouts.size(); ++i) {
    EXPECT_EQ(readouts[i].frame_index, static_cast<std::int64_t>(i));
  }
  EXPECT_EQ(pass->in_flight(), 0u);
  EXPECT_TRUE(pass->queue(context.Get(), source, 101));
  context->Flush();
  const auto next = drain(1);
  ASSERT_EQ(next.size(), 1u);
  EXPECT_EQ(next.front().frame_index, 101);
}

TEST_F(HdrStatsD3D11, RefusesASourceWithoutAView) {
  EXPECT_FALSE(pass->queue(context.Get(), {}, 1));
  EXPECT_EQ(pass->in_flight(), 0u);
}

TEST_F(HdrStatsD3D11, APassQueuedBeforeItsFrameIsKnownWaitsForItsTag) {
  const auto view = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, 64, 36, gray_bands(64, 36));
  const hdr_stats_d3d11_t::source_t source {view.Get(), 64, 36, hdr_stats_d3d11_t::transfer_e::scrgb};

  // Queued during a conversion, before the encoder named the frame.
  ASSERT_TRUE(pass->queue(context.Get(), source));
  context->Flush();
  for (int attempt = 0; attempt < 50; ++attempt) {
    const auto early = pass->poll(context.Get());
    ASSERT_TRUE(early) << early.error();
    EXPECT_TRUE(early->empty()) << "no readout without its frame";
    std::this_thread::sleep_for(std::chrono::milliseconds {1});
  }
  EXPECT_TRUE(pass->tag(12));
  EXPECT_FALSE(pass->tag(13)) << "one tag per pass";
  const auto readouts = drain(1);
  ASSERT_EQ(readouts.size(), 1u);
  EXPECT_EQ(readouts.front().frame_index, 12);
}

TEST_F(HdrStatsD3D11, AConversionThatIsNeverEncodedIsDropped) {
  const std::vector<std::uint64_t> dim(16 * 16, std::uint64_t {half_2_5} * 0x0000000100010001ull | std::uint64_t {half_1} << 48);
  const auto skipped_view = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, 16, 16, dim);
  const auto encoded_view = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, 64, 36, gray_bands(64, 36));

  // 200 nits, and its frame is never encoded; then the conversion that is, up to 1000 nits.
  ASSERT_TRUE(pass->queue(context.Get(), {skipped_view.Get(), 16, 16, hdr_stats_d3d11_t::transfer_e::scrgb}));
  ASSERT_TRUE(pass->queue(context.Get(), {encoded_view.Get(), 64, 36, hdr_stats_d3d11_t::transfer_e::scrgb}));
  EXPECT_TRUE(pass->tag(20));
  context->Flush();

  // Both buffers are freed once the GPU is done; only the encoded conversion reports.
  std::vector<hdr_stats_d3d11_t::readout_t> readouts;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds {10};
  while (pass->in_flight() > 0 && std::chrono::steady_clock::now() < deadline) {
    auto polled = pass->poll(context.Get());
    ASSERT_TRUE(polled) << polled.error();
    readouts.insert(readouts.end(), polled->begin(), polled->end());
  }
  EXPECT_EQ(pass->in_flight(), 0u);
  ASSERT_EQ(readouts.size(), 1u);
  EXPECT_EQ(readouts.front().frame_index, 20);
  EXPECT_NEAR(readouts.front().stats.max_pq, hdr::nits_to_pq(1000.0f), 1e-3f) << "frame 20 carries its own pixels";
}

TEST_F(HdrStatsD3D11, ADroppedPassTakesNoTag) {
  const auto view = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, 64, 36, gray_bands(64, 36));
  ASSERT_TRUE(pass->queue(context.Get(), {view.Get(), 64, 36, hdr_stats_d3d11_t::transfer_e::scrgb}));
  pass->drop_untagged();
  EXPECT_FALSE(pass->tag(3));
  context->Flush();

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds {10};
  while (pass->in_flight() > 0 && std::chrono::steady_clock::now() < deadline) {
    const auto polled = pass->poll(context.Get());
    ASSERT_TRUE(polled) << polled.error();
    EXPECT_TRUE(polled->empty());
  }
  EXPECT_EQ(pass->in_flight(), 0u) << "its buffer is freed all the same";
}

namespace {
  /// An HEVC AUD and P slice, as encoders emit inter frames.
  const std::vector<std::uint8_t> hevc_frame {0, 0, 0, 1, 35 << 1, 0x01, 0x50, 0, 0, 0, 1, 1 << 1, 0x01, 0xD0, 0x12};

  /// Samples the stream took into a packet of frame `frame_index`.
  std::uint64_t samples_by(hdr::stream_t &stream, std::int64_t frame_index) {
    std::vector<std::uint8_t> out;
    EXPECT_EQ(stream.splice(frame_index, hevc_frame, out), hdr::stream_t::splice_e::spliced);
    return stream.counters().samples;
  }

  class HdrStatsSession: public HdrStatsD3D11 {
  protected:
    void SetUp() override {
      HdrStatsD3D11::SetUp();
      if (IsSkipped() || HasFatalFailure()) {
        return;
      }
      view = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, 64, 36, gray_bands(64, 36));
      source = {view.Get(), 64, 36, hdr_stats_d3d11_t::transfer_e::scrgb};
    }

    /// Run submitted() for `frame_index` until no pass is in flight, or give up.
    bool settle(platf::dxgi::hdr_stats_session_t &session, std::int64_t frame_index) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds {10};
      while (std::chrono::steady_clock::now() < deadline) {
        EXPECT_FALSE(session.submitted(context.Get(), frame_index));
        if (session.in_flight() == 0) {
          return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {1});
      }
      return false;
    }

    ComPtr<ID3D11ShaderResourceView> view;
    hdr_stats_d3d11_t::source_t source;
  };
}  // namespace

TEST_F(HdrStatsSession, NothingExistsWithoutAStream) {
  platf::dxgi::hdr_stats_session_t session {HDR_STATS_TEST_SHADER_PATH};
  session.converted(context.Get(), source, true);
  session.converted(context.Get(), source, false);
  session.converted_unmeasured();
  EXPECT_FALSE(session.submitted(context.Get(), 1));
  EXPECT_FALSE(session.has_pass()) << "no shader, buffers or GPU work while the setting is off";
  EXPECT_EQ(session.in_flight(), 0u);
  EXPECT_FALSE(session.attach(device.Get(), nullptr));
  EXPECT_FALSE(session.has_pass());
}

TEST_F(HdrStatsSession, ASourceUnderTheKeyedMutexIsMeasuredInsideTheConversion) {
  auto stream = std::make_shared<hdr::stream_t>(hdr::codec_e::hevc);
  platf::dxgi::hdr_stats_session_t session {HDR_STATS_TEST_SHADER_PATH};
  ASSERT_FALSE(session.attach(device.Get(), stream));
  EXPECT_TRUE(session.has_pass()) << "made at the stream's start, before any frame";

  // Inside the hold: recorded before the conversion returns, named after the submit.
  session.converted(context.Get(), source, true);
  EXPECT_EQ(session.in_flight(), 1u) << "recorded while the conversion still holds the mutex";
  ASSERT_TRUE(settle(session, 0));
  EXPECT_EQ(samples_by(*stream, 0), 1u) << "the first frame is measured too";

  session.converted(context.Get(), source, true);
  ASSERT_TRUE(settle(session, 5));
  EXPECT_EQ(samples_by(*stream, 4), 1u) << "frame 4 takes nothing measured on frame 5";
  EXPECT_EQ(samples_by(*stream, 5), 2u);
}

TEST_F(HdrStatsSession, AConversionThatIsNeverEncodedIsNotReported) {
  auto stream = std::make_shared<hdr::stream_t>(hdr::codec_e::hevc);
  platf::dxgi::hdr_stats_session_t session {HDR_STATS_TEST_SHADER_PATH};
  ASSERT_FALSE(session.attach(device.Get(), stream));

  session.converted(context.Get(), source, true);  // skipped: its pixels were already sent
  session.converted(context.Get(), source, true);
  ASSERT_TRUE(settle(session, 9));
  EXPECT_EQ(samples_by(*stream, 9), 1u) << "one sample, from the encoded conversion";
}

TEST_F(HdrStatsSession, AnUnmeasuredConversionNamesNoEarlierPass) {
  auto stream = std::make_shared<hdr::stream_t>(hdr::codec_e::hevc);
  platf::dxgi::hdr_stats_session_t session {HDR_STATS_TEST_SHADER_PATH};
  ASSERT_FALSE(session.attach(device.Get(), stream));

  // A conversion under the mutex that is never encoded, then a blank frame that is.
  session.converted(context.Get(), source, true);
  session.converted_unmeasured();
  ASSERT_TRUE(settle(session, 5));
  EXPECT_EQ(samples_by(*stream, 5), 0u) << "the blank frame carries nothing measured on other pixels";

  // The same with a device-owned source waiting for the submit.
  session.converted(context.Get(), source, false);
  session.converted_unmeasured();
  EXPECT_FALSE(session.submitted(context.Get(), 6));
  EXPECT_EQ(session.in_flight(), 0u);
  EXPECT_EQ(samples_by(*stream, 6), 0u);

  // The next measured conversion is reported as usual.
  session.converted(context.Get(), source, true);
  ASSERT_TRUE(settle(session, 7));
  EXPECT_EQ(samples_by(*stream, 7), 1u);
}

TEST_F(HdrStatsSession, ADeviceOwnedSourceIsMeasuredAfterTheSubmit) {
  auto stream = std::make_shared<hdr::stream_t>(hdr::codec_e::hevc);
  platf::dxgi::hdr_stats_session_t session {HDR_STATS_TEST_SHADER_PATH};
  ASSERT_FALSE(session.attach(device.Get(), stream));

  session.converted(context.Get(), source, false);
  EXPECT_EQ(session.in_flight(), 0u) << "nothing recorded ahead of the encoder";
  ASSERT_TRUE(settle(session, 3));
  EXPECT_EQ(samples_by(*stream, 3), 1u);
}

TEST_F(HdrStatsSession, AShaderThatDoesNotCompileMeasuresNothing) {
  auto stream = std::make_shared<hdr::stream_t>(hdr::codec_e::hevc);
  platf::dxgi::hdr_stats_session_t session {L"does-not-exist/hdr_stats_cs.hlsl"};
  const auto reason = session.attach(device.Get(), stream);
  ASSERT_TRUE(reason);
  EXPECT_NE(reason->find("hdr_stats_cs.hlsl"), std::string::npos) << *reason;
  EXPECT_FALSE(session.has_pass());
  EXPECT_FALSE(session.active());

  session.converted(context.Get(), source, true);
  EXPECT_FALSE(session.submitted(context.Get(), 0)) << "reported once, by attach()";
  EXPECT_EQ(session.in_flight(), 0u);
}
