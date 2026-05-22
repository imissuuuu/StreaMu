# DESIGN - 2026-05-16
**Project Name:** StreaMu Android Proxy Server APK
**RDD:** `docs/RDD/RDD-2026-05-16-android-apk.md`

## 1. Implementation Strategy
- Add a new Android project under `3ds-music-player/android/`.
- Use Kotlin for Android UI/service lifecycle.
- Use Chaquopy to embed Python and reuse a forked Android-specific version of the existing proxy server logic.
- Package FFmpeg as an APK-bundled native executable/library per ABI. Do not download executable files at runtime.
- Keep the 3DS protocol unchanged: Android serves the same endpoints and response formats as the PC server.

## 2. New Files
- `3ds-music-player/android/settings.gradle.kts`
  - Gradle plugin repositories and app module registration.
- `3ds-music-player/android/build.gradle.kts`
  - Android Gradle Plugin and Chaquopy plugin versions.
- `3ds-music-player/android/gradle.properties`
  - AndroidX and Kotlin settings.
- `3ds-music-player/android/app/build.gradle.kts`
  - `com.android.application`, Kotlin, Chaquopy, dependencies, ABI filters, Python pip requirements.
- `3ds-music-player/android/app/src/main/AndroidManifest.xml`
  - Permissions:
    - `android.permission.INTERNET`
    - `android.permission.ACCESS_NETWORK_STATE`
    - `android.permission.FOREGROUND_SERVICE`
    - `android.permission.FOREGROUND_SERVICE_DATA_SYNC`
    - `android.permission.POST_NOTIFICATIONS`
  - Service:
    - `.server.StreaMuServerService`
    - `android:foregroundServiceType="dataSync"`
- `3ds-music-player/android/app/src/main/java/com/streamu/server/MainActivity.kt`
  - Displays server status, IP address, connection URL, logs.
  - Provides Start and Stop actions.
- `3ds-music-player/android/app/src/main/java/com/streamu/server/StreaMuServerService.kt`
  - Owns foreground service lifecycle.
  - Starts Python server only after explicit Start.
  - Stops Python server and notification on Stop.
- `3ds-music-player/android/app/src/main/java/com/streamu/server/NotificationHelper.kt`
  - Creates notification channel and ongoing notification with Stop action.
- `3ds-music-player/android/app/src/main/java/com/streamu/server/ServerState.kt`
  - Shared state exposed as `StateFlow`.
  - Fields: `isRunning`, `ipAddress`, `url`, `logs`.
- `3ds-music-player/android/app/src/main/python/streamu_android_server.py`
  - Android-specific Starlette app.
  - Reuses endpoint behavior from `server/proxy.py`.
  - Accepts FFmpeg path from Kotlin.
- `3ds-music-player/android/app/src/main/python/android_bridge.py`
  - Small callback bridge for logs and lifecycle events.
- `3ds-music-player/android/app/src/main/python/requirements.txt`
  - `starlette`
  - `yt-dlp`
  - `uvicorn`
  - `certifi`
- `3ds-music-player/android/app/src/main/jniLibs/README.md`
  - Documents where ABI-specific FFmpeg binaries/libraries must be placed.
- `3ds-music-player/android/README.md`
  - Build and sideload instructions.

## 3. Existing Files
- `3ds-music-player/README.md`
  - Add Android APK section in English and Japanese.
  - Explain that Android server starts only by user action, shows a notification while running, and does not auto-start after reboot.
- `3ds-music-player/server/proxy.py`
  - No functional change in this task.
  - Keep PC server stable while Android gets its own wrapper module.
- `HANDOFF/phases.md`
  - Move `Android APK` from Planned to In Progress/complete according to workflow outcome.

## 4. Kotlin Interfaces
```kotlin
data class ServerUiState(
    val isRunning: Boolean = false,
    val ipAddress: String = "",
    val connectionUrl: String = "",
    val logs: List<String> = emptyList()
)

object ServerState {
    val uiState: StateFlow<ServerUiState>
    fun setRunning(ipAddress: String)
    fun setStopped()
    fun addLog(message: String)
}

class StreaMuServerService : Service() {
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int
    override fun onDestroy()
    private fun startServer()
    private fun stopServer()
}

object NotificationHelper {
    fun ensureChannel(context: Context)
    fun buildRunningNotification(context: Context): Notification
}
```

