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
- Windows / Mac / Linux
- **EXE version (recommended):** No dependencies — just download and run
- **Python version:** Python 3.10+

### Quick Start

1. Download `streamu.cia` (or `.3dsx`) and `StreaMu-Server.zip` from [Releases](../../releases)
2. Install the CIA on your 3DS with FBI
3. Extract `StreaMu-Server.zip` and run the EXE inside
4. Launch StreaMu on your 3DS and enter the IP address shown on the server dashboard
5. Search for music with the Y button and enjoy!

### Install (3DS)

Download `streamu.cia` or `streamu.3dsx` from [Releases](../../releases).

- **CIA**: Install with FBI. The app appears on the HOME Menu.
- **3DSX**: Copy to `/3ds/` on your SD card. Launch from Homebrew Launcher.

### Proxy Server Setup

#### Option A: Standalone EXE (Recommended)

Download `StreaMu-Server.zip` from [Releases](../../releases), extract it, and run `StreaMu-Server.exe`.

The server uses native YouTube Opus audio and does not require FFmpeg. No Python or manual setup needed.

#### Option B: Python Script

Mac / Linux:
```bash
cd server
chmod +x start_server.sh
./start_server.sh
```

This will:
1. Create a Python virtual environment
2. Install required packages (starlette, yt-dlp, uvicorn)
3. Start the proxy server

Windows:
```bash
cd server
python setup.py
venv\Scripts\python proxy.py
```

Or double-click `start_server.bat` in the `server/` folder.

The server starts on port 8080. Open `http://localhost:8080` in your browser to view the dashboard.

### Usage

1. Start the proxy server on your PC
2. Launch StreaMu on your 3DS
3. On first launch, enter your PC's IP address (shown on the proxy dashboard)
4. Press **Y** to open the search keyboard and find music
5. From search results, tap a track to start playback
6. Use the **+ Add** button or the track options menu to save tracks to a playlist

### Playlists

- **Creating a playlist:** From search results, tap the **menu icon** on a track and select **Add to Playlist**. Choose **New Playlist** to create one, or select an existing playlist.
- **Browsing playlists:** Tap the **Playlists** tab to see all your saved playlists. Tap a playlist to view its tracks.
- **Playing from a playlist:** Open a playlist and tap any track to start playback. The playlist becomes your play queue.
- **Renaming tracks:** In the track options menu, select **Rename** to give a track a custom name.

### Custom Wallpaper

You can set a custom image for the top screen background:

1. Place PNG files in `sdmc:/3ds/StreaMu/wallpaper/` on your SD card
2. Open **Settings** in the app
3. Select **Wallpaper** and choose from your images

### Settings

Accessible from the settings icon in the app:

| Setting | Description |
|---------|-------------|
| Theme | Light / Dark mode |
| Accent Color | Adjust hue, saturation, and brightness |
| Color Palette | Choose a preset palette |
| L/R Button | Assign actions: skip track, play/pause |
| D-Pad Speed | Scroll speed for list navigation |
| Wallpaper | Top screen background image |
| Language | English / Japanese |

Settings are saved to `sdmc:/3ds/StreaMu/config.json`.

### Controls

| Button | Action |
|--------|--------|
| Y | Open search keyboard |
| A | Select / Confirm |
| B | Back / Close popup |
| D-Pad | Navigate lists and menus |
| L/R | Switch tabs (configurable) |
| Select | Open track options menu |
| Start | Exit app |
| Touch | Tap items, scroll lists, tap buttons |

### Troubleshooting

**"Connection Error (Timeout)"**
- Make sure your 3DS and PC are on the **same Wi-Fi network**
- Check that the server is running — open `http://localhost:8080` in your PC's browser to confirm
- Try the IP address shown on the server dashboard (e.g. `192.168.x.x`)
- **Windows Firewall:** You may need to allow port 8080. Go to Windows Defender Firewall > Advanced Settings > Inbound Rules > New Rule > Port > TCP 8080 > Allow

