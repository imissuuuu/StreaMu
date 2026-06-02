import asyncio
import collections
import re
import time
import urllib.parse
import urllib.request
from dataclasses import dataclass
from datetime import datetime
from typing import Any, AsyncGenerator, Iterator

from opus_probe import (
    OpusExtractMetrics,
    OpusFormatInfo,
    extract_opus_format_lookup,
)
from webm_opus_remux import (
    WebmOpusRemuxError,
    iter_ogg_opus_pages_from_webm_chunks,
)

from starlette.applications import Starlette
from starlette.routing import Route
from starlette.responses import StreamingResponse, PlainTextResponse, HTMLResponse, JSONResponse, Response
from starlette.requests import Request
import yt_dlp # type: ignore
import uvicorn # type: ignore
import socket

PORT = 8080
MAX_LOGS = 50
app_logs: collections.deque[str] = collections.deque(maxlen=MAX_LOGS)

_opus_info_cache: dict[str, tuple[OpusFormatInfo, float]] = {}
URL_CACHE_TTL = 3600  # 1 hour
MAX_OPUS_WEBM_BYTES = 64 * 1024 * 1024
OPUS_THUMBNAIL_DELAY_BYTES = 16 * 1024
OPUS_OGG_PAGE_BATCH_BYTES = 16 * 1024
OPUS_OGG_PAGE_BATCH_MAX = 64
OPUS_THUMBNAIL_DELAY_TIMEOUT_SEC = 3.0
OPUS_SEEK_PREROLL_SECONDS = 15
OPUS_SEEK_RANGE_BACKTRACK_BYTES = 1024 * 1024
OPUS_SEEK_CLUSTER_SCAN_BYTES = 1024 * 1024
WEBM_CLUSTER_ID = b"\x1f\x43\xb6\x75"
_opus_prebuffer_videos: set[str] = set()


@dataclass(frozen=True)
class OpusPerfRequestContext:
    route: str
    video_id: str
    request_id: str
    start_monotonic: float


@dataclass(frozen=True)
class OpusInfoLookupResult:
    info: OpusFormatInfo | None
    cache_hit: bool
    lookup_ms: int
    extract_metrics: OpusExtractMetrics
    failure_stage: str | None


def add_log(msg: str) -> None:
    time_str: str = datetime.now().strftime("%H:%M:%S")
    log_msg: str = f"[{time_str}] {msg}"
    print(log_msg)
    app_logs.appendleft(log_msg)


def _begin_opus_perf_request(route: str,
                             video_id: str) -> OpusPerfRequestContext:
    request_id = f"{time.monotonic_ns() & 0xFFFFFF:06x}"
    return OpusPerfRequestContext(
        route=route,
        video_id=video_id,
        request_id=request_id,
        start_monotonic=time.monotonic(),
    )


def _perf_elapsed_ms(ctx: OpusPerfRequestContext) -> int:
    return int((time.monotonic() - ctx.start_monotonic) * 1000)


def _format_perf_field_value(value: object) -> str:
    if isinstance(value, bool):
        return str(value).lower()
    return str(value).replace(" ", "_")


def _log_opus_perf(ctx: OpusPerfRequestContext,
                   event: str,
                   **fields: object) -> None:
    parts: list[str] = [
        "Opus perf",
        f"route={ctx.route}",
        f"id={ctx.video_id}",
        f"req={ctx.request_id}",
        f"+{_perf_elapsed_ms(ctx)}ms",
        f"event={event}",
    ]
    for key, value in fields.items():
        parts.append(f"{key}={_format_perf_field_value(value)}")
    add_log(" ".join(parts))


def _lookup_cached_or_extract_opus_info(v_id: str) -> OpusInfoLookupResult:
    lookup_start = time.monotonic()
    cached = _opus_info_cache.get(v_id)
    if cached and time.time() - cached[1] < URL_CACHE_TTL:
        return OpusInfoLookupResult(
            info=cached[0],
            cache_hit=True,
            lookup_ms=int((time.monotonic() - lookup_start) * 1000),
            extract_metrics=OpusExtractMetrics(
                ydl_init_ms=0,
                extract_info_ms=0,
                postprocess_ms=0,
                total_ms=0,
            ),
            failure_stage=None,
        )

    lookup = extract_opus_format_lookup(v_id)
    if lookup.info is not None:
        _opus_info_cache[v_id] = (lookup.info, time.time())

    return OpusInfoLookupResult(
        info=lookup.info,
        cache_hit=False,
        lookup_ms=int((time.monotonic() - lookup_start) * 1000),
        extract_metrics=lookup.metrics,
        failure_stage=lookup.failure_stage,
    )

