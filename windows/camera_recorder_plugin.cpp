#include "camera_recorder_plugin.h"

// This must be included before many other Windows headers.
#include <windows.h>

// For getPlatformVersion; remove unless needed for your plugin implementation.
#include <VersionHelpers.h>

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <flutter/texture_registrar.h>
// Pigeon bridge
#include "pigeon/messages.g.h"

#include <memory>
#include <sstream>
#include <string>
#include <vector>

// Media Foundation
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfcaptureengine.h>

// New handlers
#include "preview_handler.h"
#include "texture_handler.h"
#include "capture_engine_listener.h"
#include "string_utils.h"

namespace camera_recorder {

// File-scope Pigeon HostApi implementation
class CameraHostApiImpl : public camera_recorder_pigeon::CameraHostApi {
 public:
  explicit CameraHostApiImpl(CameraRecorderPlugin* p) : plugin_(p) {}
  camera_recorder_pigeon::ErrorOr<flutter::EncodableList> EnumerateCameras() override;
  camera_recorder_pigeon::ErrorOr<bool> OpenCamera(const std::string& device_id) override;
  camera_recorder_pigeon::ErrorOr<bool> StartPreview() override { return true; }
  camera_recorder_pigeon::ErrorOr<bool> StopPreview() override { return true; }
  camera_recorder_pigeon::ErrorOr<bool> StartVideoRecording(int64_t camera_id, const std::string& file_path) override { plugin_->current_output_path_ = file_path; plugin_->is_recording_ = true; return true; }
  camera_recorder_pigeon::ErrorOr<std::optional<std::string>> StopVideoRecording(int64_t camera_id) override { plugin_->is_recording_ = false; return std::optional<std::string>(plugin_->current_output_path_); }
  std::optional<camera_recorder_pigeon::FlutterError> CloseCamera() override { plugin_->CloseInternal(); return std::nullopt; }
  camera_recorder_pigeon::ErrorOr<int64_t> GetPreviewTextureId() override { 
    if (plugin_->texture_handler_) {
      return plugin_->texture_handler_->RegisterTexture();
    }
    return -1;
  }
  camera_recorder_pigeon::ErrorOr<camera_recorder_pigeon::PreviewSize> GetPreviewSize() override { 
    camera_recorder_pigeon::PreviewSize s(plugin_->frame_width_, plugin_->frame_height_); 
    s.set_rotation((int64_t)0); 
    return s; 
  }
 private:
  CameraRecorderPlugin* plugin_;
};

// static
void CameraRecorderPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows *registrar) {
  auto channel =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          registrar->messenger(), "camera_recorder",
          &flutter::StandardMethodCodec::GetInstance());

  auto plugin = std::make_unique<CameraRecorderPlugin>();
  plugin->texture_registrar_ = registrar->texture_registrar();
  
  // 初始化 texture_handler 与 texture_registrar
  plugin->texture_handler_ = std::make_unique<camera_windows::TextureHandler>(registrar->texture_registrar());

  channel->SetMethodCallHandler(
      [plugin_pointer = plugin.get()](const auto &call, auto result) {
        plugin_pointer->HandleMethodCall(call, std::move(result));
      });

  CameraRecorderPlugin* raw_plugin_ptr = plugin.get();
  camera_recorder_pigeon::CameraHostApi::SetUp(registrar->messenger(), new CameraHostApiImpl(raw_plugin_ptr));

  registrar->AddPlugin(std::move(plugin));
}

