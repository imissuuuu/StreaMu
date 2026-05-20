# Opus Direct PoC

Date: 2026-05-20

## Purpose

FFmpeg runtime dependency and AAC decoder licensing riskを避けるため、YouTube native Opus/WebMをserver側でOgg Opusへremuxし、3DS側で`opusfile`から直接PCMへdecodeできるかを確認した。

## Rights Boundary

- `3ds-libopus`, `3ds-opusfile`, `3ds-libogg` はdevkitPro pacman metadataと同梱licenseでBSD系として確認済み。
- WebM -> Ogg Opus remuxはこのリポジトリ内の自前PoC実装で、FFmpeg/libavformatはruntime pathに含めない。
- `ctrmus` はGPLv3のため、コードはコピーしない。実装方針の参考に留める。
- yt-dlp仕様、YouTube利用規約、配信コンテンツ権利は本PoCのコード面確認から除外する。

## Server Gate

Implemented files:

- `server/opus_probe.py`
- `server/webm_opus_remux.py`
- `server/proxy.py`

Endpoints:

- `GET /api/opus-info?i=<video_id>`
- `GET /stream_opus?i=<video_id>`
- `GET /stream_opus_ogg?i=<video_id>`

Representative gate corpus:

| video_id | channels | sample_rate | packets | codec_delay_ns | seek_preroll_ns | ogg_bytes | host decode |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `dQw4w9WgXcQ` | 2 | 48000 | 10653 | 6500000 | 80000000 | 3667561 | PASS |
| `jNQXAC9IVRw` | 2 | 48000 | 951 | 6500000 | 80000000 | checked | PASS |
| `9bZkp7q19f0` | 2 | 48000 | 12612 | 6500000 | 80000000 | checked | PASS |

`/api/opus-info?i=dQw4w9WgXcQ` returned Opus WebM format `251`, `acodec=opus`, `container=webm_dash`, `sample_rate=48000`, `channels=2`.

`/stream_opus?i=dQw4w9WgXcQ` returned `audio/webm` with EBML first bytes `1A 45 DF A3`.

`/stream_opus_ogg?i=dQw4w9WgXcQ` returned 3667561 bytes, first bytes `OggS`, and contained `OpusHead`.

## Technical Result

PASS. The sampled YouTube audio-only WebM files fit the narrow subset:

- single `A_OPUS` audio track
- `CodecPrivate` starts with `OpusHead`
- no unsupported lacing observed
- `CodecDelay` and `SeekPreRoll` are parsed and recorded
- Ogg granule position is built from parsed Opus packet duration, not fixed 20ms
- PC-side FFmpeg validation decoded generated Ogg Opus without errors

`make clean` followed by `make` completed successfully and produced `streamu.3dsx` and `streamu.cia`.

## 3DS PoC Path

The 3DS does not parse WebM. It loads a complete Ogg Opus file from:

- `sdmc:/3ds/StreaMu/opus_poc.opus`
- `sdmc:/3ds/streamu/opus_poc.opus`
- `sdmc:/opus_poc.opus`

Hidden trigger:

- Home screen: `L + R + X`

Expected preparation:

1. Run the server.
2. Save `/stream_opus_ogg?i=dQw4w9WgXcQ` as `sdmc:/3ds/StreaMu/opus_poc.opus`.
3. Launch the app and press `L + R + X` on Home.

## Promotion Notes

- `Opus Direct` is being promoted from hidden PoC to a formal selectable Audio Path candidate.
- Default audio path remains unchanged during the promotion step.
- Initial promoted playback uses full Ogg Opus download before decoder open.
- Seek is not supported for Opus in this phase; MP3 Proxy remains fallback.
