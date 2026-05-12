"""Dependency auto-download for Windows (ffmpeg, yt-dlp)."""
import os
import sys
import io
import platform
import zipfile
import urllib.request
from pathlib import Path

IS_WINDOWS: bool = platform.system() == "Windows"

FFMPEG_URL = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip"
YTDLP_URL = "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe"


def get_app_dir() -> Path:
    if getattr(sys, 'frozen', False):
        return Path(sys.executable).parent.resolve()
    return Path(__file__).parent.resolve()


def get_ffmpeg_path() -> str:
    return str(get_app_dir() / ("ffmpeg.exe" if IS_WINDOWS else "ffmpeg"))


def ensure_ffmpeg() -> str:
    ffmpeg_path = get_ffmpeg_path()
    if os.path.isfile(ffmpeg_path):
        return ffmpeg_path

    if not IS_WINDOWS:
        print("[ERROR] ffmpeg not found. Install via your package manager (e.g. apt install ffmpeg).")
        sys.exit(1)

    print("[INFO] ffmpeg not found. Downloading...")
    _download_ffmpeg(get_app_dir())
    if not os.path.isfile(ffmpeg_path):
        print("[ERROR] ffmpeg download failed.")
        print("  Please download manually from https://ffmpeg.org/download.html")
        print(f"  and place ffmpeg.exe in: {get_app_dir()}")
        sys.exit(1)
    print("[INFO] ffmpeg ready.")
    return ffmpeg_path


def _download_ffmpeg(dest_dir: Path) -> None:
    try:
        req = urllib.request.Request(FFMPEG_URL)
        with urllib.request.urlopen(req, timeout=120) as resp:
            total = int(resp.headers.get('Content-Length', 0))
            downloaded = 0
            chunks: list[bytes] = []
            while True:
                chunk = resp.read(1024 * 256)
                if not chunk:
                    break
                chunks.append(chunk)
                downloaded += len(chunk)
                if total > 0:
                    pct = downloaded * 100 // total
                    mb = downloaded // (1024 * 1024)
                    print(f"\r[INFO] Downloading ffmpeg ({mb} MB)... {pct}%", end="", flush=True)
            print()

        data = b"".join(chunks)
        with zipfile.ZipFile(io.BytesIO(data)) as zf:
            for name in zf.namelist():
                if name.endswith("/bin/ffmpeg.exe"):
                    with zf.open(name) as src, open(dest_dir / "ffmpeg.exe", "wb") as dst:
                        dst.write(src.read())
                    return
        print("[ERROR] ffmpeg.exe not found in downloaded archive.")
    except (urllib.error.URLError, OSError, zipfile.BadZipFile) as e:
        print(f"[ERROR] Download failed: {e}")


def get_ytdlp_path() -> str:
    return str(get_app_dir() / "yt-dlp.exe")


def ensure_ytdlp() -> str:
    ytdlp_path = get_ytdlp_path()
    if os.path.isfile(ytdlp_path):
        return ytdlp_path

    print("[INFO] yt-dlp.exe not found. Downloading...")
    try:
        req = urllib.request.Request(YTDLP_URL)
        with urllib.request.urlopen(req, timeout=120) as resp:
            total = int(resp.headers.get('Content-Length', 0))
            downloaded = 0
            chunks: list[bytes] = []
            while True:
                chunk = resp.read(1024 * 256)
                if not chunk:
                    break
                chunks.append(chunk)
                downloaded += len(chunk)
                if total > 0:
                    pct = downloaded * 100 // total
                    mb = downloaded // (1024 * 1024)
                    print(f"\r[INFO] Downloading yt-dlp ({mb} MB)... {pct}%", end="", flush=True)
            print()
        with open(ytdlp_path, "wb") as f:
            f.write(b"".join(chunks))
        print("[INFO] yt-dlp ready.")
    except (urllib.error.URLError, OSError) as e:
        print(f"[WARNING] yt-dlp download failed: {e}")
        print("  Falling back to built-in mode (slower streaming).")
        return ""
    return ytdlp_path