camera_recorder_pigeon::ErrorOr<flutter::EncodableList> CameraHostApiImpl::EnumerateCameras() {
  flutter::EncodableList list;
  HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
  if (FAILED(hr)) {
    return camera_recorder_pigeon::FlutterError("mf_startup_failed: " + std::to_string(hr));
  }
  IMFAttributes* attributes = nullptr;
  hr = MFCreateAttributes(&attributes, 1);
  if (FAILED(hr)) { MFShutdown(); return camera_recorder_pigeon::FlutterError("attr_failed"); }
  hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  if (FAILED(hr)) { attributes->Release(); MFShutdown(); return camera_recorder_pigeon::FlutterError("set_guid_failed"); }
  IMFActivate** devices = nullptr; UINT32 count = 0;
  hr = MFEnumDeviceSources(attributes, &devices, &count);
  attributes->Release();
  if (FAILED(hr)) { 
    MFShutdown(); 
    return camera_recorder_pigeon::FlutterError("enum_failed: " + std::to_string(hr) + ", count: " + std::to_string(count)); 
  }
  for (UINT32 i = 0; i < count; ++i) {
    WCHAR* friendlyName = nullptr; UINT32 nameLen = 0;
    devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendlyName, &nameLen);
    WCHAR* symbolicLink = nullptr; UINT32 linkLen = 0;
    devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &symbolicLink, &linkLen);
    int neededName = WideCharToMultiByte(CP_UTF8, 0, friendlyName ? friendlyName : L"", -1, nullptr, 0, nullptr, nullptr);
    std::string nameUtf8; if (neededName > 0) { nameUtf8.resize(neededName - 1);
      WideCharToMultiByte(CP_UTF8, 0, friendlyName ? friendlyName : L"", -1, nameUtf8.data(), neededName, nullptr, nullptr);
    }
    int neededId = WideCharToMultiByte(CP_UTF8, 0, symbolicLink ? symbolicLink : L"", -1, nullptr, 0, nullptr, nullptr);
    std::string idUtf8; if (neededId > 0) { idUtf8.resize(neededId - 1);
      WideCharToMultiByte(CP_UTF8, 0, symbolicLink, -1, idUtf8.data(), neededId, nullptr, nullptr);
    }
    if (friendlyName) CoTaskMemFree(friendlyName);
    if (symbolicLink) CoTaskMemFree(symbolicLink);
    
    // 直接构建 EncodableList，按照 Pigeon 期望的格式
    flutter::EncodableList camera_item;
    camera_item.push_back(flutter::EncodableValue(idUtf8));
    camera_item.push_back(flutter::EncodableValue(nameUtf8.empty() ? "" : nameUtf8));
    list.push_back(flutter::EncodableValue(camera_item));
    devices[i]->Release();
  }
  CoTaskMemFree(devices);
  MFShutdown();
  return list;
}

camera_recorder_pigeon::ErrorOr<bool> CameraHostApiImpl::OpenCamera(const std::string& device_id) {
  if (device_id.empty()) return false;
  plugin_->CloseInternal();
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool needCoUninit = SUCCEEDED(hr);
  if (hr == RPC_E_CHANGED_MODE) needCoUninit = false;
  hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
  if (FAILED(hr)) { if (needCoUninit) CoUninitialize(); return false; }
  bool ok = plugin_->OpenDeviceById(device_id);
  if (!ok) { MFShutdown(); if (needCoUninit) CoUninitialize(); return false; }
  UINT32 w=0,h=0,fpsN=0,fpsD=1;
  ok = plugin_->ConfigureSourceReaderRGB32(w,h,fpsN,fpsD);
  if (!ok) { plugin_->CloseInternal(); MFShutdown(); if (needCoUninit) CoUninitialize(); return false; }
  bool got=false; for (int i=0;i<20 && !got;++i){ DWORD si=0, fl=0; LONGLONG ts=0; IMFSample* smp=nullptr;
    hr = plugin_->source_reader_ ? plugin_->source_reader_->ReadSample((DWORD)plugin_->video_stream_index_,0,&si,&fl,&ts,&smp):E_POINTER;
    if (SUCCEEDED(hr) && smp){ got=true; smp->Release(); break; } Sleep(50);
  }
  if (!got) { plugin_->CloseInternal(); MFShutdown(); if (needCoUninit) CoUninitialize(); return false; }
  plugin_->current_device_id_ = device_id; 
  plugin_->is_open_ = true; 
  plugin_->frame_width_ = w; 
  plugin_->frame_height_ = h;
  
  // 设置 texture_handler 的尺寸
  if (plugin_->texture_handler_) {
    plugin_->texture_handler_->UpdateTextureSize(w, h);
  }
  plugin_->stop_flag_.store(false);
  plugin_->fps_window_start_ms_.store(CameraRecorderPlugin::NowMs());
  plugin_->frame_count_.store(0);
  plugin_->capture_thread_ = std::thread(&CameraRecorderPlugin::CaptureLoop, plugin_);
  return true;
}

CameraRecorderPlugin::CameraRecorderPlugin() {
  // 初始化处理器
  texture_handler_ = std::make_unique<camera_windows::TextureHandler>(nullptr);
  preview_handler_ = std::make_unique<camera_windows::PreviewHandler>();
  // capture_listener_ 需要 observer，暂时设为 nullptr，后续在需要时再初始化
  capture_listener_ = nullptr;
}