def get_local_ip() -> str:
    try:
        s: socket.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip: str = s.getsockname()[0]
        s.close()
        return ip
    except (socket.error, OSError):
        return "127.0.0.1"

def format_duration(seconds: int | float | None) -> str:
    """Convert seconds to mm:ss or h:mm:ss format"""
    if seconds is None or seconds <= 0:
        return "?"
    total = int(seconds)  # Ensure integer for formatting
    h = total // 3600
    m = (total % 3600) // 60
    s = total % 60
    if h > 0:
        return f"{h}:{m:02d}:{s:02d}"
    return f"{m}:{s:02d}"

def format_views(count: int | float | None, lang: str = "en") -> str:
    """Format view count to human-readable string (e.g. 1.2M views / 120万回)"""
    if count is None or count <= 0:
        return "?"
    c = int(count)  # Ensure integer for formatting
    if lang == "ja":
        if c >= 100_000_000:
            return f"{c / 100_000_000:.1f}億回".replace(".0億回", "億回")
        if c >= 10_000:
            return f"{c / 10_000:.1f}万回".replace(".0万回", "万回")
        return f"{c:,}回"
    # English (default)
    if c >= 1_000_000_000:
        return f"{c / 1_000_000_000:.1f}B views".replace(".0B views", "B views")
    if c >= 1_000_000:
        return f"{c / 1_000_000:.1f}M views".replace(".0M views", "M views")
    if c >= 1_000:
        return f"{c / 1_000:.1f}K views".replace(".0K views", "K views")
    return f"{c:,} views"

def format_relative_date(upload_date_str: str | None) -> str:
    """Convert YYYYMMDD to relative date string (e.g. '3y ago', '2mo ago')"""
    if not upload_date_str or len(upload_date_str) != 8:
        return "?"
    try:
        upload = datetime(int(upload_date_str[:4]), int(upload_date_str[4:6]), int(upload_date_str[6:8]))
        now = datetime.now()
        diff = now - upload
        days = diff.days
        if days < 0:
            return "?"
        if days == 0:
            return "Today"
        if days == 1:
            return "1d ago"
        if days < 7:
            return f"{days}d ago"
        if days < 30:
            weeks = days // 7
            return f"{weeks}w ago"
        if days < 365:
            months = days // 30
            return f"{months}mo ago"
        years = days // 365
        return f"{years}y ago"
    except (ValueError, TypeError):
        return "?"

def search_youtube(query: str, lang: str = "en") -> str:
    ydl_opts = {
        'extract_flat': 'in_playlist',
        'max_downloads': 10,
        'quiet': True,
        'default_search': 'ytsearch10',
        'socket_timeout': 10,
    }
    with yt_dlp.YoutubeDL(ydl_opts) as ydl:
        try:
            info = ydl.extract_info(query, download=False)
            output = ""
            if 'entries' in info:
                for entry in info['entries']:
                    # Filter out live streams and archives (title-based)
                    title = entry.get('title', '').replace('\t', ' ').replace('\n', ' ')
                    live_status = entry.get('live_status', '')
                    title_lower = title.lower()
                    if live_status in ('is_live', 'is_upcoming', 'post_live'):
                        continue
                    if any(kw in title_lower for kw in ['\u3010live\u3011', '\u3010生放送\u3011', 'live stream', '配信中', '生配信']):
                        continue
                    
                    vid = entry.get('id', '')
                    duration = format_duration(entry.get('duration'))
                    views = format_views(entry.get('view_count'), lang)
                    uploader = entry.get('channel', entry.get('uploader', 'Unknown')).replace('\t', ' ').replace('\n', ' ')
                    upload_date = ''  # Not available with extract_flat
                    
                    output += f"{vid}\t{title}\t{duration}\t{views}\t{uploader}\t{upload_date}\n"
            add_log(f"Search completed: '{query}' ({len(info.get('entries', []))} results)")
            return output
        except yt_dlp.utils.DownloadError as e:
            add_log(f"Search ERROR (Download): {e}")
            return ""
        except ValueError as e:
            add_log(f"Search ERROR (Value): {e}")
            return ""

