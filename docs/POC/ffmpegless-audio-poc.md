# FFmpegless Audio PoC

Date: 2026-05-17
Updated: 2026-05-21

## Current Public Path

The public FFmpegless path is Opus Direct. The server remuxes YouTube WebM Opus
to Ogg Opus, and the 3DS client decodes it through `opusfile`.

## Retired AAC Direct Checkpoint

AAC Direct was evaluated as an earlier PoC, but it required shipping an AAC
decoder in the 3DS binary. That code path, the AAC vendor tree, and the
server-side AAC endpoints were removed from the public repository.

Historical result summary:

- Server-side MP4/fMP4 AAC to ADTS transmux was technically viable.
- 3DS-side LC-AAC ADTS decoding was technically viable.
- The approach was retired for public distribution because decoder licensing and
  patent clearance were not aligned with the release goal.