CameraRecorderPlugin::~CameraRecorderPlugin() {
  CloseInternal();
}

void CameraRecorderPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue> &method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (method_call.method_name().compare("getPlatformVersion") == 0) {
    std::ostringstream version_stream;
    version_stream << "Windows ";
    if (IsWindows10OrGreater()) {
      version_stream << "10+";
    } else if (IsWindows8OrGreater()) {
      version_stream << "8";
    } else if (IsWindows7OrGreater()) {
      version_stream << "7";
    }
    result->Success(flutter::EncodableValue(version_stream.str()));
  } else if (method_call.method_name().compare("enumerateCameras") == 0) {
    // Initialize Media Foundation
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
      result->Error("mf_startup_failed", "MFStartup failed", flutter::EncodableValue(static_cast<int>(hr)));
      return;
    }

    IMFAttributes* attributes = nullptr;
    hr = MFCreateAttributes(&attributes, 1);
    if (FAILED(hr)) {
      MFShutdown();
      result->Error("mf_attr_failed", "MFCreateAttributes failed", flutter::EncodableValue(static_cast<int>(hr)));
      return;
    }

    hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) {
      attributes->Release();
      MFShutdown();
      result->Error("mf_set_guid_failed", "SetGUID failed", flutter::EncodableValue(static_cast<int>(hr)));
      return;
    }

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(attributes, &devices, &count);
    attributes->Release();
    if (FAILED(hr)) {
      MFShutdown();
      result->Error("mf_enum_failed", "MFEnumDeviceSources failed", flutter::EncodableValue(static_cast<int>(hr)));
      return;
    }

    flutter::EncodableList list;
    for (UINT32 i = 0; i < count; ++i) {
      WCHAR* friendlyName = nullptr;
      UINT32 nameLen = 0;
      devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &friendlyName, &nameLen);

      WCHAR* symbolicLink = nullptr;
      UINT32 linkLen = 0;
      devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &symbolicLink, &linkLen);

      // Convert to UTF-8
      int neededName = WideCharToMultiByte(CP_UTF8, 0, friendlyName ? friendlyName : L"", -1, nullptr, 0, nullptr, nullptr);
      std::string nameUtf8;
      if (neededName > 0) {
        nameUtf8.resize(static_cast<size_t>(neededName - 1));
        WideCharToMultiByte(CP_UTF8, 0, friendlyName ? friendlyName : L"", -1, nameUtf8.data(), neededName, nullptr, nullptr);
      }

      int neededId = WideCharToMultiByte(CP_UTF8, 0, symbolicLink ? symbolicLink : L"", -1, nullptr, 0, nullptr, nullptr);
      std::string idUtf8;
      if (neededId > 0) {
        idUtf8.resize(static_cast<size_t>(neededId - 1));
        WideCharToMultiByte(CP_UTF8, 0, symbolicLink ? symbolicLink : L"", -1, idUtf8.data(), neededId, nullptr, nullptr);
      }

      if (friendlyName) CoTaskMemFree(friendlyName);
      if (symbolicLink) CoTaskMemFree(symbolicLink);

      flutter::EncodableMap map;
      map[flutter::EncodableValue("id")] = flutter::EncodableValue(idUtf8);
      map[flutter::EncodableValue("name")] = flutter::EncodableValue(nameUtf8);
      list.push_back(flutter::EncodableValue(map));

      devices[i]->Release();
    }
    CoTaskMemFree(devices);
    MFShutdown();

    result->Success(list);
  } else if (method_call.method_name().compare("openCamera") == 0) {
    const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());
    std::string deviceId;
    if (args) {
      auto it = args->find(flutter::EncodableValue("deviceId"));
      if (it != args->end() && std::holds_alternative<std::string>(it->second)) {
        deviceId = std::get<std::string>(it->second);
      }
    }
    if (deviceId.empty()) {
      result->Error("bad_args", "deviceId is required");
      return;
    }
    CloseInternal();

    // Initialize COM & MF
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool needCoUninit = SUCCEEDED(hr);
    if (hr == RPC_E_CHANGED_MODE) {
      // Already initialized in different mode; continue without uninit.
      needCoUninit = false;
    }
    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
      if (needCoUninit) CoUninitialize();
      result->Error("mf_startup_failed", "MFStartup failed", flutter::EncodableValue(static_cast<int>(hr)));
      return;
    }

    bool ok = OpenDeviceById(deviceId);
    if (!ok) {
      MFShutdown();
      if (needCoUninit) CoUninitialize();
      result->Success(flutter::EncodableValue(false));
      return;
    }

    UINT32 width = 0, height = 0, fpsNum = 0, fpsDen = 1;
    ok = ConfigureSourceReaderRGB32(width, height, fpsNum, fpsDen);
    if (!ok) {
      CloseInternal();
      MFShutdown();
      if (needCoUninit) CoUninitialize();
      result->Success(flutter::EncodableValue(false));
      return;
    }

    // Try read first sample to ensure device is actually delivering frames
    bool got_first_frame = false;
    for (int attempt = 0; attempt < 20 && !got_first_frame; ++attempt) {
      DWORD sIndex = 0; DWORD flags = 0; LONGLONG ts = 0; IMFSample* sample = nullptr;
      hr = source_reader_->ReadSample((DWORD)video_stream_index_, 0, &sIndex, &flags, &ts, &sample);
      if (SUCCEEDED(hr) && sample != nullptr) {
        got_first_frame = true;
        sample->Release();
        break;
      }
      Sleep(50);
    }
    if (!got_first_frame) {
      CloseInternal();
      MFShutdown();
      if (needCoUninit) CoUninitialize();
      result->Success(flutter::EncodableValue(false));
      return;
    }

    current_device_id_ = deviceId;
    is_open_ = true;
    frame_width_ = width; frame_height_ = height;
    // prepare pixel buffer and register texture if needed
    if (texture_registrar_ && texture_id_ == -1) {
      pixel_buffer_.assign(static_cast<size_t>(width*height*4), 0);
      texture_ = std::make_unique<flutter::TextureVariant>(
        flutter::PixelBufferTexture([
          this
        ](size_t, size_t) -> const FlutterDesktopPixelBuffer* {
          static FlutterDesktopPixelBuffer buffer;
          std::lock_guard<std::mutex> lock(pixel_mutex_);
          buffer.buffer = pixel_buffer_.empty() ? nullptr : pixel_buffer_.data();
          buffer.width = frame_width_;
          buffer.height = frame_height_;
          return &buffer;
        })
      );
      texture_id_ = texture_registrar_->RegisterTexture(texture_.get());
    }
    // We intentionally keep MF started; will shutdown in CloseInternal
    // Start capture thread for FPS + preview
    stop_flag_.store(false);
    fps_window_start_ms_.store(NowMs());
    frame_count_.store(0);
    capture_thread_ = std::thread(&CameraRecorderPlugin::CaptureLoop, this);
    result->Success(flutter::EncodableValue(true));
  } else if (method_call.method_name().compare("startRecording") == 0) {
    const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());
    std::string path;
    if (args) {
      auto it = args->find(flutter::EncodableValue("filePath"));
      if (it != args->end() && std::holds_alternative<std::string>(it->second)) {
        path = std::get<std::string>(it->second);
      }
    }
    if (!is_open_) {
      result->Error("not_open", "camera not open");
      return;
    }
    if (path.empty()) {
      result->Error("bad_args", "filePath is required");
      return;
    }
    current_output_path_ = path;
    // 占位：后续实现 SinkWriter，当前先返回 false 代表未实现
    is_recording_ = false;
    result->Success(flutter::EncodableValue(false));
  } else if (method_call.method_name().compare("stopRecording") == 0) {
    if (!is_open_) {
      result->Error("not_open", "camera not open");
      return;
    }
    // 占位：停止并返回路径
    is_recording_ = false;
    result->Success(flutter::EncodableValue(current_output_path_));
  } else if (method_call.method_name().compare("closeCamera") == 0) {
    CloseInternal();
    result->Success();
  } else if (method_call.method_name().compare("getFps") == 0) {
    result->Success(flutter::EncodableValue(fps_value_.load()));
  } else if (method_call.method_name().compare("getPreviewTextureId") == 0) {
    result->Success(flutter::EncodableValue(static_cast<int64_t>(texture_id_)));
  } else if (method_call.method_name().compare("getPreviewSize") == 0) {
    flutter::EncodableMap m;
    m[flutter::EncodableValue("width")] = flutter::EncodableValue(static_cast<int>(frame_width_));
    m[flutter::EncodableValue("height")] = flutter::EncodableValue(static_cast<int>(frame_height_));
    m[flutter::EncodableValue("rotation")] = flutter::EncodableValue(0);
    result->Success(flutter::EncodableValue(m));
  } else {
    result->NotImplemented();
  }
}

