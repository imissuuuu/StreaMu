# StreaMu v1.5.0 Release Notes

## Highlights

- Opus Direct is now the only release playback path.
- MP3 Proxy and server-side FFmpeg transcoding have been removed.
- The proxy server no longer requires FFmpeg.
- Existing `audio_path` settings, including old `mp3` and `aac_direct` values, are treated as Opus Direct.

## Upgrade Notes

- Update both the 3DS app and the proxy server together.
- Use the latest `StreaMu-Server.zip`; older proxy servers may not provide the required `/stream_opus_ogg` endpoint.
- If playback fails for a specific video, please include the video ID and the server dashboard log when reporting it.

## Release Assets

- `streamu.cia`
- `streamu.3dsx`
- `StreaMu-Server.zip`
  - Includes `StreaMu-Server.exe`, `LICENSE`, and `THIRD_PARTY_LICENSES.md`.

## Validation

- Clean 3DS build passed and produced `streamu.3dsx` / `streamu.cia`.
- Python syntax check passed for proxy server modules.
- Manual 3DS smoke test passed.