**Server won't start**
- **EXE version:** Make sure Windows Defender or antivirus isn't blocking the EXE. You may need to add an exception.
- **Windows Python version:** Run `python setup.py` first to install dependencies.
- **Mac/Linux Python version:** Run `./start_server.sh` from the `server/` folder. It creates the virtual environment and installs dependencies automatically.

**No audio / playback issues**
- StreaMu uses Opus Direct only. Make sure the proxy server is current and that `/stream_opus_ogg` is reachable from the 3DS.

### Build from Source

Requires [devkitPro](https://devkitpro.org/wiki/Getting_Started) with `3ds-dev` package group.

```bash
make clean && make      # produces streamu.3dsx
make cia                # produces streamu.cia
```

To build the server EXE:
```bash
cd server
build_exe.bat           # produces dist/StreaMu-Server.zip
```

### License

StreaMu project code is MIT licensed. See [LICENSE](LICENSE) for details.
Bundled third-party components are governed by their own licenses.

Third-party licenses: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)

Development references: StreaMu's audio design was informed by public 3DS
homebrew projects such as ctrmus, Video_player_for_3DS, FourthTube, and
pomegranate. Their code is not bundled in StreaMu.

---

## 日本語

### 概要

Nintendo 3DS（カスタムファームウェア導入済み）で動作する音楽プレイヤーです。YouTubeから楽曲を検索・ストリーミング再生できます。

PC上で動作するプロキシサーバーがYouTubeからのデータ取得と音声変換を担当します。

### 必要なもの

**3DS側:**
- カスタムファームウェア（Luma3DS）導入済みのNintendo 3DS

**プロキシサーバー側（PC）:**
- Windows / Mac / Linux
- **EXE版（推奨）：** 依存関係不要 — ダウンロードして実行するだけ
- **Python版：** Python 3.10以上

### クイックスタート

1. [Releases](../../releases) から `streamu.cia`（または `.3dsx`）と `StreaMu-Server.zip` をダウンロード
2. 3DSにCIAをFBIでインストール
3. `StreaMu-Server.zip` を解凍してEXEを実行
4. 3DSでStreaMuを起動し、サーバーダッシュボードに表示されるIPアドレスを入力
5. Yボタンで音楽を検索して再生！

### インストール（3DS）

[Releases](../../releases) から `streamu.cia` または `streamu.3dsx` をダウンロード。

- **CIA**: FBIでインストール。HOMEメニューにアプリが追加されます。
- **3DSX**: SDカードの `/3ds/` にコピー。Homebrew Launcherから起動。

### プロキシサーバーのセットアップ

#### 方法A: スタンドアロンEXE（推奨）

[Releases](../../releases) から `StreaMu-Server.zip` をダウンロードして解凍し、`StreaMu-Server.exe` を実行するだけです。

サーバーはYouTubeのOpus音声を直接利用し、FFmpegは不要です。Pythonやセットアップは不要です。

#### 方法B: Pythonスクリプト

Mac / Linux:
```bash
cd server
chmod +x start_server.sh
./start_server.sh
```

以下が自動で行われます：
1. Python仮想環境の作成
2. 必要パッケージのインストール（starlette, yt-dlp, uvicorn）
3. プロキシサーバーの起動

Windows:
```bash
cd server
python setup.py
venv\Scripts\python proxy.py
```

または `server/` フォルダ内の `start_server.bat` をダブルクリック。

サーバーはポート8080で起動します。ブラウザで `http://localhost:8080` を開くとダッシュボードが表示されます。

### 使い方

1. PCでプロキシサーバーを起動
2. 3DSでStreaMuを起動
3. 初回起動時、PCのIPアドレスを入力（プロキシのダッシュボードに表示されます）
4. **Y**ボタンで検索キーボードを開いて楽曲を検索
5. 検索結果からトラックをタップして再生開始
6. **+ Add** ボタンやトラックオプションメニューからプレイリストに保存