// Helpers implementation
uint64_t CameraRecorderPlugin::NowMs() {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  ULARGE_INTEGER uli;
  uli.LowPart = ft.dwLowDateTime;
  uli.HighPart = ft.dwHighDateTime;
  // FILETIME is 100-ns since Jan 1, 1601. Convert to ms since 1601.
  return static_cast<uint64_t>(uli.QuadPart / 10000ULL);
}
bool CameraRecorderPlugin::OpenDeviceById(const std::string& symbolic_link_utf8) {
  IMFAttributes* attributes = nullptr;
  HRESULT hr = MFCreateAttributes(&attributes, 2);
  if (FAILED(hr)) return false;
  hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  if (FAILED(hr)) {
    attributes->Release();
    return false;
  }

  IMFActivate** devices = nullptr;
  UINT32 count = 0;
  hr = MFEnumDeviceSources(attributes, &devices, &count);
  attributes->Release();
  if (FAILED(hr)) return false;

  bool found = false;
  for (UINT32 i = 0; i < count && !found; ++i) {
    WCHAR* symbolicLink = nullptr;
    UINT32 linkLen = 0;
    if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &symbolicLink, &linkLen))) {
      // Convert symbolicLink to UTF-8
      int needed = WideCharToMultiByte(CP_UTF8, 0, symbolicLink, -1, nullptr, 0, nullptr, nullptr);
      std::string idUtf8;
      if (needed > 0) {
        idUtf8.resize(static_cast<size_t>(needed - 1));
        WideCharToMultiByte(CP_UTF8, 0, symbolicLink, -1, idUtf8.data(), needed, nullptr, nullptr);
      }
      CoTaskMemFree(symbolicLink);
      if (idUtf8 == symbolic_link_utf8) {
        // Activate media source
        hr = devices[i]->ActivateObject(__uuidof(IMFMediaSource), reinterpret_cast<void**>(&media_source_));
        if (SUCCEEDED(hr)) {
          found = true;
        }
      }
    }
    devices[i]->Release();
  }
  CoTaskMemFree(devices);
  if (!found) return false;

  // Create SourceReader from media source
  {
    IMFAttributes* readerAttr = nullptr;
    if (SUCCEEDED(MFCreateAttributes(&readerAttr, 1))) {
      readerAttr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
      hr = MFCreateSourceReaderFromMediaSource(media_source_, readerAttr, &source_reader_);
      readerAttr->Release();
    } else {
      hr = MFCreateSourceReaderFromMediaSource(media_source_, nullptr, &source_reader_);
    }
  }
  if (FAILED(hr)) return false;
  return true;
}

