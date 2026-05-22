# Third-Party Licenses

This project code is licensed under the MIT License. See `LICENSE`.

StreaMu also uses the following third-party components. Their licenses apply to
the corresponding components, libraries, and bundled runtime files.

## Windows Server Package

`StreaMu-Server.exe` is a PyInstaller-built executable containing the proxy
server and its Python runtime dependencies.

| Component | Description | License | URL |
| --- | --- | --- | --- |
| Python | Python runtime embedded by PyInstaller | PSF License | https://www.python.org/ |
| PyInstaller | Windows executable packaging and bootloader | GPL-2.0-or-later with PyInstaller bootloader exception | https://pyinstaller.org/ |
| yt-dlp | YouTube video/audio data extraction Python package | Unlicense | https://github.com/yt-dlp/yt-dlp |
| Starlette | Lightweight ASGI framework for the proxy server | BSD 3-Clause | https://github.com/encode/starlette |
| Uvicorn | ASGI server for running the proxy | BSD 3-Clause | https://github.com/encode/uvicorn |
| AnyIO | Async compatibility layer used by Starlette | MIT | https://github.com/agronholm/anyio |
| Click | Command line utility package used by Uvicorn | BSD 3-Clause | https://github.com/pallets/click |
| h11 | HTTP/1.1 protocol implementation used by Uvicorn | MIT | https://github.com/python-hyper/h11 |
| idna | Internationalized domain name support | BSD 3-Clause | https://github.com/kjd/idna |
| colorama | Windows console color support used by Uvicorn | BSD 3-Clause | https://github.com/tartley/colorama |
| certifi | CA certificate bundle used by Python networking packages | MPL-2.0 | https://github.com/certifi/python-certifi |

The v1.5.0 Windows package does not bundle FFmpeg or a separate `yt-dlp.exe`.

## Nintendo 3DS Application

The 3DS app is built with devkitPro and links against the following libraries.

| Component | Description | License | URL |
| --- | --- | --- | --- |
| devkitPro / libctru | Homebrew development toolchain and Nintendo 3DS library | zlib / ISC | https://github.com/devkitPro/libctru |
| citro2d / citro3d | 2D/3D rendering libraries for Nintendo 3DS | zlib | https://github.com/devkitPro/citro2d |
| libcurl | HTTP client library | curl license | https://curl.se/libcurl/ |
| mbed TLS | TLS and cryptography library used by libcurl | Apache-2.0 | https://github.com/Mbed-TLS/mbedtls |
| zlib | Compression library | zlib | https://zlib.net/ |
| libogg | Ogg container library | BSD-style | https://github.com/xiph/ogg |
| libopus | Opus audio codec library | BSD 3-Clause | https://github.com/xiph/opus |
| opusfile | Ogg Opus decoder helper library | BSD 3-Clause | https://github.com/xiph/opusfile |
| Tremor / libvorbisidec | Integer-only Vorbis decoder | BSD-style | https://github.com/xiph/tremor |
| nlohmann/json | Header-only JSON library | MIT | https://github.com/nlohmann/json |
| stb_image | Header-only image loading library | MIT or Public Domain | https://github.com/nothings/stb |

## Content and Services

StreaMu uses `yt-dlp` to resolve YouTube media URLs. Users are responsible for
using StreaMu with content and services they are permitted to access.
