/// Optional desktop-only tuning for [CameraDesktopPlugin.createCameraWithSettings].
///
/// `camera_platform_interface` [MediaSettings] does not carry these fields; the
/// plugin reads the static values below when building the native `create` call.
///
/// **Windows:** [allowUpscaleToOnlyAvailableFormat] controls whether negotiation
/// may select a format **taller** than the [ResolutionPreset] height hint when
/// nothing fits underneath (for example a camera that only exposes 1080p while
/// the preset targets 720p). Other platforms currently always use the same soft
/// negotiation behavior and ignore this flag.
class CameraDesktopCaptureHints {
  CameraDesktopCaptureHints._();

  /// When `true` (default), Windows may pick the smallest available resolution
  /// whose height is **above** the preset ceiling if no in-range mode exists.
  ///
  /// When `false`, Windows keeps the old strict filter (only heights at or
  /// below the preset hint); initialization may fail on sparse devices.
  static bool allowUpscaleToOnlyAvailableFormat = true;

  static final Map<int, String> _nextPhotoPath = {};
  static final Map<int, String> _nextVideoPath = {};

  /// Writes the next [CameraDesktopPlugin.takePicture] for [cameraId] to [path]
  /// instead of a generated temp file. The entry is removed when `takePicture`
  /// runs (or when [clearPendingPathsForCamera] is called).
  ///
  /// The parent directory must exist; encoding is always JPEG (`.jpg` suffix
  /// recommended).
  static void setNextPhotoCapturePath(int cameraId, String path) {
    if (path.isEmpty) {
      throw ArgumentError.value(path, 'path', 'must not be empty');
    }
    _nextPhotoPath[cameraId] = path;
  }

  /// Writes the next recording for [cameraId] to [path] instead of a temp
  /// file. Consumed when [CameraDesktopPlugin.startVideoRecording] runs.
  ///
  /// Use an extension appropriate for the platform (typically `.mp4` on
  /// macOS/Windows; Linux follows the active GStreamer muxer, often `.mp4` or
  /// `.mkv`).
  static void setNextVideoRecordingPath(int cameraId, String path) {
    if (path.isEmpty) {
      throw ArgumentError.value(path, 'path', 'must not be empty');
    }
    _nextVideoPath[cameraId] = path;
  }

  /// Drops any pending output paths for [cameraId] (e.g. after [dispose]).
  static void clearPendingPathsForCamera(int cameraId) {
    _nextPhotoPath.remove(cameraId);
    _nextVideoPath.remove(cameraId);
  }

  /// Used by [CameraDesktopPlugin] only.
  static String? consumeNextPhotoCapturePath(int cameraId) =>
      _nextPhotoPath.remove(cameraId);

  /// Used by [CameraDesktopPlugin] only.
  static String? consumeNextVideoRecordingPath(int cameraId) =>
      _nextVideoPath.remove(cameraId);
}
