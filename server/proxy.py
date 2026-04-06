import asyncio
import subprocess
import collections
import re
import os
import sys
import platform
import time
import urllib.request
from pathlib import Path
from datetime import datetime
from typing import AsyncGenerator

BASE_DIR: Path = Path(__file__).parent.resolve()
FFMPEG_PATH: str = str(BASE_DIR / ("ffmpeg.exe" if platform.system() == "Windows" else "ffmpeg"))

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

# Cache for seek format info.
# value: (direct_url, fragments, http_headers, timestamp)
_url_cache: dict[str, tuple[str, list[dict], dict[str, str], float]] = {}
URL_CACHE_TTL = 3600  # 1 hour


def add_log(msg: str) -> None:
    time_str: str = datetime.now().strftime("%H:%M:%S")
    log_msg: str = f"[{time_str}] {msg}"
    print(log_msg)
    app_logs.appendleft(log_msg)

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

def _extract_seek_info(v_id: str) -> tuple[str, list[dict], dict[str, str]]:
    """Extract seek info from yt-dlp: (direct_url, fragments, http_headers).
    For DASH content, fragments is a non-empty list of {url, duration} dicts.
    For progressive content, fragments is empty and direct_url is the full file URL.
    Cached for URL_CACHE_TTL seconds."""
    cached = _url_cache.get(v_id)
    if cached and time.time() - cached[3] < URL_CACHE_TTL:
        add_log(f"Seek info cache hit: {v_id}")
        return cached[0], cached[1], cached[2]

    yt_url = f"https://www.youtube.com/watch?v={v_id}"
    ydl_opts = {
        "format": "bestaudio[ext=m4a]/bestaudio/best",
        "quiet": True,
        "no_warnings": True,
        "socket_timeout": 10,
    }
    try:
        with yt_dlp.YoutubeDL(ydl_opts) as ydl:
            info = ydl.extract_info(yt_url, download=False)
        direct_url  = info.get("url", "")
        fragments   = info.get("fragments", [])
        http_headers: dict[str, str] = info.get("http_headers", {})
        ext = info.get("ext", "?")
        n_frags = len(fragments)
        _url_cache[v_id] = (direct_url, fragments, http_headers, time.time())
        add_log(f"Seek info cached: {v_id} (ext={ext} frags={n_frags})")
        return direct_url, fragments, http_headers
    except (yt_dlp.utils.DownloadError, OSError, ValueError) as e:
        add_log(f"Seek info extraction failed: {e}")
    return "", [], {}

async def _stream_seek(v_id: str, t: int) -> AsyncGenerator[bytes, None]:
    """Seek stream: extract seek info then pipe DASH or progressive content through ffmpeg."""
    url = f"https://www.youtube.com/watch?v={v_id}"
    direct_url, fragments, http_headers = await asyncio.to_thread(
        _extract_seek_info, v_id
    )

    r_fd, w_fd = os.pipe()
    ffmpeg_proc: asyncio.subprocess.Process | None = None
    ydl_proc_prog: asyncio.subprocess.Process | None = None

    if fragments:
        # DASH fMP4: manually fetch init (sq=0) + media segments from t.
        # YouTube CDN serves only ~37s per plain GET, so HTTP Range seek is
        # impossible. Instead we reconstruct the fMP4 stream from the right
        # segment and pipe it directly into ffmpeg.
        cumulative = 0.0
        start_idx = 0
        for i, frag in enumerate(fragments):
            dur = frag.get("duration", 0) or 0
            if cumulative + dur > t:
                start_idx = i
                break
            cumulative += dur

        first_url = fragments[0].get("url", direct_url)
        init_url  = re.sub(r"&sq=\d+", "&sq=0", first_url)

        add_log(f"Seek info: DASH {len(fragments)} frags, start_idx={start_idx} (~{cumulative:.0f}s)")

        def _fetch(fetch_url: str) -> bytes:
            req = urllib.request.Request(fetch_url, headers=http_headers)
            with urllib.request.urlopen(req, timeout=30) as resp:
                return resp.read()

        async def _feed_dash() -> None:
            try:
                with os.fdopen(w_fd, "wb") as pipe_out:
                    # init segment (ftyp + moov — codec metadata)
                    pipe_out.write(await asyncio.to_thread(_fetch, init_url))
                    # media segments from seek position
                    for frag in fragments[start_idx:]:
                        pipe_out.write(await asyncio.to_thread(_fetch, frag["url"]))
            except (BrokenPipeError, OSError):
                pass  # 3DS disconnected; pipe broken

        asyncio.create_task(_feed_dash())
    else:
        # Progressive: yt-dlp → pipe → ffmpeg output-seek (-ss after -i)
        ydl_cmd = [
            sys.executable, "-m", "yt_dlp",
            "-f", "bestaudio[ext=m4a]/bestaudio/best",
            "--quiet", "--no-warnings", "-o", "-", url
        ]
        ydl_proc_prog = await asyncio.create_subprocess_exec(
            *ydl_cmd, stdout=w_fd, stderr=subprocess.DEVNULL
        )
        os.close(w_fd)
        w_fd = -1

    ffmpeg_cmd = [
        FFMPEG_PATH,
        "-i", "pipe:0",
        *([] if fragments else ["-ss", str(t)]),
        "-f", "mp3", "-ar", "44100", "-ac", "2", "-b:a", "96k",
        "pipe:1"
    ]
    label = "DASH-frags" if fragments else "progressive-pipe"

    try:
        ffmpeg_proc = await asyncio.create_subprocess_exec(
            *ffmpeg_cmd,
            stdin=r_fd,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL
        )
        os.close(r_fd)
        r_fd = -1

        assert ffmpeg_proc.stdout is not None
        add_log(f"Seek stream started: {v_id} t={t}s ({label})")
        while True:
            data = await ffmpeg_proc.stdout.read(32768)
            if not data:
                break
            yield data
    except asyncio.CancelledError:
        add_log(f"Seek stream disconnected: {v_id}")
    except (BrokenPipeError, ConnectionResetError):
        add_log(f"Seek stream socket closed: {v_id}")
    except (OSError, RuntimeError) as e:
        add_log(f"Seek stream error: {e}")
    finally:
        if w_fd >= 0:
            try: os.close(w_fd)
            except OSError: pass
        if r_fd >= 0:
            try: os.close(r_fd)
            except OSError: pass
        if ffmpeg_proc:
            try: ffmpeg_proc.kill()
            except OSError: pass
            await ffmpeg_proc.wait()
        if ydl_proc_prog:
            try: ydl_proc_prog.kill()
            except OSError: pass
            await ydl_proc_prog.wait()


