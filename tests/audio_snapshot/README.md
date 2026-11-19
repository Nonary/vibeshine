# Audio snapshot production-component tests

```sh
cmake -S tests/audio_snapshot -B /tmp/audio-snapshot-build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=/usr/bin/g++-15
cmake --build /tmp/audio-snapshot-build -j2
ctest --test-dir /tmp/audio-snapshot-build --output-on-failure -j1
python tests/audio_snapshot/prove_regression.py . /tmp/audio-snapshot-red-green
```

The test executable compiles the real `src/audio_context.cpp`, includes the real
`src/audio_role_control.h`, and uses the same `audio_ctx_t`, `safe::shared_t`, role
registry, first-assignment policy, restoration, and transport join functions as
the application. The Windows MMDevice adapter delegates to this role controller.
Device enumeration/format/write APIs, process state, scheduler admission and the
RTSP watchdog/control-end boundary are test adapters. Ownership and role policy
are not reimplemented by the test adapters. Capture join tests run real threads.

The compatibility Python entry points compile these complete sources; they do
not extract, truncate, rewrite or generate C++.

The proof script compiles complete copies of these production components,
with unchanged tests. Three checked-in patches independently restore historical
behavior from `69e163f33179860b8c63e6dd36ecc73f144e6fbd`: first assignment overwrote
nonempty role defaults, launch did not hold audio before display mutation, and
direct restoration used only the initial role snapshot to decide ownership.
They must compile successfully and observably select `fallback` instead of `dp`,
or overwrite a newer `user` choice with `headset`. A fourth patch deliberately
releases ownership before joining; both real transport join functions must expose
early control destruction. This is a release-order regression mutation, not a
claim about the exact historical transport implementation. The historical commit
does not contain these newly separated components, so these are **refactor-equivalent
behavioral baselines**, not builds of the original Windows translation unit.

The suite establishes portable production-component behavior. It does not
establish MMDevice/COM integration, native Windows compilation, real display or
endpoint timing, RTSP/WebRTC networking, microphone samples, or hardware behavior.
The unchanged legacy Windows visibility fallback remains outside this component's
runtime test scope. Call-site inspection supplements the component tests.

The production `webrtc_capture_owner.h` now supplies the actual active-state
thread owner, teardown owner, join wrapper and startup hold-transfer wrapper.
The hold is destroyed after its owned threads during exceptional teardown.
Two separate-process tests inject real self-join errors and inspect the hold
from the terminating thread destructor. They exercise the actual enclosing
`WebRtcCaptureTeardown`; they do not catch a helper exception while retaining
the hold in an unrelated outer variable. This injects the failure boundary;
no normal application self-join route was found in the inspected call graph.

During startup, the wrapper takes the hold out of pending metadata before that
metadata is globally published, keeps it local through thread construction,
and commits it to the active owner only after construction succeeds. Failure
tests throw at the OS thread-construction boundary before and after video has
started. Successful creation and ordinary teardown also use the same wrapper.
The proof script independently reinstates v2-equivalent late member destruction
and premature hold publication; each must compile and lose/strand the actual
audio control observably.

`69e163f33179860b8c63e6dd36ecc73f144e6fbd:src/webrtc_stream.cpp:3463`
already publishes `active=true` before video/audio thread construction at
3465/3468. Resource-exhaustion exceptions can therefore leave partial startup
eligible for the same-key fast path. This existing runtime rollback problem is
outside the audio-hold correction. The wrapper prevents the added hold from
being stranded by those exceptions; it does not claim to repair the partial
capture state, its retry behavior, or cleanup of other runtime layers.

Role restoration is best effort: it preserves user choices **observed by the
final default-role check**. The subsequent `SetDefaultEndpoint` call is separate;
the interface provides no atomic compare-and-set. A change to that same role
after the final check, including while the write is in flight, can be overwritten.
The `roles.read_write_gap` test demonstrates this exact limit with the actual
controller. The same qualification applies to retry writes and the unchanged
native visibility worker; no OS-event or atomicity redesign is claimed.

## Configuration-triggered display mutation

`src/config_display_transition.h` is the actual production sequencer used by
`config::apply_config_now`: install effective settings, evaluate the original
DD-change/no-owner/empty-runtime predicate, make a best-effort caller capture,
then dispatch the existing REVERT with its original enabled-to-disabled policy.
The configuration write gate and parser stay in `config.cpp`; the hook must not
reenter that gate. Callers without a hook retain their previous behavior.

`src/audio_display_transition.h` supplies the real lazy request owner used by
normal RTSP game launch/resume and standalone WebRTC startup. Construction does not acquire audio.
RTSP captures before its existing first DISARM; configuration captures after
effective settings are installed and before its own REVERT. The same shared
hold is copied into launch metadata and retained locally through metadata,
runtime/config and response rollback. Preparation and WebRTC use the same actual
`capture_if_empty_and_attach` method: adopt existing pending ownership, acquire
best effort only when empty, and attach while retaining the local copy. A failed
earlier attempt can retry here; repeated exceptions leave ownership empty and
allow preparation to continue. Successful transports keep using
the v3 startup and teardown owners.

Seven transition cases exercise these complete production headers with the real
context/role components. Test adapters install configuration values and replace
display/device boundaries. They do not reimplement the predicate, ordering or
request ownership. Coverage includes effective `stream_audio` false-to-true and
true-to-false, changed `virtual_sink` and driver-install configuration observed
at the device factory, all predicate/disabled-recovery flag combinations,
non-vetoing capture exceptions, first DISARM ordering, lazy early denial and
aborted metadata with both early/fallback capture surviving rollback. The raw
configuration parser and HTTP admission/authentication engines are not executed
by these portable tests; their unchanged placement is inspected in the callers.

Five additional cases call that complete fallback method directly: first capture
without an earlier DISARM, first acquisition failure, repeated failures after
config/DISARM, successful retry surviving metadata/config/response rollback, and
existing pending/local holds without replacement or extra acquisition. The same
real context and role components run on successful acquisition. A checked-in
whole-method control restores the old NVHTTP raw-acquisition-then-attach sequence;
it must compile and fail normally with an escaped exception and missing
preparation continuation, for both first and repeated failures.

The suite now has 36 ordinary cases and two isolated owner-unwind checks across
five CTest groups. Five additional compiled fault controls remove config capture,
move capture before installation or after DISARM, drop the local shared copy,
or let capture exceptions veto REVERT. They must fail through observed role
defaults, capture/destruction counts or missing REVERT dispatch, alongside the
previous eight controls. Two fallback-control runs bring the total to 15 expected
behavioral reds. The missing-config-capture control is v3-equivalent
sequencing in the newly separated boundary; the others are regression mutations.
No original native Windows/protocol build or incident reproduction is claimed.

For those normal startup paths, a configuration REVERT dispatched by the current
request follows its capture hook even when the helper runs it asynchronously.
Synthetic Remote Input/Monitor and other control paths remain unhooked and are
outside this claim; synthetic game actions delegated to normal resume enter the
hooked path. No concurrent WebRTC deferred-configuration timing fix is claimed.
Work from a previous request that already changed the live
defaults before acquisition cannot be reconstructed; the prior-restore-boundary
case explicitly retains that limitation. DISARM remains cancellation of existing
work, with no new display commands introduced by this change.
