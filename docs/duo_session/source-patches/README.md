# Preserved terminal-session HDR source

This directory makes the external source used by the confirmed 2026-08-15 HDR
proof reconstructible from the `duo_session_large` branch even if its local side
worktrees are removed before their branches are published.

## Remote session provider

- Repository: `https://github.com/lcxxjmsg-cyber/RemoteControlPanel.git`
- Base: `97960ae512baf14a609e5a8b4d49298e5f20ee49`
- Proven source commit: `00d5d5c43356c09b744f686a877a565e30af1977`
- Patch series: `remote-control-panel/*.patch`

Apply the patch from a checkout of the base commit with:

```powershell
git am D:/sources/worktrees/duo_session_large/docs/duo_session/source-patches/remote-control-panel/*.patch
```

## libvirtualdisplay Remote IDD

- Repository: `https://github.com/Nonary/libvirtualdisplay.git`
- Base: `3e85c1fb0e155eebdd640ee4abfd4fdca25bf3ce` (`v1.6.3`)
- Proven source commit: `c7c8c97b3793a4f75c0f3f263b53376f6af98397`
- Patch series: `libvirtualdisplay/*.patch`

The security correction is landed in isolated source commit
`ac2a37aa2ae698f76c593540ef52a586b5886e38` and must remain the pinned source
for the Duo integration; patch `0007-fix-driver-keep-remote-control-ACL-broker-only.patch`
reconstructs that exact commit, which removes proof ACL widening and rejects
ambiguous remote control-device matches.

The parent repository also pins `third-party/libvirtualdisplay` to the proven
commit, including the live remote-mode control contract. The patch series
preserves every intervening branch commit so the source can still be
reconstructed if that commit has not yet been published:

```powershell
git am D:/sources/worktrees/duo_session_large/docs/duo_session/source-patches/libvirtualdisplay/*.patch
```

These are proof sources, not a production deployment recipe. Live activation
must retain the guarded rollback, bounded helper, and fail-closed listener rules
documented in `../PROGRESS.md`.