async def search(request: Request) -> PlainTextResponse:
    q = request.query_params.get("q", "")
    lang = request.query_params.get("lang", "en")
    # Sanitize and length limit: reject queries over 100 chars to prevent DoS/overflow
    if not q or len(q) > 100:
        add_log(f"Blocked invalid search query: {q[:20]}...")
        return PlainTextResponse("Invalid or too long query", status_code=400)

    add_log(f"Search requested: {q} (lang={lang})")
    output = await asyncio.to_thread(search_youtube, q, lang)
    return PlainTextResponse(output)

async def opus_info(request: Request) -> JSONResponse | PlainTextResponse:
    v_id = request.query_params.get("i", "")
    if not v_id or not isinstance(v_id, str) or not re.match(r'^[a-zA-Z0-9_\-]{11}$', v_id):
        add_log(f"Blocked invalid Opus info ID: {v_id}")
        return PlainTextResponse("Invalid video ID format", status_code=400)

    ctx = _begin_opus_perf_request("opus_info", v_id)
    _log_opus_perf(ctx, "request-start")

    lookup = await asyncio.to_thread(_lookup_cached_or_extract_opus_info, v_id)
    if lookup.info is None:
        _log_opus_perf(
            ctx,
            "info-failed",
            cache_hit=lookup.cache_hit,
            lookup_ms=lookup.lookup_ms,
            ydl_init_ms=lookup.extract_metrics.ydl_init_ms,
            extract_info_ms=lookup.extract_metrics.extract_info_ms,
            postprocess_ms=lookup.extract_metrics.postprocess_ms,
            extract_total_ms=lookup.extract_metrics.total_ms,
            failure_stage=lookup.failure_stage or "unknown",
        )
        return PlainTextResponse("Opus extraction failed", status_code=502)

    _log_opus_perf(
        ctx,
        "info-ready",
        cache_hit=lookup.cache_hit,
        lookup_ms=lookup.lookup_ms,
        ydl_init_ms=lookup.extract_metrics.ydl_init_ms,
        extract_info_ms=lookup.extract_metrics.extract_info_ms,
        postprocess_ms=lookup.extract_metrics.postprocess_ms,
        extract_total_ms=lookup.extract_metrics.total_ms,
        fragments=len(lookup.info.fragments),
        ext=lookup.info.ext,
        acodec=lookup.info.acodec,
    )
    _log_opus_perf(ctx, "response-ready")
    return JSONResponse(lookup.info.to_json_dict())

def _get_cached_or_extract_opus_info(v_id: str) -> OpusFormatInfo | None:
    return _lookup_cached_or_extract_opus_info(v_id).info


def _clamp_seek_seconds(value: str | None) -> int:
    try:
        seek_seconds = int(value or "0")
    except ValueError:
        return 0
    if seek_seconds < 0 or seek_seconds > 86400:
        return 0
    return seek_seconds


def _fragment_start_index(fragments: list[dict[str, Any]],
                          seek_seconds: int) -> tuple[int, float]:
    if seek_seconds <= 0 or not fragments:
        return 0, 0.0

    cumulative = 0.0
    for index, fragment in enumerate(fragments):
        duration = fragment.get("duration", 0) or 0
        try:
            duration_seconds = float(duration)
        except (TypeError, ValueError):
            return 0, 0.0
        if duration_seconds <= 0:
            return 0, 0.0
        if cumulative + duration_seconds > seek_seconds:
            return index, cumulative
        cumulative += duration_seconds

    return max(0, len(fragments) - 1), cumulative


def _fragment_duration_seconds(fragment: dict[str, Any]) -> float:
    duration = fragment.get("duration", 0) or 0
    try:
        return float(duration)
    except (TypeError, ValueError):
        return 0.0


