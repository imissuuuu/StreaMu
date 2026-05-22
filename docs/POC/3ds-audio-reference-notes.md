# 3DS Audio Reference Notes

Date: 2026-05-17

## Useful References

- `ctrmus`: best direct reference for StreaMu's player split. It keeps codec
  details behind a small decoder function table and feeds NDSP with reusable
  PCM buffers.
- `Video_player_for_3DS`: proves AAC/MP4 playback is possible on 3DS, but does
  it through FFmpeg/libavformat/libavcodec/libswresample. Useful as a reality
  check, not as the PoC implementation path.
- `FourthTube`: useful for YouTube extraction behavior. It prefers `mp4a`
  audio and specifically chooses itag 140 when available.
- `pomegranate`: useful conceptually for packet decode into fixed-duration
  NDSP buffers, but the Rust/Symphonia stack is too large to port directly into
  this C++ codebase.

## Design Takeaways

- Opus Direct is the release playback path. Keep codec-specific decoder logic
  separated from NDSP queueing so future experiments stay isolated.
- Do not import the full Video_player/FourthTube FFmpeg playback stack into
  StreaMu. It would work against the FFmpegless goal and increases build and
  release risk.

## Applied Now

- `OpusPocPlayer` handles release playback from Ogg Opus data produced by the
  proxy server.
