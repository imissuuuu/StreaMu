# Release Forum Post Template

Use this template for manual GBATemp release posts. Replace placeholders before
posting.

```text
Hey everyone, StreaMu v{VERSION} is out!

This release is mostly a {RELEASE_TYPE} release for {FOCUS_AREA}. {MAIN_CHANGE}

{PRACTICAL_CHANGE}

Download:
https://github.com/imissuuuu/StreaMu/releases/tag/v{VERSION}

Notes:
- {NOTE_1}
- {NOTE_2}
- {NOTE_3}

Please let me know if you run into any issues!
```

## v1.5.0 Example

```text
Hey everyone, StreaMu v1.5.0 is out!

This release is mostly a cleanup/stability release for the new audio path. StreaMu now uses Opus Direct as the only playback path, so the old MP3 proxy path and server-side FFmpeg transcoding have been removed.

The big practical change is that the PC server no longer needs FFmpeg. Just update both the 3DS app and StreaMu-Server.zip together, then run the new server as usual.

Download:
https://github.com/imissuuuu/StreaMu/releases/tag/v1.5.0

Notes:
- Older server builds will not work with this version because the app now expects the Opus/Ogg streaming endpoint.
- Old audio path settings such as mp3 or aac_direct are automatically treated as Opus Direct.
- If a specific video does not play, please include the video ID and the server dashboard log when reporting it.

Please let me know if you run into any issues!
```