bool CameraRecorderPlugin::ConfigureSourceReaderRGB32(UINT32& width, UINT32& height, UINT32& fpsNum, UINT32& fpsDen) {
  if (!source_reader_) return false;

  // Request NV12
  IMFMediaType* type = nullptr;
  HRESULT hr = MFCreateMediaType(&type);
  if (FAILED(hr)) return false;
  hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  if (FAILED(hr)) { type->Release(); return false; }
  hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
  if (FAILED(hr)) { type->Release(); return false; }
  hr = source_reader_->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type);
  type->Release();
  if (FAILED(hr)) {
    // fallback to NV12
    IMFMediaType* type2 = nullptr;
    if (SUCCEEDED(MFCreateMediaType(&type2))) {
      type2->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
      type2->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
      if (SUCCEEDED(source_reader_->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type2))) {
        is_nv12_format_ = true;
      }
      type2->Release();
    }
    if (!is_nv12_format_) return false;
  } else {
    is_nv12_format_ = false;
  }

  // Query the selected media type for width/height/fps
  IMFMediaType* current = nullptr;
  hr = source_reader_->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current);
  if (FAILED(hr)) return false;
  UINT32 w = 0, h = 0;
  MFGetAttributeSize(current, MF_MT_FRAME_SIZE, &w, &h);
  UINT32 num = 0, den = 1;
  MFGetAttributeRatio(current, MF_MT_FRAME_RATE, &num, &den);
  current->Release();
  width = w; height = h; fpsNum = num; fpsDen = den;
  video_stream_index_ = (unsigned int)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
  return true;
}