def _fragment_url(base_url: str, fragment: dict[str, Any]) -> str:
    fragment_url = str(fragment.get("url") or "")
    if fragment_url.startswith("http://") or fragment_url.startswith("https://"):
        return fragment_url
    return urllib.parse.urljoin(base_url, fragment_url)


def _webm_header_prefix(data: bytes) -> bytes:
    cluster_offset = data.find(WEBM_CLUSTER_ID)
    if cluster_offset <= 0:
        return data
    return data[:cluster_offset]


def _opus_seek_range_start(info: OpusFormatInfo,
                           seek_seconds: int) -> int | None:
    if seek_seconds <= 0 or not info.filesize or not info.duration:
        return None
    if info.filesize <= 0 or info.duration <= 0:
        return None
    start_seconds = max(0.0, float(seek_seconds - OPUS_SEEK_PREROLL_SECONDS))
    estimated = int((start_seconds / info.duration) * info.filesize)
    return max(0, estimated - OPUS_SEEK_RANGE_BACKTRACK_BYTES)


def _fetch_url_bytes(fetch_url: str, headers: dict[str, str],
                     timeout: int = 30) -> bytes:
    req = urllib.request.Request(fetch_url, headers=headers)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def _fetch_url_bytes_limited(fetch_url: str, headers: dict[str, str],
                             max_bytes: int,
                             timeout: int = 60) -> bytes:
    req = urllib.request.Request(fetch_url, headers=headers)
    chunks: list[bytes] = []
    total = 0
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        while True:
            chunk = resp.read(65536)
            if not chunk:
                break
            total += len(chunk)
            if total > max_bytes:
                raise ValueError("response too large")
            chunks.append(chunk)
    return b"".join(chunks)


def _fetch_url_range(fetch_url: str, headers: dict[str, str], start: int,
                     end: int | None = None, timeout: int = 30) -> bytes:
    if start < 0:
        raise ValueError("negative range start")
    range_headers = dict(headers)
    if end is None:
        range_headers["Range"] = f"bytes={start}-"
    else:
        if end < start:
            raise ValueError("invalid byte range")
        range_headers["Range"] = f"bytes={start}-{end}"
    req = urllib.request.Request(fetch_url, headers=range_headers)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


async def opus_webm_generator(v_id: str) -> AsyncGenerator[bytes, None]:
    info = await asyncio.to_thread(_get_cached_or_extract_opus_info, v_id)
    if info is None:
        add_log(f"Opus WebM extraction failed: {v_id}")
        return
    if not info.direct_url:
        add_log(f"Opus WebM fragmented stream unsupported: {v_id}")
        return

    add_log(f"Opus WebM stream started: {v_id} ({info.ext}/{info.acodec})")
    try:
        req = urllib.request.Request(info.direct_url, headers=info.http_headers)
        with urllib.request.urlopen(req, timeout=300) as resp:
            while True:
                chunk = resp.read(32768)
                if not chunk:
                    break
                yield chunk
    except asyncio.CancelledError:
        add_log(f"Opus WebM stream disconnected: {v_id}")
    except OSError as e:
        add_log(f"Opus WebM stream error: {e}")

async def stream_opus(request: Request) -> StreamingResponse | PlainTextResponse:
    v_id = request.query_params.get("i", "")
    if not v_id or not isinstance(v_id, str) or not re.match(r'^[a-zA-Z0-9_\-]{11}$', v_id):
        add_log(f"Blocked invalid Opus stream ID: {v_id}")
        return PlainTextResponse("Invalid video ID format", status_code=400)

    info = await asyncio.to_thread(_get_cached_or_extract_opus_info, v_id)
    if info is None:
        return PlainTextResponse("Opus extraction failed", status_code=502)
    if not info.direct_url:
        return PlainTextResponse("Opus fragmented stream unsupported", status_code=501)
    _opus_info_cache[v_id] = (info, time.time())
    return StreamingResponse(opus_webm_generator(v_id), media_type="audio/webm")


