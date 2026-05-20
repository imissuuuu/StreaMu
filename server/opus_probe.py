from __future__ import annotations

from dataclasses import asdict, dataclass
from typing import Any

import yt_dlp  # type: ignore


OPUS_FORMAT_SELECTOR = (
    "bestaudio[acodec*=opus][ext=webm]/"
    "bestaudio[acodec*=opus]"
)


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


def extract_opus_format(video_id: str) -> OpusFormatInfo | None:
    yt_url = f"https://www.youtube.com/watch?v={video_id}"
    ydl_opts: dict[str, Any] = {
        "format": OPUS_FORMAT_SELECTOR,
        "quiet": True,
        "no_warnings": True,
        "socket_timeout": 10,
    }

    try:
        with yt_dlp.YoutubeDL(ydl_opts) as ydl:
            raw_info = ydl.extract_info(yt_url, download=False)
    except (yt_dlp.utils.DownloadError, OSError, ValueError):
        return None

    if not isinstance(raw_info, dict):
        return None

    direct_url = str(raw_info.get("url", ""))
    fragments = _fragments_from_info(raw_info)
    acodec = str(raw_info.get("acodec", ""))
    ext = str(raw_info.get("ext", ""))
    container = str(raw_info.get("container", raw_info.get("protocol", "")))

    if not direct_url and not fragments:
        return None
    if "opus" not in acodec.lower():
        return None

    return OpusFormatInfo(
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
        filesize=_as_int_or_none(
            raw_info.get("filesize") or raw_info.get("filesize_approx")
        ),
        duration=_as_float_or_none(raw_info.get("duration")),
    )