void CameraRecorderPlugin::CloseInternal() {
  stop_flag_.store(true);
  if (capture_thread_.joinable()) {
    capture_thread_.join();
  }
  
  // 清理新的处理器
  if (preview_handler_) {
    preview_handler_.reset();
  }
  if (capture_listener_) {
    capture_listener_.reset();
  }
  if (texture_handler_) {
    texture_handler_.reset();
  }
  
  // 清理旧的纹理
  if (texture_registrar_ && texture_id_ != -1) {
    texture_registrar_->UnregisterTexture(texture_id_);
    texture_id_ = -1;
  }
  texture_.reset();
  
  if (sink_writer_) { sink_writer_->Release(); sink_writer_ = nullptr; }
  if (source_reader_) { source_reader_->Release(); source_reader_ = nullptr; }
  if (media_source_) { media_source_->Release(); media_source_ = nullptr; }
  if (capture_engine_) { capture_engine_->Release(); capture_engine_ = nullptr; }
  
  is_recording_ = false;
  is_open_ = false;
  current_device_id_.clear();
  current_output_path_.clear();
  MFShutdown();
  CoUninitialize();
}

void CameraRecorderPlugin::CaptureLoop() {
  // Simple loop to read samples and update FPS
  while (!stop_flag_.load()) {
    DWORD sIndex = 0; DWORD flags = 0; LONGLONG ts = 0; IMFSample* sample = nullptr;
    HRESULT hr = source_reader_ ? source_reader_->ReadSample((DWORD)video_stream_index_, 0, &sIndex, &flags, &ts, &sample) : E_POINTER;
    if (SUCCEEDED(hr) && sample != nullptr) {
      IMFMediaBuffer* buffer = nullptr;
      if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && buffer) {
        BYTE* data = nullptr; DWORD maxLen = 0; DWORD curLen = 0;
        if (SUCCEEDED(buffer->Lock(&data, &maxLen, &curLen)) && data) {
          if (!is_nv12_format_) {
            // Prefer 2D lock to respect stride/pitch
            IMF2DBuffer* buf2d = nullptr;
            if (SUCCEEDED(buffer->QueryInterface(__uuidof(IMF2DBuffer), reinterpret_cast<void**>(&buf2d))) && buf2d) {
              BYTE* scan0 = nullptr; LONG pitch = 0;
              if (SUCCEEDED(buf2d->Lock2D(&scan0, &pitch)) && scan0 && pitch != 0) {
                std::lock_guard<std::mutex> lock(pixel_mutex_);
                if (pixel_buffer_.size() >= static_cast<size_t>(frame_width_ * frame_height_ * 4)) {
                  const UINT32 w = frame_width_;
                  const UINT32 h = frame_height_;
                  for (UINT32 y = 0; y < h; ++y) {
                    const uint8_t* srcRow = pitch > 0 ? (scan0 + y * pitch)
                                                      : (scan0 + (h - 1 - y) * (-pitch));
                    uint8_t* dstRow = pixel_buffer_.data() + static_cast<size_t>(y) * w * 4;
                    for (UINT32 x = 0; x < w; ++x) {
                      const uint8_t b = srcRow[x * 4 + 0];
                      const uint8_t g = srcRow[x * 4 + 1];
                      const uint8_t r = srcRow[x * 4 + 2];
                      dstRow[x * 4 + 0] = b;
                      dstRow[x * 4 + 1] = g;
                      dstRow[x * 4 + 2] = r;
                      dstRow[x * 4 + 3] = 255;
                    }
                  }
                }
                buf2d->Unlock2D();
              } else {
                // Fallback to flat buffer copy
                std::lock_guard<std::mutex> lock(pixel_mutex_);
                if (curLen >= frame_width_ * frame_height_ * 4 && pixel_buffer_.size() >= static_cast<size_t>(frame_width_ * frame_height_ * 4)) {
                  const uint8_t* src = data;
                  uint8_t* dst = pixel_buffer_.data();
                  const size_t total = static_cast<size_t>(frame_width_ * frame_height_);
                  for (size_t i = 0; i < total; ++i) {
                    dst[i * 4 + 0] = src[i * 4 + 0];
                    dst[i * 4 + 1] = src[i * 4 + 1];
                    dst[i * 4 + 2] = src[i * 4 + 2];
                    dst[i * 4 + 3] = 255;
                  }
                }
              }
              buf2d->Release();
            } else {
              // No 2D buffer; flat copy
              std::lock_guard<std::mutex> lock(pixel_mutex_);
              if (curLen >= frame_width_ * frame_height_ * 4 && pixel_buffer_.size() >= static_cast<size_t>(frame_width_ * frame_height_ * 4)) {
                const uint8_t* src = data;
                uint8_t* dst = pixel_buffer_.data();
                const size_t total = static_cast<size_t>(frame_width_ * frame_height_);
                for (size_t i = 0; i < total; ++i) {
                  dst[i * 4 + 0] = src[i * 4 + 0];
                  dst[i * 4 + 1] = src[i * 4 + 1];
                  dst[i * 4 + 2] = src[i * 4 + 2];
                  dst[i * 4 + 3] = 255;
                }
              }
            }
          } else {
            // NV12/NV21 fallback: YUV420 4:2:0 -> BGRA
            std::lock_guard<std::mutex> lock(pixel_mutex_);
            const UINT32 w = frame_width_;
            const UINT32 h = frame_height_;
            const size_t ySize = static_cast<size_t>(w) * h;
            if (curLen >= ySize + (ySize / 2) && pixel_buffer_.size() >= static_cast<size_t>(w) * h * 4) {
              const uint8_t* yPlane = data;
              const uint8_t* uvPlane = data + ySize;
              auto clamp8 = [](int v) -> uint8_t { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); };
              for (UINT32 y = 0; y < h; ++y) {
                uint8_t* dstRow = pixel_buffer_.data() + static_cast<size_t>(y) * w * 4;
                const uint8_t* yRow = yPlane + static_cast<size_t>(y) * w;
                const uint8_t* uvRow = uvPlane + static_cast<size_t>(y / 2) * w; // UV stride ~ w
                for (UINT32 x = 0; x < w; x += 2) {
                  const int Y0 = yRow[x + 0];
                  const int Y1 = yRow[x + 1];
                  // Try NV21 order (VU) first to mitigate蓝灰偏色; many devices output NV21
                  const int V = uvRow[x + 0] - 128; // NV21: VU
                  const int U = uvRow[x + 1] - 128;
                  // BT.601 limited-range: C = Y - 16; scale 1.164
                  const int C0 = Y0 - 16;
                  const int C1 = Y1 - 16;
                  const int Rv = (int)(1.596f * V);
                  const int Guv = (int)(-0.392f * U - 0.813f * V);
                  const int Bu = (int)(2.017f * U);

                  uint8_t r0 = clamp8((int)(1.164f * C0) + Rv);
                  uint8_t g0 = clamp8((int)(1.164f * C0) + Guv);
                  uint8_t b0 = clamp8((int)(1.164f * C0) + Bu);
                  uint8_t r1 = clamp8((int)(1.164f * C1) + Rv);
                  uint8_t g1 = clamp8((int)(1.164f * C1) + Guv);
                  uint8_t b1 = clamp8((int)(1.164f * C1) + Bu);

                  // write two pixels (BGRA)
                  size_t i0 = (size_t)x * 4;
                  dstRow[i0 + 0] = b0; dstRow[i0 + 1] = g0; dstRow[i0 + 2] = r0; dstRow[i0 + 3] = 255;
                  size_t i1 = (size_t)(x + 1) * 4;
                  dstRow[i1 + 0] = b1; dstRow[i1 + 1] = g1; dstRow[i1 + 2] = r1; dstRow[i1 + 3] = 255;
                }
              }
            }
          }
          buffer->Unlock();
          
          // 使用新的 texture_handler 更新纹理
          if (texture_handler_) {
            texture_handler_->UpdateBuffer(data, curLen);
          } else if (texture_registrar_ && texture_id_ != -1) {
            texture_registrar_->MarkTextureFrameAvailable(texture_id_);
          }
        }
        buffer->Release();
      }
      sample->Release();
      frame_count_.fetch_add(1);
      auto start = fps_window_start_ms_.load();
      auto now = NowMs();
      if (now > start + 1000) {
        auto frames = frame_count_.exchange(0);
        fps_window_start_ms_.store(now);
        fps_value_.store(static_cast<double>(frames));
      }
    } else {
      Sleep(5);
    }
  }
}

}  // namespace camera_recorder
