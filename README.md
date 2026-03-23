# StreaMu

A homebrew music player for Nintendo 3DS that searches and streams YouTube audio via a companion proxy server.

[English](#english) | [日本語](#日本語)

---

## English

### Overview

This app runs on a Nintendo 3DS (with custom firmware) and lets you search for music on YouTube, browse playlists, and stream audio — all from the 3DS's dual-screen interface.

A lightweight proxy server runs on your PC and handles YouTube data fetching and audio transcoding.

### Requirements

**3DS side:**
- Nintendo 3DS with custom firmware (Luma3DS)

**Proxy server side (PC):**
- Python 3.10+
- FFmpeg

### Install (3DS)

Download `streamu.cia` or `streamu.3dsx` from Releases.

- **CIA**: Install with FBI. The app appears on the HOME Menu.
- **3DSX**: Copy to `/3ds/` on your SD card. Launch from Homebrew Launcher.

### Proxy Server Setup

```bash
cd server
python setup.py
```

This will:
1. Create a Python virtual environment
2. Install required packages (starlette, yt-dlp, uvicorn)
3. Check for FFmpeg

If FFmpeg is not found, download it from [ffmpeg.org](https://ffmpeg.org/download.html) and place the binary in the `server/` directory.

### Start the Server

**Windows:**
```bash
cd server
venv\Scripts\python proxy.py
```

Or double-click `start_server.bat` in the `server/` folder.

**Mac / Linux:**
```bash
cd server
venv/bin/python proxy.py
```

The server starts on port 8080. Open `http://localhost:8080` in your browser to view the dashboard.

### Usage

1. Start the proxy server on your PC
2. Launch StreaMu on your 3DS
3. On first launch, enter your PC's IP address (shown on the proxy dashboard)
4. Search for music with the Y button
5. Browse and play from your playlists

### Controls

| Button | Action |
|--------|--------|
| Y | Open search keyboard |
| A | Select / Confirm |
| B | Back / Close popup |
| D-Pad | Navigate lists and menus |
| L/R | Switch tabs (configurable) |
| Touch | Tap items, scroll lists, tap buttons |

### Build from Source

Requires [devkitPro](https://devkitpro.org/wiki/Getting_Started) with `3ds-dev` package group.

```bash
make clean && make      # produces streamu.3dsx
make cia                # produces streamu.cia
```

### License

MIT License. See [LICENSE](LICENSE) for details.

Third-party licenses: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)

---

## 日本語

### 概要

Nintendo 3DS（カスタムファームウェア導入済み）で動作する音楽プレイヤーです。YouTubeから楽曲を検索・ストリーミング再生できます。

PC上で動作するプロキシサーバーがYouTubeからのデータ取得と音声変換を担当します。

### 必要なもの

**3DS側:**
- カスタムファームウェア（Luma3DS）導入済みのNintendo 3DS

**プロキシサーバー側（PC）:**
- Python 3.10以上
- FFmpeg

### インストール（3DS）

Releasesから `streamu.cia` または `streamu.3dsx` をダウンロード。

- **CIA**: FBIでインストール。HOMEメニューにアプリが追加されます。
- **3DSX**: SDカードの `/3ds/` にコピー。Homebrew Launcherから起動。

### プロキシサーバーのセットアップ

```bash
cd server
python setup.py
```

以下が自動で行われます：
1. Python仮想環境の作成
2. 必要パッケージのインストール（starlette, yt-dlp, uvicorn）
3. FFmpegの確認

FFmpegが見つからない場合は [ffmpeg.org](https://ffmpeg.org/download.html) からダウンロードし、`server/` ディレクトリに配置してください。

### サーバーの起動

**Windows:**
```bash
cd server
venv\Scripts\python proxy.py
```

または `server/` フォルダ内の `start_server.bat` をダブルクリック。

**Mac / Linux:**
```bash
cd server
venv/bin/python proxy.py
```

サーバーはポート8080で起動します。ブラウザで `http://localhost:8080` を開くとダッシュボードが表示されます。

### 使い方

1. PCでプロキシサーバーを起動
2. 3DSでStreaMuを起動
3. 初回起動時、PCのIPアドレスを入力（プロキシのダッシュボードに表示されます）
4. Yボタンで楽曲を検索
5. プレイリストから再生

### 操作方法

| ボタン | 動作 |
|--------|------|
| Y | 検索キーボードを開く |
| A | 選択 / 決定 |
| B | 戻る / ポップアップを閉じる |
| 十字キー | リスト・メニューの移動 |
| L/R | タブ切替（設定で変更可） |
| タッチ | 項目タップ、リストスクロール、ボタン操作 |

### ソースからビルド

[devkitPro](https://devkitpro.org/wiki/Getting_Started) の `3ds-dev` パッケージグループが必要です。

```bash
make clean && make      # streamu.3dsx を生成
make cia                # streamu.cia を生成
```

### ライセンス

MIT License。詳細は [LICENSE](LICENSE) を参照。

サードパーティライセンス: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)
