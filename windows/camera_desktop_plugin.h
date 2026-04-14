#pragma once

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>

#include <windows.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

#include "camera.h"

class CameraDesktopPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar);

  CameraDesktopPlugin(
      flutter::PluginRegistrarWindows* registrar,
      std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel);

  ~CameraDesktopPlugin() override;

  void EraseCameraAfterDispose(int camera_id);

  // Global instance for FFI access.
  static CameraDesktopPlugin* instance() { return instance_; }

  /// Marshals |task| onto the Flutter view's HWND message loop (platform
  /// thread). Safe to call from any thread. No-op HWND falls back to |task| on
  /// the current thread (best effort).
  static void RunSyncOnUi(HWND hwnd, std::function<void()> task);

 private:
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  // Helpers for individual methods.
  void HandleAvailableCameras(
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void HandleGetPlatformCapabilities(
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void HandleCreate(
      const flutter::EncodableMap& args,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void HandleInitialize(
      const flutter::EncodableMap& args,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void HandleTakePicture(
      const flutter::EncodableMap& args,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void HandleStartVideoRecording(
      const flutter::EncodableMap& args,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void HandleStopVideoRecording(
      const flutter::EncodableMap& args,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void HandleStartImageStream(
      const flutter::EncodableMap& args,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void HandleStopImageStream(
      const flutter::EncodableMap& args,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void HandlePausePreview(
      const flutter::EncodableMap& args,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void HandleResumePreview(
      const flutter::EncodableMap& args,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void HandleSetMirror(
      const flutter::EncodableMap& args,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
  void HandleDispose(
      const flutter::EncodableMap& args,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  std::optional<LRESULT> OnTopLevelWindowProc(HWND hwnd,
                                              UINT message,
                                              WPARAM wparam,
                                              LPARAM lparam);

  /// Resolves root HWND and registers the top-level WindowProc hook once.
  void EnsureRunOnUiHook();

  // Returns the camera for |args["cameraId"]| or responds with an error.
  std::shared_ptr<Camera> FindCamera(
      const flutter::EncodableMap& args,
      flutter::MethodResult<flutter::EncodableValue>* result);

  flutter::PluginRegistrarWindows* registrar_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel_;
  mutable std::mutex cameras_mutex_;
  std::map<int, std::shared_ptr<Camera>> cameras_;
  int next_camera_id_ = 1;
  bool should_co_uninitialize_ = false;
  bool shutting_down_ = false;

  /// Root HWND used with PostMessage for RunSyncOnUi (not necessarily the view child).
  HWND flutter_view_hwnd_ = nullptr;
  int window_proc_delegate_id_ = 0;

  static CameraDesktopPlugin* instance_;
};
