# Native host audio regressions

This port builds against Common PR #4 on current Nonary canary. The synthetic
generator lives only under `tests/host714/support`, never in the host target.
No kernel-capture test or Vibe Spatial DSP is included. Current test receipts
must bind actual source/build hashes; older figures below are historical.

These tests exercise the production host codec, source generator, parser, RTP/AES-CBC/FEC packetizer, source admission/timing decisions and sender completion gate. The receiver is the actual completed client Common source, built in a separate DLL. They open no audio endpoint or socket and play no sound.

## Reproduce

From the sibling support directory, run `Build-Transport.ps1` with the pinned UCRT64 toolchain installed. It configures this standalone CMake project and supplies the same static Opus/OpenSSL archives as the host build. For other build environments:

```powershell
cmake -S tests/host714 -B build-host714 -G Ninja -DCOMMON_SOURCE_DIR=<completed-common-checkout> -DOPUS_LIBRARY=<host-static-opus-archive> -DOPUS_INCLUDE_DIR=<opus-include-root> -DOPENSSL_USE_STATIC_LIBS=ON
cmake --build build-host714
ctest --test-dir build-host714 --output-on-failure
```

`COMMON_SOURCE_DIR` is read only. The standalone build adds the existing `LI_AUDIO_TESTING` adapter and exports only its replay API; private host/client Reed-Solomon and OpenSSL symbols stay separate. `HOST714_BUILD_TRANSPORT=OFF` builds only protocol, delivery and source-lifecycle tests. `HOST714_SANITIZERS=ON` enables ASan/UBSan on supported native GCC/Clang builds; MinGW rejects this option rather than reporting an unsupported sanitizer run as PASS.

## Historical RC1 regression reference (not results of this port)

The UCRT64 build used GCC 16.2.0, libopus 1.6.1 and static OpenSSL 3.6.5. The actual Common revision was `4c0d39ca4c74197a214526562d668ce67946b960`; host nanors was `19f07b513e924e471cadd141943c1ec4adc8d0e0`. Exact archive and header hashes are recorded in `host714-support/evidence/transport-build-pins.json`.

| Test executable | Verified scope |
| --- | --- |
| `Host714ProtocolTests` | 210 assertions and 50,000 malformed-input cases; production host parser, actual Common SDP parser, legacy normal/HQ mapping, unsigned launch field, invalid 12ch/HQ rejection and startup/display context guards. |
| `Host714DeliveryTests` | 18 assertions; final parity acknowledgment, stop/cancellation, sender lease lifetime, failed sends, queue discard without removing another producer's data. |
| `Host714SourceLifecycleTests` | 28 assertions; production source admission, two concurrent contenders, RAII release after failure/end, reconnect, monotonic 5ms schedule, late reset, synthetic stall and native timeout/error decisions. |
| `Host714EncoderTests` | 1,323 assertions; actual host encoder and bounded synthetic source, all three bitrates, exact blocks, invalid inputs, delay-corrected channel identity and EOF. |
| `Host714TransportTests` | 57 cases, 128,409 assertions, zero failures/skips; additionally the unchanged Common receiver's 19 cases and 17,627 assertions pass. |

Transport cases cover plain/encrypted packets, one/two losses per FEC block, duplicates/reordering, unrecoverable loss and actual PLC, sequence/timestamp wrap, malformed/truncated RTP/FEC/CBC, invalid padding, exact size bounds, session CBR changes, stop/reconnect and legacy packet sizes. The first four data packets are intentionally discarded by Common's normal receiver synchronization. All remaining recovered Opus bytes are checked before actual 12-channel Opus decoding.

| Opus bitrate | Fixed Opus bytes | Plain data/FEC bytes | Encrypted data/FEC bytes | Encrypted mean datagram bytes | Encrypted datagram payload rate | Synthetic minimum channel correlation |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 600 kbit/s | 375 | 387 / 399 | 396 / 408 | 400 | 960,000 bit/s | 0.99541876 |
| 1,536 kbit/s | 960 | 972 / 984 | 988 / 1,000 | 992 | 2,380,800 bit/s | 0.99886972 |
| 2,048 kbit/s | 1,280 | 1,292 / 1,304 | 1,308 / 1,320 | 1,312 | 3,148,800 bit/s | 0.99889613 |

These are measured production packetizer datagram lengths and aggregate payload rates including 4+2 FEC and CBC padding. IP/UDP network headers are not included. Every decoded channel's strongest correlation matches the same input channel, including LFE and all four heights. The independent broadband fixture also passes, with minimum correlations 0.93759419 / 0.98874641 / 0.99146800. A requested 3,072 kbit/s profile is rejected, and 1,376-byte encrypted plaintext/1,920-byte payloads are rejected before transmission.

## Scope and remaining hardware work

The synthetic source-to-production-encoder-to-production-packetizer-to-actual-Common-receiver-to-Opus-decoder path is proven offline. The clock/admission tests run the production decisions with deterministic time and real competing threads; they do not measure Windows scheduler performance or acoustic latency. Sender cancellation/EOF drain is tested independently from packet replay.

The portable test directory needs the five EXEs plus adjacent `libmoonlight-common-c.dll` and `libwinpthread-1.dll`; Opus/OpenSSL are statically linked. Logs under `host714-support/evidence` distinguish CTest, full replay, codec and Windows-only-PATH execution. The isolated host package's manual instructions control any live playback.
