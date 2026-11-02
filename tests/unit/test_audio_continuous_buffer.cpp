#include "src/platform/windows/audio_continuous_buffer.h"

#include <cstdlib>
#include <iostream>
#include <numeric>
using platf::audio_detail::continuous_buffer_t;
#define CHECK(expr) \
  do { \
    if (!(expr)) { \
      std::cerr << "failed: " #expr << " at line " << __LINE__ << "\n"; \
      std::exit(1); \
    } \
  } while (false)
constexpr std::int64_t epoch = 9000000000000;  // nonzero QPC uptime, not session time
constexpr std::int64_t ms = 10000;

int main() {
  // 5 ms output packets and a 10 ms WASAPI period. Exactly 200 output
  // packets/second, even with no events or with repeated empty events.
  continuous_buffer_t silence(48000, 2, 240, 480, epoch);
  std::vector<float> stereo(480, 99);
  CHECK(!silence.take(epoch + 29 * ms, stereo));
  for (int n = 0; n < 200; ++n) {
    auto now = epoch + (30 + n * 5) * ms;
    silence.advance(now);
    CHECK(silence.take(now, stereo));
    CHECK(std::all_of(stereo.begin(), stereo.end(), [](float x) {
      return x == 0;
    }));
    CHECK(!silence.take(now, stereo));
  }

  // A captured 10 ms packet delivered just before its deadline is split into
  // two 5 ms packets; the next deadline emits silence, never a duplicate.
  continuous_buffer_t delayed(48000, 2, 240, 480, epoch);
  std::vector<float> input(960);
  std::iota(input.begin(), input.end(), 1);
  CHECK(delayed.put(epoch, 480, input.data(), false));
  CHECK(delayed.take(epoch + 30 * ms, stereo));
  CHECK(std::equal(stereo.begin(), stereo.end(), input.begin()));
  CHECK(delayed.take(epoch + 35 * ms, stereo));
  CHECK(std::equal(stereo.begin(), stereo.end(), input.begin() + 480));
  CHECK(delayed.put(epoch, 480, input.data(), false));  // wholly late: drop
  CHECK(delayed.take(epoch + 40 * ms, stereo));
  CHECK(std::all_of(stereo.begin(), stereo.end(), [](float x) {
    return x == 0;
  }));

  // Gaps are replaceable until emitted; arrival ordering does not append
  // extra time or conceal a short earlier packet behind a future one.
  continuous_buffer_t reordered(48000, 2, 240, 480, epoch);
  CHECK(reordered.put(epoch + 10 * ms, 240, input.data(), false));
  CHECK(reordered.put(epoch, 240, input.data() + 480, false));
  CHECK(reordered.take(epoch + 30 * ms, stereo));
  CHECK(stereo.front() == 481);
  CHECK(reordered.take(epoch + 35 * ms, stereo));
  CHECK(stereo.front() == 0);
  CHECK(reordered.take(epoch + 40 * ms, stereo));
  CHECK(stereo.front() == 1);

  // Partial overlap trims frames already emitted; multichannel indexing is
  // frame-aligned and preserves the unplayed tail.
  continuous_buffer_t overlap(48000, 2, 240, 480, epoch);
  CHECK(overlap.take(epoch + 30 * ms, stereo));
  CHECK(overlap.put(epoch, 480, input.data(), false));
  CHECK(overlap.take(epoch + 35 * ms, stereo));
  CHECK(stereo.front() == 481 && stereo.back() == 960);

  // Timestamp quantization of less than half a frame retains alignment.
  continuous_buffer_t jitter(48000, 2, 240, 480, epoch);
  CHECK(jitter.put(epoch + 4, 240, input.data(), false));
  CHECK(jitter.take(epoch + 30 * ms, stereo));
  CHECK(stereo.front() == 1);

  // Silent WASAPI buffers may have a null pointer. Long gaps never allocate
  // more memory; a 10-second scheduler stall recovers with one current packet.
  continuous_buffer_t stall(48000, 2, 240, 480, epoch);
  CHECK(stall.put(epoch, 240, nullptr, true));
  stall.advance(epoch + 10000 * ms);
  CHECK(stall.take(epoch + 10000 * ms, stereo));
  CHECK(!stall.take(epoch + 10000 * ms, stereo));
  CHECK(stall.wait_ms(epoch + 10000 * ms) == 5);
  CHECK(stall.put(epoch, 480, input.data(), false));
  CHECK(!stall.put(epoch + 20000 * ms, 240, input.data(), false));
  CHECK(!stall.put(-1, 240, input.data(), false));

  // Ring reuse cannot replay sound from a previous lap; 8 channels and
  // non-divisible device/output periods also maintain the output duration.
  continuous_buffer_t surround(48000, 8, 240, 336, epoch);
  std::vector<float> surround_in(1920, 7), surround_out(1920);
  CHECK(surround.put(epoch, 240, surround_in.data(), false));
  for (int n = 0; n < 450; ++n) {
    auto now = epoch + (24 + n * 5) * ms;
    surround.advance(now);
    CHECK(surround.take(now, surround_out));
    CHECK(surround_out.front() == (n == 0 ? 7 : 0));
  }
  // New endpoint/session: a new absolute QPC origin resets all state.
  continuous_buffer_t reset(48000, 2, 240, 480, epoch + 100000 * ms);
  CHECK(!reset.take(epoch + 100000 * ms, stereo));
  CHECK(reset.put(epoch + 100000 * ms, 240, input.data(), false));
  CHECK(reset.take(epoch + 100030 * ms, stereo));
  CHECK(stereo.front() == 1);
  std::cout << "continuous audio timeline tests passed\n";
}
