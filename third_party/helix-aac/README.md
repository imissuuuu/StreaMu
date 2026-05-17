# Helix AAC

Source: `pschatzmann/arduino-libhelix`, `src/libhelix-aac`

Imported on: 2026-05-17

This directory contains only the C Helix AAC decoder sources used by the AAC
PoC. The Arduino C++ wrapper is not imported.

License notes:

- The upstream repository states that the decoder code is from the Helix
  project and licensed under RealNetworks RPSL.
- `RPSL.txt` is kept in this directory.
- AAC patent/licensing requirements are not resolved by this PoC. Do not treat
  this as release-ready until distribution obligations are reviewed.

PoC scope:

- Decode ADTS AAC bytes emitted by `server/aac_transmux.py`.
- Keep MP3 proxy playback as the default path.
