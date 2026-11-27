# PyroWave D3D12 conversion

Windows PyroWave sessions convert the captured frame to the three PyroWave
planes on a D3D12 **compute** command queue on the capture adapter. This is
the default path. It does not add native AMF D3D12 input or move DDA/WGC
capture copies onto a compute queue.

Why: the D3D11 conversion dispatch is queued with the game's own graphics
work. The aim is to keep conversion from waiting behind it; the benefit is not
yet measured on hardware (see Status). The D3D12 compute queue is created at `D3D12_COMMAND_QUEUE_PRIORITY_HIGH`, so the scheduler runs it ahead
of the game's compute work. If the driver refuses HIGH, the queue is created at
NORMAL and the host log says so. `GLOBAL_REALTIME` is never requested: it needs
a privilege and can starve the desktop. The Vulkan encoder queue is created at
HIGH global priority the same way (REALTIME is never requested; the log
reports the granted priority).

Selection and fallback, decided once at session start and logged:

- Default build: `SUNSHINE_PYROWAVE_D3D12_CONVERSION` is ON and sessions
  request the D3D12 conversion.
- `VIBESHINE_PYROWAVE_CONVERSION=d3d11` in the host's environment forces the
  D3D11 conversion for the session and logs a warning saying so.
- RTX HDR sessions use D3D11 (the transform produces its own RGB texture);
  the log says so.
- If the D3D12 device or queue cannot be created, or the driver lacks shared
  fences, the host logs a warning with the failing HRESULT and converts with
  D3D11 for the whole session. This is not a silent per-frame fallback.
- A source outside the supported set (below) logs one warning, then the
  session converts with D3D11 from that frame on.
- A queue ordering error or timeout after submission fails the session; it
  never falls back, because the planes may still be in flight.

The supported source is a single-mip, single-layer, non-MSAA Texture2D with
an ordinary matching SRV, in BGRA8 UNORM, RGBA8 UNORM, or RGBA16 FLOAT. All
existing transfer variants and the existing conversion shader are retained.
With the conversion requested, the core first registers a source-retention sink for the full adapter LUID. D3D12
initialization failure keeps a D3D11-only sink; failure to allocate or
register that sink fails core creation before capture begins. Unsupported
sources use the existing D3D11 conversion path. The core reports whether the
D3D12 backend was selected so a measurement cannot silently use the fallback.

Retirement is intentionally serialized:

1. Hold the capture keyed mutex, copy the source into a private NT-shared
   D3D11 snapshot, signal a dedicated producer fence, and flush D3D11.
2. The D3D12 compute queue waits for that producer value. Its command list
   reads the snapshot and writes the existing three shared PyroWave planes,
   returning every accessed resource to COMMON state.
3. The queue signals the existing Vulkan-imported conversion fence. The CPU
   waits up to two seconds for retirement before releasing capture ownership
   and passing the planes to the existing synchronous Vulkan encoder.

The successful return retires the sole allocator, descriptors, and snapshot
even if the caller subsequently skips encode because its byte budget is too
small. After a queue ordering error or timeout the session fails; it does
not overwrite potentially live planes through D3D11 fallback. Unfinished
backend objects retain separate producer and consumer obligations. A valid
covering D3D11 producer ticket must be observed complete; a failed producer
Signal with no covering ticket permanently retains the exact source image,
original and opened COM objects, and acquired encode-device keyed mutex. A
D3D12 tail, timeout, device removal or sentinel never discharges that unknown
producer obligation. A failed capture release also permanently retains the
source without retry. A successfully queued consumer-retirement signal can
cover the consumer tail. The existing removed-device policy remains for a
consumer whose completion is no longer marked unknown; native teardown effects
are not established here. An explicitly unknown consumer tail stays retained
even after device removal. Only one D3D12 conversion backend is admitted per full
adapter LUID; other requesting sessions may hold fallback-only sinks. The
per-LUID frame gate serializes their capture and fallback ownership. A source
whose release status is unknown permanently blocks that full LUID. Retained
objects are checked when another sink is registered.