def _next_ogg_page_batch_or_none(iterator: Iterator[bytes]) -> list[bytes] | None:
    pages: list[bytes] = []
    total = 0
    while len(pages) < OPUS_OGG_PAGE_BATCH_MAX and total < OPUS_OGG_PAGE_BATCH_BYTES:
        try:
            page = next(iterator)
        except StopIteration:
            break
        pages.append(page)
        total += len(page)
    if not pages:
        return None
    return pages


async def opus_ogg_generator(
    ctx: OpusPerfRequestContext,
    info: OpusFormatInfo,
    seek_seconds: int = 0,
) -> AsyncGenerator[bytes, None]:
    upstream_first_byte_logged = False

    def webm_chunks() -> Iterator[bytes]:
        nonlocal upstream_first_byte_logged
        total = 0

        def read_url(fetch_url: str) -> Iterator[bytes]:
            nonlocal upstream_first_byte_logged
            nonlocal total
            req = urllib.request.Request(fetch_url, headers=info.http_headers)
            with urllib.request.urlopen(req, timeout=300) as resp:
                while True:
                    chunk = resp.read(16384)
                    if not chunk:
                        break
                    total += len(chunk)
                    if total > MAX_OPUS_WEBM_BYTES:
                        raise ValueError("response too large")
                    if not upstream_first_byte_logged:
                        upstream_first_byte_logged = True
                        _log_opus_perf(
                            ctx,
                            "upstream-first-byte",
                            bytes=total,
                        )
                    yield chunk

        if info.fragments:
            init_fragments = [
                fragment for fragment in info.fragments
                if _fragment_duration_seconds(fragment) <= 0.0
            ]
            media_fragments = [
                fragment for fragment in info.fragments
                if _fragment_duration_seconds(fragment) > 0.0
            ]
            if seek_seconds > 0 and not init_fragments:
                start_idx, fragment_seconds = 0, 0.0
                add_log("Opus Ogg seek: no init fragment; streaming from first fragment")
            else:
                start_idx, fragment_seconds = _fragment_start_index(
                    media_fragments, seek_seconds
                )
            add_log(
                f"Opus Ogg seek info: start_idx={start_idx} "
                f"fragment_time={fragment_seconds:.1f}s"
            )
            for fragment in init_fragments:
                yield from read_url(_fragment_url(info.direct_url, fragment))
            for fragment in media_fragments[start_idx:]:
                yield from read_url(_fragment_url(info.direct_url, fragment))
            return

        range_start = _opus_seek_range_start(info, seek_seconds)
        if range_start is not None:
            try:
                header = _fetch_url_range(
                    info.direct_url, info.http_headers, 0, (512 * 1024) - 1
                )
                header_prefix = _webm_header_prefix(header)
                range_headers = dict(info.http_headers)
                range_headers["Range"] = f"bytes={range_start}-"
                req = urllib.request.Request(info.direct_url, headers=range_headers)
                with urllib.request.urlopen(req, timeout=300) as resp:
                    pending = bytearray()
                    while True:
                        chunk = resp.read(16384)
                        if not chunk:
                            raise ValueError("Opus seek cluster not found")
                        total += len(chunk)
                        if total > MAX_OPUS_WEBM_BYTES:
                            raise ValueError("response too large")
                        if not upstream_first_byte_logged:
                            upstream_first_byte_logged = True
                            _log_opus_perf(
                                ctx,
                                "upstream-first-byte",
                                bytes=total,
                            )
                        pending.extend(chunk)
                        cluster_offset = pending.find(WEBM_CLUSTER_ID)
                        if cluster_offset >= 0:
                            yield header_prefix
                            yield bytes(pending[cluster_offset:])
                            break
                        if len(pending) > OPUS_SEEK_CLUSTER_SCAN_BYTES:
                            raise ValueError("Opus seek cluster not found")
                    while True:
                        chunk = resp.read(16384)
                        if not chunk:
                            break
                        total += len(chunk)
                        if total > MAX_OPUS_WEBM_BYTES:
                            raise ValueError("response too large")
                        if not upstream_first_byte_logged:
                            upstream_first_byte_logged = True
                            _log_opus_perf(
                                ctx,
                                "upstream-first-byte",
                                bytes=total,
                            )
                        yield chunk
                    return
            except (OSError, ValueError) as e:
                add_log(f"Opus Ogg seek range fallback: {e}")

        yield from read_url(info.direct_url)

    _opus_info_cache[ctx.video_id] = (info, time.time())
    _log_opus_perf(
        ctx,
        "stream-start",
        seek=seek_seconds,
        fragments=len(info.fragments),
        ext=info.ext,
        acodec=info.acodec,
    )
    _opus_prebuffer_videos.add(ctx.video_id)
    sent_bytes = 0
    page_count = 0
    first_page_logged = False
    sixteen_k_logged = False
    page_iterator = iter_ogg_opus_pages_from_webm_chunks(
        webm_chunks(), seek_start_ms=seek_seconds * 1000
    )
    try:
        while True:
            page_batch = await asyncio.to_thread(
                _next_ogg_page_batch_or_none, page_iterator
            )
            if page_batch is None:
                break
            for page in page_batch:
                page_count += 1
                if not first_page_logged:
                    first_page_logged = True
                    _log_opus_perf(
                        ctx,
                        "first-ogg-page",
                        bytes=len(page),
                    )
                sent_bytes += len(page)
                yield page
                if (
                    not sixteen_k_logged
                    and sent_bytes >= OPUS_THUMBNAIL_DELAY_BYTES
                ):
                    sixteen_k_logged = True
                    _log_opus_perf(
                        ctx,
                        "sent-16kb",
                        pages=page_count,
                        sent=sent_bytes,
                    )
                if sent_bytes >= OPUS_THUMBNAIL_DELAY_BYTES:
                    _opus_prebuffer_videos.discard(ctx.video_id)
    except asyncio.CancelledError:
        add_log(f"Opus Ogg stream disconnected: {ctx.video_id}")
        _log_opus_perf(ctx, "disconnected", sent=sent_bytes, pages=page_count)
        raise
    except ValueError as e:
        add_log(f"Opus Ogg stream rejected: {e}")
        _log_opus_perf(
            ctx,
            "rejected",
            error_type=type(e).__name__,
            sent=sent_bytes,
            pages=page_count,
        )
    except (OSError, WebmOpusRemuxError) as e:
        add_log(f"Opus Ogg stream failed: {e}")
        _log_opus_perf(
            ctx,
            "failed",
            error_type=type(e).__name__,
            sent=sent_bytes,
            pages=page_count,
        )
    else:
        add_log(f"Opus Ogg stream finished: {ctx.video_id}")
        _log_opus_perf(
            ctx,
            "finished",
            sent=sent_bytes,
            pages=page_count,
        )
    finally:
        _opus_prebuffer_videos.discard(ctx.video_id)