async def _stream_normal(v_id: str) -> AsyncGenerator[bytes, None]:
    """Normal streaming: yt-dlp → pipe → ffmpeg."""
    url = f"https://www.youtube.com/watch?v={v_id}"
    ydl_cmd = [
        sys.executable, "-m", "yt_dlp",
        "-f", "bestaudio/best",
        "--quiet", "--no-warnings",
        "-o", "-",
        url
    ]
    ffmpeg_cmd = [
        FFMPEG_PATH,
        "-i", "pipe:0",
        "-f", "mp3", "-ar", "44100", "-ac", "2", "-b:a", "96k",
        "pipe:1"
    ]

    r_fd, w_fd = os.pipe()
    ydl_proc = None
    ffmpeg_proc = None
    try:
        ydl_proc = await asyncio.create_subprocess_exec(
            *ydl_cmd,
            stdout=w_fd,
            stderr=subprocess.DEVNULL
        )
        os.close(w_fd)
        w_fd = -1

        ffmpeg_proc = await asyncio.create_subprocess_exec(
            *ffmpeg_cmd,
            stdin=r_fd,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL
        )
        os.close(r_fd)
        r_fd = -1

        assert ffmpeg_proc.stdout is not None
        add_log(f"Pipe streaming started: {v_id}")
        while True:
            data = await ffmpeg_proc.stdout.read(32768)
            if not data:
                break
            yield data
    except asyncio.CancelledError:
        add_log(f"Stream disconnected by 3DS: {v_id}")
    except (BrokenPipeError, ConnectionResetError):
        add_log(f"Stream socket closed: {v_id}")
    except (OSError, RuntimeError) as e:
        add_log(f"Unexpected stream error: {e}")
    finally:
        if w_fd >= 0:
            try: os.close(w_fd)
            except OSError: pass
        if r_fd >= 0:
            try: os.close(r_fd)
            except OSError: pass
        for proc in filter(None, [ffmpeg_proc, ydl_proc]):
            try:
                proc.kill()
            except OSError:
                pass
        if ffmpeg_proc:
            await ffmpeg_proc.wait()
        if ydl_proc:
            await ydl_proc.wait()


async def stream_audio_generator(v_id: str, t: int = 0) -> AsyncGenerator[bytes, None]:
    add_log(f"Stream requested: {v_id}" + (f" (seek {t}s)" if t > 0 else ""))
    if t > 0:
        async for chunk in _stream_seek(v_id, t):
            yield chunk
    else:
        async for chunk in _stream_normal(v_id):
            yield chunk

async def stream(request: Request) -> StreamingResponse | PlainTextResponse:
    i = request.query_params.get("i", "")
    # Security: YouTube video IDs are 11 alphanumeric chars (plus hyphen/underscore).
    # Reject anything else (e.g. OS commands) with 400 error.
    if not i or not isinstance(i, str) or not re.match(r'^[a-zA-Z0-9_\-]{11}$', i):
        add_log(f"Blocked invalid stream ID: {i}")
        return PlainTextResponse("Invalid video ID format", status_code=400)

    t_str = request.query_params.get("t", "0")
    try:
        t = int(t_str)
        if t < 0 or t > 86400:
            t = 0
    except ValueError:
        t = 0

    return StreamingResponse(stream_audio_generator(i, t), media_type="audio/mpeg")

async def thumbnail(request: Request) -> Response | PlainTextResponse:
    vid = request.query_params.get("id", "")
    if not vid or not re.match(r'^[a-zA-Z0-9_\-]{11}$', vid):
        add_log(f"Blocked invalid thumbnail ID: {vid}")
        return PlainTextResponse("Invalid video ID format", status_code=400)

    url = f"https://i.ytimg.com/vi/{vid}/mqdefault.jpg"

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
    Route("/stream", stream),
    Route("/thumbnail", thumbnail),
    Route("/api/logs", get_logs),
    Route("/", dashboard)
]

app = Starlette(debug=False, routes=routes)

if __name__ == "__main__":
    ip = get_local_ip()
    add_log(f"Starlette Server started on http://{ip}:{PORT}")
    add_log(f"You can now open http://127.0.0.1:{PORT} in your browser to view the dashboard.")
    uvicorn.run("proxy:app", host="0.0.0.0", port=PORT, log_level="warning")