Nonblank D3D12 copies require an explicit caller-scoped keyed-writer lease.
The host supplies the existing image control block from the exclusively async
PyroWave pool: pool selection requires use_count()==1, and actual DDA/WGC pixel
writers acquire the capture resource mutex. Standalone callers must provide a
real source owner and require every future writer of that exact resource to
acquire its mutex; a generic shared_ptr or arbitrary raw writer is insufficient.
The node pins exact original and opened resources independently of image-field
replacement, cache/session destruction and metadata changes. The production
PyroWave caller binds original and opened resources through `open_image()`'s
capture-texture cache key and `encoder_texture_handle`; the generic source
record does not prove that relationship for arbitrary callers. In a requesting
session, missing or unproven leases fail before acquisition. The sink takes
the unarmed exact mutex pin before `AcquireSync` and arms it only on `S_OK`.
Unsupported D3D12 preparation keeps the acquired lease in the sink through
D3D11 fallback. All D3D12 command setup and Close precede the possible-copy
mark. After that mark, failures are terminal and only scalar operation/status
metadata is recorded; there is no fallback or diagnostic allocation/callback
in that submission path.

Collection keeps a completed node admitted while it attempts capture release and
drops source owners outside the registry lock. Failed release poisons admission;
successful release permits detachment and final destruction outside the lock.
Permanent unknown retention is bounded to the existing single admitted generation
and its one source per full adapter LUID, with an explicit availability cost.
Native API-internal effects, cross-thread COM behavior, failure triggers and fence
query liveness remain unqualified; this source protocol does not establish a
healthy-device failed Signal outcome or full backend acceptance.

Before acquisition in a session that requested the conversion, the sink pins the
caller-provided image owner, original capture texture/mutex, opened SRV,
opened texture, and the exact keyed-mutex interface that will be acquired.
The production PyroWave caller binds original and opened resources through
`open_image()`'s capture-texture cache key and `encoder_texture_handle`; the
generic source record does not prove that relationship for arbitrary callers.
The per-LUID gate serializes same-adapter capture, conversion, and fallback
release. Contention skips a frame. Unsupported D3D12 preparation keeps the
acquired lease in the sink through D3D11 dispatch and unbind, then releases it
before signaling or flushing the output. A failed release is attempted once,
returns a failed frame before output publication, retains the exact disarmed
mutex plus all source/fallback owners, and permanently poisons that full LUID.
No later same-LUID sink may acquire after poison.

A session that does not request the conversion (RTX HDR, the environment
override, or a build with the option OFF) keeps the local D3D11 guard
behavior. These retention guarantees describe a session that requested the
conversion. They do not establish native API failure frequency,
all-caller lease validity, performance, feature acceptance, or deployment.

This conservative CPU wait and longer capture-mutex lifetime are the current
limits. Changing them requires an explicit per-frame retirement contract,
not just removal of the wait. The separate AMF work needs a DX12 surface
contract, per-output fences and surface-observer lifetime accounting.

With a native Windows toolchain and working PyroWave interop, the option is
on by default (`-DSUNSHINE_PYROWAVE_D3D12_CONVERSION=OFF` removes the backend
from the build). Build `test_pyrowave_windows_gpu` and run CTest's `pyrowave_windows_gpu` and
`pyrowave_windows_d3d12_gpu`. The second test requires actual D3D12 selection.
It checks real keyed-mutex sharing from a separate D3D11 producer, changing
frame values and snapshot extents, R8/R16 planes, 4:2:0/4:4:4, quarter-turn
letterboxing, blank frames, conversion-before-budget-skip retirement,
single-generation admission, and unsupported-source fallback through real
encode/decode calls. Decoding is to eight-bit CPU planes, so this does not
establish bit-identical R16 output or all HDR transfer functions.

## Status

Closed in source, with the Linux-side source checks that exercise the
extracted ownership bodies:

- the option and the session request are on by default, not opt-in;
- the compute queue priority is HIGH with a logged NORMAL fallback;
- unavailable D3D12 is reported at session start as a warning, and the
  environment override exists for field triage;
- the Windows GPU fixture and the CTest entries no longer carry the
  `experimental` label.

Native hardware qualification (a Linux source check or a successful fixture
build cannot establish it):

- D3D12 debug-layer and GPU validation;
- raw-plane parity for SDR/scRGB/PQ/SDR-to-PQ and all supported formats;
- alternating-frame/repeat detection under load;
- device-loss/timeout and shutdown/reinitialization behavior;
- latency and game frame-time comparison against `VIBESHINE_PYROWAVE_CONVERSION=d3d11`
  on Radeon, NVIDIA and Intel adapters, including whether HIGH priority is
  honored under a GPU-bound game.
