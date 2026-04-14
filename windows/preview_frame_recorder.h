// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PACKAGES_CAMERA_CAMERA_WINDOWS_WINDOWS_PREVIEW_FRAME_RECORDER_H_
#define PACKAGES_CAMERA_CAMERA_WINDOWS_WINDOWS_PREVIEW_FRAME_RECORDER_H_

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace camera_windows {
using Microsoft::WRL::ComPtr;

// 使用预览回调帧进行录像（与 TextureHandler 同源），支持水平镜像后编码写入 MP4。
// 不依赖 IMFCaptureRecordSink 的视频流，便于在编码前做软件翻转，使录像与预览镜像一致。
class PreviewFrameRecorder {
 public:
  PreviewFrameRecorder() = default;
  ~PreviewFrameRecorder();

  PreviewFrameRecorder(const PreviewFrameRecorder&) = delete;
  PreviewFrameRecorder& operator=(const PreviewFrameRecorder&) = delete;

  // 开始录像。帧格式为 RGB32，与预览一致。
  // fps_num/fps_den: 帧率，用于 SinkWriter 时间戳与编码。
  bool Start(const std::string& file_path, uint32_t width, uint32_t height,
             uint32_t fps_num, uint32_t fps_den, bool mirror);

  // 送入一帧（RGB32，width*height*4 字节）。timestamp_us 为微秒。
  void OnFrame(const uint8_t* rgb32_data, uint32_t data_length,
               uint64_t timestamp_us);

  // 停止录像并关闭文件。
  void Stop();

  bool IsRecording() const { return recording_; }
  std::string GetPath() const { return file_path_; }
  uint64_t GetDurationUs() const { return duration_us_; }
  // 启动失败时的详细原因（如 "MFCreateFile 0x80070002"），便于排查。
  std::string GetLastError() const { return last_error_; }

 private:
  void SetLastError(const char* step, HRESULT hr);
  bool CreateSinkWriter(uint32_t width, uint32_t height, uint32_t fps_num,
                        uint32_t fps_den);
  void Rgb32ToNv12(const uint8_t* rgb32, uint32_t width, uint32_t height,
                   uint8_t* nv12_out, bool mirror);

  std::string file_path_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t fps_num_ = 30;
  uint32_t fps_den_ = 1;
  bool mirror_ = false;
  bool recording_ = false;

  int64_t first_timestamp_us_ = -1;
  uint64_t duration_us_ = 0;

  ComPtr<IMFSinkWriter> sink_writer_;
  ComPtr<IMFMediaSink> media_sink_;
  ComPtr<IMFByteStream> byte_stream_;
  DWORD stream_index_ = 0;
  std::vector<uint8_t> nv12_buffer_;
  std::vector<uint8_t> rgb32_frame_buffer_;  // RGB32 输入模式时存一帧（含可选镜像）
  std::mutex write_mutex_;
  std::string last_error_;
  // 为 true 时 OnFrame 直接送 RGB32（与预览同源），否则送 NV12
  bool input_is_rgb32_ = false;
};

}  // namespace camera_windows

#endif  // PACKAGES_CAMERA_CAMERA_WINDOWS_WINDOWS_PREVIEW_FRAME_RECORDER_H_
