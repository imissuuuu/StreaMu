# RDD - 2026-05-17
**Project Name:** StreaMu FFmpegless Audio PoC

## 1. 機能一覧
- FR-1: StreaMu のサーバー側 FFmpeg 依存を削減または除去できるか検証する PoC を行う。
- FR-2: サーバーは yt-dlp を使って YouTube の音声ストリーム情報を抽出する。
- FR-3: PoC では YouTube の `mp4a` / AAC 系音声を優先して扱う。
- FR-4: 3DS 側で AAC を直接デコードして ndsp に流せるか検証する。
- FR-5: 既存の MP3 / FFmpeg 経路は壊さず、PoC 経路は明示的に切り替えられる形にする。
- FR-6: Android APK 版で FFmpeg を同梱しなくても動かせる可能性を検証対象に含める。
- FR-7: PoC 結果として、FFmpeg 完全削除、オプション化、または現状維持のどれが妥当か判断できる材料を残す。

## 2. 非機能要件
- NFR-1: 3DS 側のメモリ使用量を急増させない。
- NFR-2: 既存 MP3 再生の安定性を維持する。
- NFR-3: YouTube 抽出の壊れやすい部分はサーバー側 yt-dlp に残し、3DS 側へ Innertube / client spoofing / PO-token 対応を持ち込まない。
- NFR-4: 実機テストはユーザーが手動で行う。Codex はエミュレータ起動や adb 操作を行わない。
- NFR-5: 失敗時に既存の FFmpeg 経路へ戻せること。

## 3. 詳細仕様
- 入力:
  - 3DS 側で既存通り検索結果から曲を選択する。
  - PoC モードではサーバーから AAC 直接再生用のストリームを受け取る。
- 処理:
  - サーバーは yt-dlp で `bestaudio[acodec^=mp4a]` などを抽出する。
  - サーバーは FFmpeg で MP3 へ変換せず、AAC / M4A の直接URLまたはバイト列を 3DS に渡す方式を検証する。
  - 3DS 側は既存 `MP3Player` と分離した AAC PoC プレイヤーまたは抽象化層でデコードを試す。
  - AAC デコード方式は調査・設計段階で選定する。候補は libavcodec, miniaudio 系, dr_libs 系, または FourthTube 由来の実装パターン参照。
  - まずは seek なしの通常再生を対象にし、seek は PoC 成功後に検討する。
- 出力:
  - 成功時は FFmpeg を使わずに 3DS で音声再生できる。
  - 失敗時は失敗理由を分類し、Android APK への FFmpeg 同梱に戻るか、別デコーダを試すか判断できる。

## 4. UI/UX考慮事項
- 通常ユーザー向け UI は原則変更しない。
- PoC 中に設定項目を追加する場合は、既定を現行 MP3 / FFmpeg 経路にする。
- 失敗時は再生エラーとして扱い、既存の操作導線を壊さない。

## 5. 追加設計情報
- 現行構成:
  - `server/proxy.py` が yt-dlp と FFmpeg で MP3 を生成し、`/stream` が `audio/mpeg` を返す。
  - 3DS 側 `MP3Player` は `minimp3` で MP3 フレームをデコードし、ndsp に PCM を渡す。
- PoC の推奨順:
  - サーバー側に AAC 情報取得用の実験エンドポイントを追加する。
  - 3DS 側に AAC デコード候補を小さく組み込み、ローカルまたは短いストリームで再生可否を確認する。
  - ネットワークストリーム再生へ広げる。
- 範囲外:
  - 完全 proxyless 化。
  - 3DS 側 Innertube 直叩き。
  - ログイン必須動画対応。
  - 動画再生。
  - seek 完全対応。
