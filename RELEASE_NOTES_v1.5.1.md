# StreaMu v1.5.1 Release Notes

## Highlights

- Improved steady-state Opus playback behavior to reduce low-FPS feeling and refill instability during playback.
- Reduced noisy hot-path observation overhead while keeping the playback pipeline changes that were validated on hardware.
- Tightened several playback and UI edge cases, including buffering state transitions, startup connection checks, and QA Remove handling.

## Upgrade Notes

- Update both the 3DS app and the proxy server together.
- Replace your existing `StreaMu-Server.zip` with the version from this release.
- Existing Opus Direct usage stays the same; this is a maintenance and smoothness update rather than a feature reset.

## Release Assets

- `streamu.cia`
- `streamu.3dsx`
- `StreaMu-Server.zip`
  - Includes `StreaMu-Server.exe`, `LICENSE`, and `THIRD_PARTY_LICENSES.md`.

## Validation

- The Opus playback tuning changes were reviewed in TAKT review runs before release preparation.
- Device testing reported no regression and no meaningful playback artifacts after the steady-state refill tuning adjustments.
- GitHub release automation rebuilds the 3DS and server artifacts from the merged `main` commit before publishing this release.
