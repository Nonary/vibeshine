# Native audio integration

See [Vibe Speakers](vibe-speakers.md) for the native-only source contract,
setup, bounded IPC, format/role restoration, dependencies and release guard.

The production codec/packet/receiver regressions are in `tests/host714`.
There is no kernel-ring, test-source or DSP route in the shipped host.
