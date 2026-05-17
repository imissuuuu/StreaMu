# FFmpegless Audio PoC

Date: 2026-05-17

## Checkpoint A: Server AAC Info

Command target:

- `GET /api/aac-info?i=dQw4w9WgXcQ`

Result:

- PASS
- `ext`: `m4a`
- `acodec`: `mp4a.40.2`
- `container`: `m4a_dash`
- `sample_rate`: `44100`
- `channels`: `2`

## Checkpoint B: Server AAC to ADTS

Command target:

- `GET /stream_aac_adts?i=dQw4w9WgXcQ`

Result:

- PASS
- First bytes: `ff f1 50 80 2f 5f fc 21 30 05 00 a0 1b ff c0 00`
- The first two bytes confirm ADTS sync.

## Current Decision Notes

- YouTube's selected `m4a_dash` stream begins as `ftyp/moov/sidx/moof/mdat`.
- The server-side transmuxer parses AAC config from `moov`, sample sizes from
  `moof/trun`, and emits ADTS frames from `mdat`.
- 3DS-side MP4 parsing is still intentionally avoided.

## Checkpoint C: 3DS AAC Decoder Build

Result:

- PASS
- Vendored Helix AAC C core under `third_party/helix-aac`.
- Added minimal non-Arduino compatibility headers for `ConfigHelix.h`,
  `utils/helix_pgm.h`, `utils/helix_memory.h`, and `hlxclib/stdlib.h`.
- Disabled SBR at config and Makefile level for the PoC. Current target is
  LC-AAC ADTS only.
- `make clean; make` builds `streamu.3dsx` and `streamu.cia`.

## Helix Porting Notes

- The Arduino wrapper is GPLv3, so the PoC imports only `src/libhelix-aac`
  plus license material and local compatibility shims.
- The AAC core expects RealNetworks/Helix-era portability headers even when the
  Arduino C++ wrapper is not used.
- Upstream ARM assembly selection is not usable as-is for this target, so the
  PoC selects the portable C path for `__3DS__`.
- RPSL and AAC codec patent/licensing implications need a separate review
  before treating this as release-ready.
