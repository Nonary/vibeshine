#!/usr/bin/env python3
"""Regression test for Dolby encoder failures remaining local to one session.

The fixture compiles the exact failure statements extracted from audio.cpp, then
runs them against independent per-session sample queues and one shared packet
queue. It covers both encoder initialization and frame-encoding failure without
requiring FFmpeg, audio devices, a GPU, or a running host. Source-contract checks
ensure the extracted statements remain the ones used by the Dolby production
branch and that neither failure path stops the shared queue.
"""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2]
compiler = sys.argv[2] if len(sys.argv) > 2 else "c++"
source = (root / "src/audio.cpp").read_text()


def between(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


dolby = between(source, "    if (config.use_ac3 || config.use_eac3) {", "    // Opus encoding")
if dolby.count("samples->stop();") != 2:
    raise AssertionError("Both Dolby initialization and encoding failures must stop the session sample queue")
if "packets->stop();" in dolby:
    raise AssertionError("Dolby failure must never stop the shared audio_packets queue")

init_failure = between(
    dolby,
    "if (bitrate_kbps <= 0 || !encoder.initialize",
    "BOOST_LOG(info) << codec_name",
)
encode_failure = between(
    dolby,
    "if (bytes < 0) {",
    "if (webrtc_stream::has_active_sessions()",
)
for name, failure in (("initialization", init_failure), ("encoding", encode_failure)):
    if "samples->stop();" not in failure or "return;" not in failure:
        raise AssertionError(f"Dolby {name} failure no longer terminates only its own encoder worker")

# Compile the actual failure statements in a deterministic two-session fixture.
# Each failure lambda contains the production queue-stop/return statements, not
# a reimplementation of their control flow.
program = r"""
#include <cassert>
#include <cstddef>
#include <iostream>
#include <vector>

struct Queue {
  bool running = true;
  std::vector<int> packets;
  void stop() { running = false; }
  void raise(int packet) { if (running) packets.push_back(packet); }
};

int main() {
  Queue shared_audio_packets;
  Queue dolby_initialization_samples;
  Queue dolby_encoding_samples;
  Queue opus_samples;

  auto fail_dolby_initialization = [&]() {
    Queue *samples = &dolby_initialization_samples;
    PRODUCTION_INIT_FAILURE
  };
  auto fail_dolby_encoding = [&]() {
    Queue *samples = &dolby_encoding_samples;
    PRODUCTION_ENCODE_FAILURE
  };

  // A Dolby encoder that cannot initialize must not tear down shared delivery.
  fail_dolby_initialization();
  assert(!dolby_initialization_samples.running);
  assert(shared_audio_packets.running);

  // The same isolation is required if initialization succeeded but encoding fails.
  fail_dolby_encoding();
  assert(!dolby_encoding_samples.running);
  assert(shared_audio_packets.running);

  // An unrelated Opus session must still deliver packets after either failure.
  opus_samples.raise(1);
  shared_audio_packets.raise(101);
  shared_audio_packets.raise(102);
  assert(opus_samples.running);
  assert(opus_samples.packets.size() == 1);
  assert(shared_audio_packets.running);
  assert((shared_audio_packets.packets == std::vector<int>{101, 102}));

  std::cout << "Dolby initialization/encoding failures stayed session-local; Opus delivery continued\n";
}
"""


def replace_once(text, marker, replacement):
    if text.count(marker) != 1:
        raise AssertionError(f"expected exactly one fixture marker: {marker}")
    return text.replace(marker, replacement, 1)


# Keep only the production queue-stop and return statements, removing logging
# and surrounding FFmpeg setup which are irrelevant to this isolation contract.
init_statements = "samples->stop();\n        return;"
encode_statements = "samples->stop();\n          return;"
if init_failure.count(init_statements) != 1:
    raise AssertionError("Could not isolate production initialization failure statements")
if encode_failure.count(encode_statements) != 1:
    raise AssertionError("Could not isolate production encoding failure statements")
program = replace_once(program, "PRODUCTION_INIT_FAILURE", init_statements)
program = replace_once(program, "PRODUCTION_ENCODE_FAILURE", encode_statements)

with tempfile.TemporaryDirectory(prefix="dolby-session-isolation-") as temporary:
    cpp = pathlib.Path(temporary) / "test.cpp"
    executable = pathlib.Path(temporary) / "test"
    cpp.write_text(program)
    subprocess.run(
        [compiler, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
         str(cpp), "-o", str(executable)],
        check=True,
    )
    result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=5, check=True)
    print(result.stdout, end="")

