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

- Keep MP3 as the default and preserve the current proxy-transcoded path.
- Split decoder logic from NDSP queueing before adding AAC. This keeps AAC PoC
  work from tangling with playback buffer ownership.
- For AAC PoC, avoid MP4 parsing on 3DS. The server should select `mp4a`,
  transmux to ADTS, and let 3DS decode ADTS frames only.
- Do not import the full Video_player/FourthTube FFmpeg playback stack into
  StreaMu. It would work against the FFmpegless goal and increases build and
  release risk.

## Applied Now

- `MP3Player` now delegates MP3 frame decode to `Minimp3StreamDecoder`.
- The new `StreamDecoder` result shape is compatible with a future
  `AacAdtsStreamDecoder`.
