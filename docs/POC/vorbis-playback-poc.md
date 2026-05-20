# Vorbis Playback PoC

## Goal
Verify that StreaMu can feed PCM decoded by `VorbisStreamDecoder` into NDSP and play an Ogg Vorbis sample on real 3DS hardware.

## Input
- Place a sample at `sdmc:/3ds/StreaMu/vorbis_poc.ogg`.
- Keep the sample reasonably small for this PoC. The app rejects files larger than 64 MiB.
- The file is fully loaded into memory before playback starts.

## Trigger
- Open the Home screen.
- Hold `L` + `R`, then press `Y`.
- On success, the now-playing title becomes `Vorbis PoC`.

## Implementation Notes
- `VorbisPocPlayer` mirrors the existing stream-decoder-to-NDSP player shape.
- The official Settings `Audio Path` remains `AAC Direct` / `MP3 Proxy`; Vorbis is not a user-facing delivery path yet.
- Server-side YouTube to Ogg Vorbis conversion is intentionally out of scope for this PoC.
- `3ds-libvorbisidec` and `3ds-libogg` are provided through the existing `vorbisidec` pkg-config Makefile integration.

## Manual Test Items
- Boot regression: app reaches Home screen.
- Vorbis sample: `L+R+Y` starts playback and audio is heard.
- Vorbis finish: playback reaches the end without freezing, and now-playing clears.
- AAC Direct regression: normal playback still starts and produces audio.
- MP3 Proxy regression: Settings can switch to MP3 Proxy and normal playback still works.
