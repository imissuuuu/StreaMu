# Third-Party Licenses

This project uses the following third-party libraries and tools.

## devkitPro / libctru
- **Description**: Homebrew development toolchain and library for Nintendo 3DS
- **License**: zlib / ISC
- **URL**: https://github.com/devkitPro/libctru

## citro2d / citro3d
- **Description**: 2D/3D rendering libraries for Nintendo 3DS
- **License**: zlib
- **URL**: https://github.com/devkitPro/citro2d

## yt-dlp
- **Description**: YouTube video/audio data extraction
- **License**: Unlicense
- **URL**: https://github.com/yt-dlp/yt-dlp

## FFmpeg
- **Description**: Audio/video transcoding (used for MP3 conversion)
- **License**: LGPL 2.1 or later
- **URL**: https://ffmpeg.org/

## Starlette
- **Description**: Lightweight ASGI framework for the proxy server
- **License**: BSD 3-Clause
- **URL**: https://github.com/encode/starlette

## Uvicorn
- **Description**: ASGI server for running the proxy
- **License**: BSD 3-Clause
- **URL**: https://github.com/encode/uvicorn

## Helix AAC decoder core
- **Description**: AAC decoder core used by AAC Direct playback on the 3DS
- **License**: RealNetworks Public Source License 1.0 (RPSL-1.0)
- **Source in this repository**: `third_party/helix-aac/`
- **License text**: `third_party/helix-aac/RPSL.txt`
- **Upstream note**: https://github.com/pschatzmann/arduino-libhelix
- **Object code notice**: Helix DNA Client technology included. Copyright (c) RealNetworks, Inc., 1995-2002. All rights reserved.
- **Patent note**: RPSL does not resolve AAC codec patent licensing. See `docs/LEGAL/aac-direct-license-review.md` before distributing AAC Direct binary builds.