### プレイリスト

- **プレイリストの作成：** 検索結果でトラックの**メニューアイコン**をタップし、**Add to Playlist** を選択。**New Playlist** で新規作成、または既存のプレイリストを選択。
- **プレイリストの閲覧：** **Playlists** タブをタップすると保存済みのプレイリスト一覧が表示されます。
- **プレイリストから再生：** プレイリストを開いてトラックをタップすると再生開始。そのプレイリストが再生キューになります。
- **トラック名の変更：** トラックオプションメニューから **Rename** を選択。

### カスタム壁紙

上画面の背景にカスタム画像を設定できます：

1. SDカードの `sdmc:/3ds/StreaMu/wallpaper/` にPNGファイルを配置
2. アプリ内の**設定**を開く
3. **Wallpaper** から画像を選択

### 設定

アプリ内の設定アイコンからアクセス：

| 設定項目 | 説明 |
|----------|------|
| Theme | ライト / ダークモード |
| Accent Color | 色相・彩度・明度の調整 |
| Color Palette | プリセットカラーパレットの選択 |
| L/R Button | ボタン割り当て: スキップ、再生/一時停止 |
| D-Pad Speed | リストのスクロール速度 |
| Wallpaper | 上画面の背景画像 |
| Language | English / 日本語 |

設定は `sdmc:/3ds/StreaMu/config.json` に保存されます。

### 操作方法

| ボタン | 動作 |
|--------|------|
| Y | 検索キーボードを開く |
| A | 選択 / 決定 |
| B | 戻る / ポップアップを閉じる |
| 十字キー | リスト・メニューの移動 |
| L/R | タブ切替（設定で変更可） |
| Select | トラックオプションメニューを開く |
| Start | アプリ終了 |
| タッチ | 項目タップ、リストスクロール、ボタン操作 |

### トラブルシューティング

**「Connection Error (Timeout)」が表示される**
- 3DSとPCが**同じWi-Fiネットワーク**に接続されているか確認
- サーバーが起動しているか確認 — PCのブラウザで `http://localhost:8080` を開いてダッシュボードが表示されればOK
- サーバーダッシュボードに表示されるIPアドレス（例: `192.168.x.x`）を使用
- **Windowsファイアウォール：** ポート8080を許可する必要がある場合があります。Windows Defender ファイアウォール > 詳細設定 > 受信の規則 > 新しい規則 > ポート > TCP 8080 > 許可

**サーバーが起動しない**
- **EXE版：** Windows Defenderやウイルス対策ソフトがブロックしていないか確認。例外に追加が必要な場合があります。
- **Windows Python版：** 先に `python setup.py` を実行して依存関係をインストールしてください。
- **Mac/Linux Python版：** `server/` フォルダで `./start_server.sh` を実行してください。仮想環境の作成と依存関係のインストールは自動で行われます。

**音声が出ない・再生できない**
- StreaMu は Opus Direct のみを使用します。プロキシサーバーが最新で、3DS から `/stream_opus_ogg` に接続できることを確認してください。

### ソースからビルド

[devkitPro](https://devkitpro.org/wiki/Getting_Started) の `3ds-dev` パッケージグループが必要です。

```bash
make clean && make      # streamu.3dsx を生成
make cia                # streamu.cia を生成
```

サーバーEXEのビルド:
```bash
cd server
build_exe.bat           # dist/StreaMu-Server.zip を生成
```

### ライセンス

StreaMu 本体コードは MIT License です。詳細は [LICENSE](LICENSE) を参照。
同梱しているサードパーティコンポーネントには、それぞれ別のライセンスが適用されます。

サードパーティライセンス: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)

開発時の参考: StreaMu の音声設計では ctrmus、Video_player_for_3DS、
FourthTube、pomegranate などの公開3DS homebrewプロジェクトを参考にしました。
これらのコードは StreaMu には同梱していません。
