#ifndef FLUTTER_PLUGIN_CAMERA_RECORDER_PLUGIN_H_
#define FLUTTER_PLUGIN_CAMERA_RECORDER_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>

#include <memory>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>

// Forward declarations to avoid including MF headers in header file
struct IMFMediaSource;
struct IMFSourceReader;
struct IMFSinkWriter;
struct IMFCaptureEngine;

// Forward declarations for new handlers
namespace camera_windows {
class PreviewHandler;
class TextureHandler;
class CaptureEngineListener;
}

namespace camera_recorder {

class CameraRecorderPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows *registrar);

  CameraRecorderPlugin();

  virtual ~CameraRecorderPlugin();

  // Disallow copy and assign.
  CameraRecorderPlugin(const CameraRecorderPlugin&) = delete;
  CameraRecorderPlugin& operator=(const CameraRecorderPlugin&) = delete;

  // Called when a method is called on this plugin's channel from Dart.
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

 private:
  // 允许 Pigeon HostApi 实现访问私有成员
  friend class CameraHostApiImpl;
  
  // Flutter texture (for preview) - 使用新的 TextureHandler
  flutter::TextureRegistrar* texture_registrar_ = nullptr;
  std::unique_ptr<camera_windows::TextureHandler> texture_handler_;
  
  // 新的处理器
  std::unique_ptr<camera_windows::PreviewHandler> preview_handler_;
  std::unique_ptr<camera_windows::CaptureEngineListener> capture_listener_;
  
  // 旧的纹理系统（保留作为备用）
  int64_t texture_id_ = -1;
  std::unique_ptr<flutter::TextureVariant> texture_;
  std::vector<uint8_t> pixel_buffer_;
  std::mutex pixel_mutex_;
  UINT32 frame_width_ = 0;
  UINT32 frame_height_ = 0;
  
  // 设备状态
  std::string current_device_id_;
  bool is_open_ = false;
  bool is_recording_ = false;
  std::string current_output_path_;

  // Media Foundation objects
  IMFMediaSource* media_source_ = nullptr;
  IMFSourceReader* source_reader_ = nullptr;
  IMFSinkWriter* sink_writer_ = nullptr;
  IMFCaptureEngine* capture_engine_ = nullptr;
  unsigned int sink_writer_stream_index_ = 0;
  unsigned int video_stream_index_ = 0;
  bool is_nv12_format_ = false;

  // Capture thread
  std::atomic<bool> stop_flag_{false};
  std::thread capture_thread_;
  std::atomic<uint64_t> frame_count_{0};
  std::atomic<uint64_t> fps_window_start_ms_{0};
  std::atomic<double> fps_value_{0.0};

  // Helpers
  bool OpenDeviceById(const std::string& symbolic_link_utf8);
  bool ConfigureSourceReaderRGB32(UINT32& width, UINT32& height, UINT32& fpsNum, UINT32& fpsDen);
  bool CreateSinkWriterMp4(const std::wstring& filePathW, UINT32 width, UINT32 height, UINT32 fpsNum, UINT32 fpsDen);
  void CaptureLoop();
  void CloseInternal();
  static uint64_t NowMs();
};

}  // namespace camera_recorder

#endif  // FLUTTER_PLUGIN_CAMERA_RECORDER_PLUGIN_H_