## 5. Python Interfaces
```python
def start_server(host: str, port: int, ffmpeg_path: str, log_callback: object) -> None:
    """Start uvicorn and block until stop_server is called."""

def stop_server() -> None:
    """Request server shutdown and terminate active child processes."""

def get_local_ip() -> str:
    """Return the Android device LAN IP shown to the 3DS user."""
```

## 6. Lifecycle Design
- `MainActivity` never starts the server automatically.
- Start button sends `ACTION_START` to `StreaMuServerService`.
- Service calls `startForeground()` before starting Python work.
- Service starts Python on a single background executor thread.
- Python starts uvicorn on `0.0.0.0:8080`.
- Stop button or notification Stop action sends `ACTION_STOP`.
- Stop path:
  1. Call Python `stop_server()`.
  2. Kill active yt-dlp/FFmpeg subprocesses tracked by the Python module.
  3. Clear foreground notification.
  4. Stop service.
  5. Update `ServerState` to stopped.
- `onDestroy()` repeats the stop path defensively.
- `onTaskRemoved()` stops the server unless the user explicitly leaves it running through the notification. MVP behavior: stop on task removal to minimize battery.

## 7. Low Memory / Battery Design
- No boot receiver.
- No periodic worker.
- No background search or prefetch.
- No thumbnail cache warmer.
- No playlist polling.
- Search, extraction, and transcoding occur only inside active HTTP requests.
- Logs are capped at 50 lines.
- Active subprocess references are tracked and killed on client disconnect or service stop.
- Uvicorn access logs stay disabled.
- The server is stopped by default whenever the service is destroyed.

## 8. FFmpeg Packaging Design
- Android must not download FFmpeg into app-writable storage and execute it.
- The app expects FFmpeg to be packaged with the APK as ABI-specific native content.
- Initial ABI target:
  - `arm64-v8a`
- Optional later ABI:
  - `armeabi-v7a`
- `StreaMuServerService` resolves FFmpeg path from `applicationInfo.nativeLibraryDir`.
- If FFmpeg is missing, Start fails with a visible log message:
  - `FFmpeg native binary is missing for this device ABI.`
- The README must document LGPL/GPL implications and the source/build provenance for the bundled FFmpeg artifact before public release.

## 9. Endpoint Mapping
- `/`
  - Android-friendly dashboard HTML, matching PC dashboard information.
- `/api/logs`
  - Returns `{"logs": [...]}`.
- `/search?q=...&lang=...`
  - Same validation as PC server.
  - Same tab-separated response fields.
- `/thumbnail?id=...`
  - Same YouTube thumbnail proxy behavior and size guard.
- `/stream?i=...&t=...`
  - Same video ID validation.
  - Same MP3 streaming output: `audio/mpeg`.
  - Uses bundled FFmpeg path.

## 10. Implementation Order
1. Add Android Gradle project skeleton.
2. Add Kotlin UI with stopped/running state and no server logic.
3. Add foreground service, notification channel, and Stop action.
4. Add Chaquopy wiring and Python requirements.
5. Port `proxy.py` to `streamu_android_server.py` with explicit `start_server` / `stop_server`.
6. Add FFmpeg path resolution and missing-FFmpeg error path.
7. Wire Start/Stop to Python server.
8. Add README Android instructions.
9. Build APK.
10. Smoke-test app lifecycle:
    - launch shows stopped
    - Start shows notification
    - dashboard reachable from browser
    - Stop removes notification and closes port

## 11. Build Checkpoints
- Checkpoint A: `./gradlew :app:assembleDebug` builds with Kotlin UI only.
- Checkpoint B: `./gradlew :app:assembleDebug` builds after foreground service and notification.
- Checkpoint C: `./gradlew :app:assembleDebug` builds after Chaquopy and Python requirements.
- Checkpoint D: APK starts server on Android device/emulator.
- Checkpoint E: `/api/logs` and `/` respond over LAN/local browser.
- Checkpoint F: `/search` returns tab-separated results.
- Checkpoint G: `/stream` returns `audio/mpeg` when FFmpeg is packaged.

## 12. Risks
- Chaquopy package compatibility may fail for a future `yt-dlp` dependency.
- FFmpeg packaging and licensing must be solved before public release.
- Android power management may stop long-running work on aggressive vendor ROMs despite foreground service.
- YouTube extraction can break independently of the APK.
- Emulator testing cannot fully prove 3DS LAN behavior.

## 13. Out of Scope
- 3DS client changes.
- Proxyless playback.
- Play Store policy work.
- Boot-time auto-start.
- External-network access.
