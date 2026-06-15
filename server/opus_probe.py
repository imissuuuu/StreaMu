from __future__ import annotations

import time
from dataclasses import asdict, dataclass
from typing import Any

import yt_dlp

OPUS_FORMAT_SELECTOR = "249/bestaudio[acodec*=opus][ext=webm]/bestaudio[acodec*=opus]"


@dataclass(frozen=True)
class OpusFormatInfo:
    video_id: str
    direct_url: str
    fragments: list[dict[str, Any]]
    http_headers: dict[str, str]
    format_id: str
    ext: str
    acodec: str
    container: str
    abr: float | None
    sample_rate: int | None
    channels: int | None
    filesize: int | None
    duration: float | None

    def to_json_dict(self) -> dict[str, Any]:
        data: dict[str, Any] = asdict(self)
        data["direct_url"] = bool(self.direct_url)
        data["fragments"] = len(self.fragments)
        data["http_headers"] = sorted(self.http_headers.keys())
        return data


@dataclass(frozen=True)
class OpusExtractMetrics:
    ydl_init_ms: int
    extract_info_ms: int
    postprocess_ms: int
    total_ms: int


@dataclass(frozen=True)
class OpusFormatLookup:
    info: OpusFormatInfo | None
    metrics: OpusExtractMetrics
    failure_stage: str | None


def _as_float_or_none(value: Any) -> float | None:
    if value is None:
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _as_int_or_none(value: Any) -> int | None:
    if value is None:
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _headers_from_info(info: dict[str, Any]) -> dict[str, str]:
    headers = info.get("http_headers", {})
    if not isinstance(headers, dict):
        return {}
    return {str(key): str(value) for key, value in headers.items()}


def _fragments_from_info(info: dict[str, Any]) -> list[dict[str, Any]]:
    fragments = info.get("fragments", [])
    if not isinstance(fragments, list):
        return []
    return [fragment for fragment in fragments if isinstance(fragment, dict)]


def extract_opus_format_lookup(video_id: str) -> OpusFormatLookup:
    yt_url = f"https://www.youtube.com/watch?v={video_id}"
    ydl_opts: dict[str, Any] = {
        "format": OPUS_FORMAT_SELECTOR,
        "quiet": True,
        "no_warnings": True,
        "socket_timeout": 10,
    }
    total_start = time.monotonic()
    ydl_init_ms = 0
    extract_info_ms = 0

    try:
        ydl_init_start = time.monotonic()
        with yt_dlp.YoutubeDL(ydl_opts) as ydl:
            ydl_init_ms = int((time.monotonic() - ydl_init_start) * 1000)
            extract_start = time.monotonic()
            raw_info = ydl.extract_info(yt_url, download=False)
            extract_info_ms = int((time.monotonic() - extract_start) * 1000)
    except (yt_dlp.utils.DownloadError, OSError, ValueError):
        total_ms = int((time.monotonic() - total_start) * 1000)
        return OpusFormatLookup(
            info=None,
            metrics=OpusExtractMetrics(
                ydl_init_ms=ydl_init_ms,
                extract_info_ms=extract_info_ms,
                postprocess_ms=0,
                total_ms=total_ms,
            ),
            failure_stage="extract_info",
        )

    postprocess_start = time.monotonic()
    if not isinstance(raw_info, dict):
        postprocess_ms = int((time.monotonic() - postprocess_start) * 1000)
        total_ms = int((time.monotonic() - total_start) * 1000)
        return OpusFormatLookup(
            info=None,
            metrics=OpusExtractMetrics(
                ydl_init_ms=ydl_init_ms,
                extract_info_ms=extract_info_ms,
                postprocess_ms=postprocess_ms,
                total_ms=total_ms,
            ),
            failure_stage="raw_info_type",
        )

    direct_url = str(raw_info.get("url", ""))
    fragments = _fragments_from_info(raw_info)
    acodec = str(raw_info.get("acodec", ""))
    ext = str(raw_info.get("ext", ""))
    container = str(raw_info.get("container", raw_info.get("protocol", "")))

    if not direct_url and not fragments:
        postprocess_ms = int((time.monotonic() - postprocess_start) * 1000)
        total_ms = int((time.monotonic() - total_start) * 1000)
        return OpusFormatLookup(
            info=None,
            metrics=OpusExtractMetrics(
                ydl_init_ms=ydl_init_ms,
                extract_info_ms=extract_info_ms,
                postprocess_ms=postprocess_ms,
                total_ms=total_ms,
            ),
            failure_stage="missing_stream_url",
        )
    if "opus" not in acodec.lower():
        postprocess_ms = int((time.monotonic() - postprocess_start) * 1000)
        total_ms = int((time.monotonic() - total_start) * 1000)
        return OpusFormatLookup(
            info=None,
            metrics=OpusExtractMetrics(
                ydl_init_ms=ydl_init_ms,
                extract_info_ms=extract_info_ms,
                postprocess_ms=postprocess_ms,
                total_ms=total_ms,
            ),
            failure_stage="non_opus_format",
        )

    info = OpusFormatInfo(
        video_id=video_id,
        direct_url=direct_url,
        fragments=fragments,
        http_headers=_headers_from_info(raw_info),
        format_id=str(raw_info.get("format_id", "")),
        ext=ext,
        acodec=acodec,
        container=container,
        abr=_as_float_or_none(raw_info.get("abr")),
        sample_rate=_as_int_or_none(raw_info.get("asr")),
        channels=_as_int_or_none(raw_info.get("audio_channels")),
        filesize=_as_int_or_none(raw_info.get("filesize") or raw_info.get("filesize_approx")),
        duration=_as_float_or_none(raw_info.get("duration")),
    )
    postprocess_ms = int((time.monotonic() - postprocess_start) * 1000)
    total_ms = int((time.monotonic() - total_start) * 1000)
    return OpusFormatLookup(
        info=info,
        metrics=OpusExtractMetrics(
            ydl_init_ms=ydl_init_ms,
            extract_info_ms=extract_info_ms,
            postprocess_ms=postprocess_ms,
            total_ms=total_ms,
        ),
        failure_stage=None,
    )


def extract_opus_format(video_id: str) -> OpusFormatInfo | None:
    return extract_opus_format_lookup(video_id).info