async def stream_opus_ogg(request: Request) -> StreamingResponse | PlainTextResponse:
    v_id = request.query_params.get("i", "")
    if not v_id or not isinstance(v_id, str) or not re.match(r'^[a-zA-Z0-9_\-]{11}$', v_id):
        add_log(f"Blocked invalid Opus Ogg stream ID: {v_id}")
        return PlainTextResponse("Invalid video ID format", status_code=400)
    t = _clamp_seek_seconds(request.query_params.get("t", "0"))

    ctx = _begin_opus_perf_request("stream_opus_ogg", v_id)
    _log_opus_perf(ctx, "request-start", seek=t)

    lookup = await asyncio.to_thread(_lookup_cached_or_extract_opus_info, v_id)
    if lookup.info is None:
        _log_opus_perf(
            ctx,
            "info-failed",
            cache_hit=lookup.cache_hit,
            lookup_ms=lookup.lookup_ms,
            ydl_init_ms=lookup.extract_metrics.ydl_init_ms,
            extract_info_ms=lookup.extract_metrics.extract_info_ms,
            postprocess_ms=lookup.extract_metrics.postprocess_ms,
            extract_total_ms=lookup.extract_metrics.total_ms,
            failure_stage=lookup.failure_stage or "unknown",
            seek=t,
        )
        return PlainTextResponse("Opus extraction failed", status_code=502)
    if not lookup.info.direct_url:
        _log_opus_perf(
            ctx,
            "info-unsupported",
            cache_hit=lookup.cache_hit,
            lookup_ms=lookup.lookup_ms,
            ydl_init_ms=lookup.extract_metrics.ydl_init_ms,
            extract_info_ms=lookup.extract_metrics.extract_info_ms,
            postprocess_ms=lookup.extract_metrics.postprocess_ms,
            extract_total_ms=lookup.extract_metrics.total_ms,
            seek=t,
        )
        return PlainTextResponse("Opus fragmented remux unsupported", status_code=501)

    _log_opus_perf(
        ctx,
        "info-ready",
        cache_hit=lookup.cache_hit,
        lookup_ms=lookup.lookup_ms,
        ydl_init_ms=lookup.extract_metrics.ydl_init_ms,
        extract_info_ms=lookup.extract_metrics.extract_info_ms,
        postprocess_ms=lookup.extract_metrics.postprocess_ms,
        extract_total_ms=lookup.extract_metrics.total_ms,
        seek=t,
        fragments=len(lookup.info.fragments),
        ext=lookup.info.ext,
        acodec=lookup.info.acodec,
    )

    return StreamingResponse(
        opus_ogg_generator(ctx, lookup.info, t),
        media_type="audio/ogg",
    )

