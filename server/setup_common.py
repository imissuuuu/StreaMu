#!/usr/bin/env python3
"""Shared helpers for proxy server environment setup."""

import platform
import subprocess
import sys
from pathlib import Path


def ensure_python_version() -> None:
    if sys.version_info < (3, 10):
        print(f"[ERROR] Python 3.10+ is required. You have {sys.version}")
        sys.exit(1)
    print(f"[OK] Python {sys.version_info.major}.{sys.version_info.minor}")


def ensure_venv(base_dir: Path) -> tuple[Path, Path]:
    venv_dir = base_dir / "venv"
    is_windows = platform.system() == "Windows"

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

    if is_windows:
        pip_path = venv_dir / "Scripts" / "pip.exe"
        python_path = venv_dir / "Scripts" / "python.exe"
    else:
        pip_path = venv_dir / "bin" / "pip"
        python_path = venv_dir / "bin" / "python"

    return pip_path, python_path


def install_requirements(pip_path: Path, req_file: Path, label: str) -> None:
    print(f"[...] Installing {label} dependencies...")
    try:
        subprocess.run([str(pip_path), "install", "-r", str(req_file)], check=True)
    except subprocess.CalledProcessError:
        print(f"[ERROR] Failed to install {label} dependencies. Check your network connection.")
        sys.exit(1)
    print(f"[OK] {label} dependencies installed")
