#!/usr/bin/env python3
"""Cross-platform setup script for 3DS Music Player proxy server."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from setup_common import ensure_python_version, ensure_venv, install_requirements


def main() -> None:
    print("=== 3DS Music Player - Proxy Server Setup ===\n")

    base_dir: Path = Path(__file__).parent.resolve()
    ensure_python_version()
    pip_path, python_path = ensure_venv(base_dir)

    # Install dependencies
    req_file: Path = base_dir / "requirements.txt"
    install_requirements(pip_path, req_file, "runtime")

    # Done
    print("\n=== Setup Complete ===\n")
    print("To start the proxy server:")
    print(f"  {python_path} proxy.py")
    print("\nThe server will start on port 8080.")
    print("Enter this PC's IP address on your 3DS when prompted.")


if __name__ == "__main__":
    main()
