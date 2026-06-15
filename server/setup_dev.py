#!/usr/bin/env python3
"""Set up proxy server development dependencies."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from setup_common import ensure_python_version, ensure_venv, install_requirements


def main() -> None:
    print("=== 3DS Music Player - Proxy Server Dev Setup ===\n")

    base_dir = Path(__file__).parent.resolve()
    ensure_python_version()
    pip_path, python_path = ensure_venv(base_dir)

    runtime_req_file = base_dir / "requirements.txt"
    dev_req_file = base_dir / "requirements-dev.txt"

    install_requirements(pip_path, runtime_req_file, "runtime")
    install_requirements(pip_path, dev_req_file, "development")

    print("\n=== Dev Setup Complete ===\n")
    print("Runtime server entrypoint:")
    print(f"  {python_path} proxy.py")
    print("\nStatic analysis tools are now installed in the same venv.")


if __name__ == "__main__":
    main()
