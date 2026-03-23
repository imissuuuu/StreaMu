#!/usr/bin/env python3
"""Cross-platform setup script for 3DS Music Player proxy server."""

import sys
import subprocess
import platform
from pathlib import Path


def main() -> None:
    print("=== 3DS Music Player - Proxy Server Setup ===\n")

    # Check Python version
    if sys.version_info < (3, 10):
        print(f"[ERROR] Python 3.10+ is required. You have {sys.version}")
        sys.exit(1)
    print(f"[OK] Python {sys.version_info.major}.{sys.version_info.minor}")

    base_dir: Path = Path(__file__).parent.resolve()
    venv_dir: Path = base_dir / "venv"
    is_windows: bool = platform.system() == "Windows"

    # Create venv
    if venv_dir.exists():
        print(f"[OK] venv already exists: {venv_dir}")
    else:
        print("[...] Creating virtual environment...")
        try:
            subprocess.run([sys.executable, "-m", "venv", str(venv_dir)], check=True)
        except subprocess.CalledProcessError:
            print("[ERROR] Failed to create virtual environment.")
            sys.exit(1)
        print(f"[OK] venv created: {venv_dir}")

    # Determine pip path inside venv
    if is_windows:
        pip_path: Path = venv_dir / "Scripts" / "pip.exe"
        python_path: Path = venv_dir / "Scripts" / "python.exe"
    else:
        pip_path = venv_dir / "bin" / "pip"
        python_path = venv_dir / "bin" / "python"

    # Install dependencies
    req_file: Path = base_dir / "requirements.txt"
    print("[...] Installing dependencies...")
    try:
        subprocess.run([str(pip_path), "install", "-r", str(req_file)], check=True)
    except subprocess.CalledProcessError:
        print("[ERROR] Failed to install dependencies. Check your network connection.")
        sys.exit(1)
    print("[OK] Dependencies installed")

    # Check ffmpeg
    ffmpeg_name: str = "ffmpeg.exe" if is_windows else "ffmpeg"
    ffmpeg_local: Path = base_dir / ffmpeg_name
    if ffmpeg_local.exists():
        print(f"[OK] ffmpeg found: {ffmpeg_local}")
    else:
        print(f"\n[WARNING] ffmpeg not found in project directory.")
        print(f"  The proxy server needs ffmpeg to transcode audio for the 3DS.")
        print(f"  Please download ffmpeg and place '{ffmpeg_name}' in:")
        print(f"    {base_dir}")
        print(f"  Download: https://ffmpeg.org/download.html")

    # Done
    print("\n=== Setup Complete ===\n")
    print("To start the proxy server:")
    print(f"  {python_path} proxy.py")
    print(f"\nThe server will start on port 8080.")
    print("Enter this PC's IP address on your 3DS when prompted.")


if __name__ == "__main__":
    main()
