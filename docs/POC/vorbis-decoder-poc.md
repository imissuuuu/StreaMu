# Vorbis Decoder PoC

## Goal
Evaluate Ogg Vorbis as a cleaner replacement candidate for an AAC decoder in the official StreaMu binary.

## References
- `C:/dev/research/3ds-audio-refs/ctrmus` confirms a 3DS music player can use `3ds-libvorbisidec` for Vorbis playback.
- `C:/devkitPro/examples/3ds/audio/ogg-vorbis-decoding` shows devkitPro's official `libvorbisidec` example.

## Dependency
- `3ds-libvorbisidec`
- `3ds-libogg`
- `3ds-pkg-config`

The Makefile asks `arm-none-eabi-pkg-config vorbisidec` for include and link flags. If the package is missing, the Vorbis PoC decoder will fail at compile time with a missing `tremor/ivorbisfile.h`.

## Current Scope
- Adds `VorbisStreamDecoder` under `source/audio`.
- Does not expose a new Settings option.
- Does not implement server-side YouTube to Ogg Vorbis conversion.

## Notes
`ctrmus` is GPL, so this PoC does not copy its source. It only follows the public `libvorbisidec` API shape: `ov_open_callbacks`, `ov_info`, `ov_read`, and `ov_clear`.

## Next Questions
- Can `3ds-libvorbisidec` decode common 44.1 kHz stereo Ogg Vorbis on real hardware without underruns?
- Should StreaMu's server transcode remote audio to Ogg Vorbis, or should Vorbis remain a local-file-only target?
