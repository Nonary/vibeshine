# Native audio integration

See [Vibe Speakers](vibe-speakers.md) for the native-only source contract,
setup, bounded IPC, format and role restoration, and dependencies.

The production codec/packet/receiver regressions are in `tests/host714`.
There is no kernel-ring, test-source or DSP route in the shipped host.