async def thumbnail(request: Request) -> Response | PlainTextResponse:
    vid = request.query_params.get("id", "")
    if not vid or not re.match(r'^[a-zA-Z0-9_\-]{11}$', vid):
        add_log(f"Blocked invalid thumbnail ID: {vid}")
        return PlainTextResponse("Invalid video ID format", status_code=400)

    url = f"https://i.ytimg.com/vi/{vid}/mqdefault.jpg"
    if vid in _opus_prebuffer_videos:
        deadline = time.monotonic() + OPUS_THUMBNAIL_DELAY_TIMEOUT_SEC
        while vid in _opus_prebuffer_videos and time.monotonic() < deadline:
            await asyncio.sleep(0.05)

    def fetch_image() -> bytes:
        with urllib.request.urlopen(url, timeout=8) as resp:
            return resp.read()

    try:
        data = await asyncio.to_thread(fetch_image)
        if len(data) > 512 * 1024:
            return PlainTextResponse("Image too large", status_code=502)
        add_log(f"Thumbnail served: {vid}")
        return Response(content=data, media_type="image/jpeg")
    except Exception as e:
        add_log(f"Thumbnail fetch ERROR for {vid}: {e}")
        return PlainTextResponse("Failed to fetch thumbnail", status_code=502)

async def get_logs(request: Request) -> JSONResponse:
    return JSONResponse({"logs": list(app_logs)})

async def healthz(request: Request) -> PlainTextResponse:
    return PlainTextResponse("ok")

