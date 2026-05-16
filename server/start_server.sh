#!/usr/bin/env sh
set -eu

print_header() {
    printf '%s\n' '=============================================='
    printf '%s\n' ' StreaMu - Proxy Server'
    printf '%s\n' '=============================================='
    printf '\n'
}

find_python() {
    if command -v python3 >/dev/null 2>&1; then
        PYTHON_CMD="$(command -v python3)"
        return 0
    fi
    if command -v python >/dev/null 2>&1; then
        PYTHON_CMD="$(command -v python)"
        return 0
    fi

    printf '%s\n' '[ERROR] Python 3.10+ was not found.'
    printf '%s\n' 'Install Python 3.10 or newer, then run this script again.'
    exit 1
}

check_python_version() {
    if "$PYTHON_CMD" -c 'import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)' >/dev/null 2>&1; then
        version="$("$PYTHON_CMD" -c 'import sys; print(f"{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}")')"
        printf '[OK] Python %s\n' "$version"
        return 0
    fi

    version="$("$PYTHON_CMD" -c 'import sys; print(sys.version.split()[0])' 2>/dev/null || printf 'unknown')"
    printf '[ERROR] Python 3.10+ is required. Found: %s\n' "$version"
    exit 1
}

ensure_venv() {
    if [ -x "venv/bin/python" ]; then
        printf '%s\n' '[OK] venv already exists'
        return 0
    fi

    printf '%s\n' '[INFO] Creating virtual environment...'
    if ! "$PYTHON_CMD" -m venv venv; then
        printf '%s\n' '[ERROR] Failed to create the virtual environment.'
        printf '%s\n' 'On Debian/Ubuntu, install the venv package first:'
        printf '%s\n' '  sudo apt install python3-venv'
        exit 1
    fi
    printf '%s\n' '[OK] venv created'
}

install_dependencies() {
    printf '%s\n' '[INFO] Installing Python dependencies...'
    "venv/bin/python" -m pip install --upgrade pip
    "venv/bin/python" -m pip install -r requirements.txt
    printf '%s\n' '[OK] Dependencies ready'
}

print_ffmpeg_help() {
    printf '%s\n' '[ERROR] ffmpeg was not found.'
    printf '%s\n' 'Install ffmpeg, then run this script again.'
    printf '\n'

    case "$(uname -s)" in
        Darwin)
            printf '%s\n' 'macOS with Homebrew:'
            printf '%s\n' '  brew install ffmpeg'
            ;;
        *)
            printf '%s\n' 'Debian / Ubuntu:'
            printf '%s\n' '  sudo apt install ffmpeg'
            printf '\n'
            printf '%s\n' 'Fedora:'
            printf '%s\n' '  sudo dnf install ffmpeg'
            printf '\n'
            printf '%s\n' 'Arch Linux:'
            printf '%s\n' '  sudo pacman -S ffmpeg'
            ;;
    esac

    printf '\n'
    printf '%s\n' 'You can also place an executable named ffmpeg in this server folder.'
}

check_ffmpeg() {
    if [ -x "./ffmpeg" ]; then
        printf '%s\n' '[OK] ffmpeg found in server folder'
        return 0
    fi

    if command -v ffmpeg >/dev/null 2>&1; then
        ffmpeg_path="$(command -v ffmpeg)"
        if ln -sf "$ffmpeg_path" ./ffmpeg >/dev/null 2>&1; then
            printf '[OK] ffmpeg found: %s\n' "$ffmpeg_path"
            return 0
        fi

        printf '[ERROR] ffmpeg was found at %s, but the local link could not be created.\n' "$ffmpeg_path"
        printf '%s\n' 'Copy or link it manually as ./ffmpeg, then run this script again.'
        exit 1
    fi

    print_ffmpeg_help
    exit 1
}

start_server() {
    printf '\n'
    printf '%s\n' '[INFO] Starting the server...'
    printf '%s\n' 'You can press Ctrl+C to stop it.'
    printf '\n'
    exec "venv/bin/python" proxy.py
}

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
cd "$SCRIPT_DIR"

PYTHON_CMD=""

print_header
find_python
check_python_version
ensure_venv
install_dependencies
check_ffmpeg
start_server