async def dashboard(request: Request) -> HTMLResponse:
    ip: str = get_local_ip()
    html_content: str = f"""
    <!DOCTYPE html>
    <html lang="en">
    <head>
        <meta charset="UTF-8">
        <title>3DS Music Player Dashboard</title>
        <meta name="viewport" content="width=device-width, initial-scale=1">
        <style>
            body {{ font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background-color: #f5f6fa; color: #2f3640; margin: 0; padding: 20px; }}
            .container {{ max-width: 800px; margin: 0 auto; background: white; padding: 30px; border-radius: 12px; box-shadow: 0 4px 15px rgba(0,0,0,0.05); }}
            h1 {{ color: #192a56; margin-top: 0; border-bottom: 2px solid #f1f2f6; padding-bottom: 10px; }}
            .status-panel {{ display: flex; justify-content: space-between; background: #e8f4f8; padding: 15px; border-radius: 8px; margin-bottom: 20px; flex-wrap: wrap; gap: 10px; }}
            .status {{ color: #44bd32; font-weight: bold; display: flex; align-items: center; font-size: 1.1em; }}
            .status::before {{ content: ''; display: inline-block; width: 12px; height: 12px; background: #44bd32; border-radius: 50%; margin-right: 8px; box-shadow: 0 0 5px #44bd32; }}
            .info-item {{ font-size: 14px; color: #718093; margin-top: 5px; }}
            .ip-box {{ background: #192a56; color: white; padding: 5px 10px; border-radius: 5px; font-weight: bold; font-family: monospace; font-size: 1.2em; display: inline-block; margin-top: 5px; }}
            h2 {{ color: #273c75; font-size: 18px; margin-top: 30px; display: flex; align-items: center; justify-content: space-between; }}
            .badge {{ background: #2f3640; color: white; padding: 2px 8px; border-radius: 10px; font-size: 12px; }}
            .log-container {{ background: #2f3640; color: #f5f6fa; padding: 15px; border-radius: 8px; height: 350px; overflow-y: auto; font-family: 'Consolas', monospace; font-size: 13px; line-height: 1.6; border-left: 4px solid #4bcffa; }}
            .log-entry {{ border-bottom: 1px solid #353b48; padding: 6px 0; word-break: break-all; }}
            .log-entry:last-child {{ border-bottom: none; }}
            .auto-scroll-label {{ font-size: 12px; color: #718093; cursor: pointer; display: flex; align-items: center; gap: 5px; }}
        </style>
        <script>
            let autoScroll = true;
            async function fetchLogs() {{
                try {{
                    // Add cache busting query to avoid stale IE/Edge responses
                    const response = await fetch('/api/logs?t=' + new Date().getTime());
                    const data = await response.json();
                    const logContainer = document.getElementById('log-container');
                    
                    const wasAtBottom = logContainer.scrollHeight - logContainer.scrollTop <= logContainer.clientHeight + 10;

                    let html = '';
                    if (data.logs.length === 0) {{
                        html = '<div class="log-entry" style="color:#7f8fa6;">Waiting for activities...</div>';
                    }} else {{
                        const reversedLogs = [...data.logs].reverse();
                        reversedLogs.forEach(log => {{
                            html += `<div class="log-entry">${{log}}</div>`;
                        }});
                    }}
                    logContainer.innerHTML = html;
                    
                    if (autoScroll && wasAtBottom) {{
                        logContainer.scrollTop = logContainer.scrollHeight;
                    }}
                }} catch (e) {{
                    console.error('Failed to fetch logs:', e);
                }}
            }}
            setInterval(fetchLogs, 1000);
            window.onload = () => {{
                fetchLogs();
                document.getElementById('auto-scroll').addEventListener('change', (e) => {{
                    autoScroll = e.target.checked;
                }});
            }};
        </script>
    </head>
    <body>
        <div class="container">
            <h1>3DS Music Player Dashboard</h1>
            <div class="status-panel">
                <div>
                    <span class="status">Server Running (Starlette)</span>
                    <div class="info-item">Port: {PORT}</div>
                    <div class="info-item">Process: Extremely Lightweight Async mode</div>
                </div>
                <div style="text-align: right;">
                    <div class="info-item" style="text-align: left;">3DS Connect Address:</div>
                    <div class="ip-box">http://{ip}:{PORT}</div>
                </div>
            </div>
            
            <h2>
                <span>Real-time Activity Log <span class="badge">Live</span></span>
                <label class="auto-scroll-label">
                    <input type="checkbox" id="auto-scroll" checked> Auto-scroll
                </label>
            </h2>
            <div class="log-container" id="log-container">
                <div style="color: #7f8fa6;">Loading logs...</div>
            </div>
        </div>
    </body>
    </html>
    """
    return HTMLResponse(content=html_content)

routes = [
    Route("/search", search),
    Route("/api/opus-info", opus_info),
    Route("/stream_opus", stream_opus),
    Route("/stream_opus_ogg", stream_opus_ogg),
    Route("/thumbnail", thumbnail),
    Route("/healthz", healthz),
    Route("/api/logs", get_logs),
    Route("/", dashboard)
]

app = Starlette(debug=False, routes=routes)

if __name__ == "__main__":
    try:
        print("=== StreaMu Server ===")
        ip = get_local_ip()
        add_log(f"Starlette Server started on http://{ip}:{PORT}")
        add_log(f"You can now open http://127.0.0.1:{PORT} in your browser to view the dashboard.")
        uvicorn.run(app, host="0.0.0.0", port=PORT, log_level="warning")
    except Exception as e:
        print(f"\n[ERROR] {e}")
        input("Press Enter to exit...")
