// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "preview_frame_recorder.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <vector>

#include "string_utils.h"
#include "texture_handler.h"

namespace camera_windows {

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kBytesPerPixelRgb32 = 4;

// RGB32 (BGRA) 转 NV12，可选水平翻转。
void Rgb32ToNv12Impl(const uint8_t* rgb32, uint32_t width, uint32_t height,
                     uint8_t* nv12_out, bool mirror) {
  const uint32_t y_size = width * height;
  uint8_t* y_plane = nv12_out;
  uint8_t* uv_plane = nv12_out + y_size;

  const auto* src = reinterpret_cast<const MFVideoFormatRGB32Pixel*>(rgb32);
  for (uint32_t y = 0; y < height; y++) {
    for (uint32_t x = 0; x < width; x++) {
      uint32_t sx = mirror ? (width - 1 - x) : x;
      uint32_t sp = y * width + sx;
      float r = src[sp].r, g = src[sp].g, b = src[sp].b;
      float y_val = 0.299f * r + 0.587f * g + 0.114f * b;
      y_plane[y * width + x] = static_cast<uint8_t>(std::clamp(y_val, 0.f, 255.f));
    }
  }
  for (uint32_t y = 0; y < height; y += 2) {
    for (uint32_t x = 0; x < width; x += 2) {
      uint32_t sx0 = mirror ? (width - 1 - x) : x;
      uint32_t sx1 = mirror ? (width - 1 - (x + 1)) : (x + 1);
      uint32_t sp0 = y * width + sx0;
      uint32_t sp1 = (y + 1) < height ? (y + 1) * width + sx0 : sp0;
      uint32_t sp2 = y * width + sx1;
      uint32_t sp3 = (y + 1) < height ? (y + 1) * width + sx1 : sp2;
      float r = (src[sp0].r + src[sp1].r + src[sp2].r + src[sp3].r) * 0.25f;
      float g = (src[sp0].g + src[sp1].g + src[sp2].g + src[sp3].g) * 0.25f;
      float b = (src[sp0].b + src[sp1].b + src[sp2].b + src[sp3].b) * 0.25f;
      float u_val = -0.169f * r - 0.331f * g + 0.5f * b + 128.f;
      float v_val = 0.5f * r - 0.419f * g - 0.081f * b + 128.f;
      uint32_t uv_idx = (y / 2) * width + x;
      uv_plane[uv_idx] = static_cast<uint8_t>(std::clamp(u_val, 0.f, 255.f));
      uv_plane[uv_idx + 1] = static_cast<uint8_t>(std::clamp(v_val, 0.f, 255.f));
    }
  }
}

}  // namespace

PreviewFrameRecorder::~PreviewFrameRecorder() {
  Stop();
}

void PreviewFrameRecorder::SetLastError(const char* step, HRESULT hr) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%s 0x%08X", step,
                static_cast<unsigned>(hr));
  last_error_ = buf;
}

bool PreviewFrameRecorder::Start(const std::string& file_path, uint32_t width,
                                 uint32_t height, uint32_t fps_num,
                                 uint32_t fps_den, bool mirror) {
  last_error_.clear();
  if (recording_ || file_path.empty() || width == 0 || height == 0 ||
      fps_den == 0) {
    last_error_ = "invalid params";
    return false;
  }
  std::lock_guard<std::mutex> lock(write_mutex_);
  file_path_ = file_path;
  width_ = width;
  height_ = height;
  fps_num_ = fps_num;
  fps_den_ = fps_den;
  mirror_ = mirror;
  first_timestamp_us_ = -1;
  duration_us_ = 0;

  if (!CreateSinkWriter(width_, height_, fps_num_, fps_den_)) {
    return false;
  }
  recording_ = true;
  return true;
}

void PreviewFrameRecorder::OnFrame(const uint8_t* rgb32_data,
                                   uint32_t data_length, uint64_t timestamp_us) {
  if (!recording_ || !sink_writer_ || !rgb32_data) return;
  const uint32_t expected = width_ * height_ * kBytesPerPixelRgb32;
  if (data_length < expected) return;

  std::lock_guard<std::mutex> lock(write_mutex_);
  if (!sink_writer_) return;

  if (first_timestamp_us_ < 0) {
    first_timestamp_us_ = static_cast<int64_t>(timestamp_us);
  }
  duration_us_ = timestamp_us - static_cast<uint64_t>(first_timestamp_us_);

  const uint32_t rgb32_size = width_ * height_ * kBytesPerPixelRgb32;
  LONGLONG time_100ns = static_cast<LONGLONG>(timestamp_us) * 10;
  LONGLONG duration_100ns =
      (10000000LL * fps_den_) / static_cast<LONGLONG>(fps_num_);

  if (input_is_rgb32_) {
    if (rgb32_frame_buffer_.size() != rgb32_size) {
      rgb32_frame_buffer_.resize(rgb32_size);
    }
    const auto* src = reinterpret_cast<const MFVideoFormatRGB32Pixel*>(rgb32_data);
    auto* dst = reinterpret_cast<MFVideoFormatRGB32Pixel*>(rgb32_frame_buffer_.data());
    if (mirror_) {
      for (uint32_t y = 0; y < height_; y++) {
        for (uint32_t x = 0; x < width_; x++) {
          dst[y * width_ + (width_ - 1 - x)] = src[y * width_ + x];
        }
      }
    } else {
      memcpy(rgb32_frame_buffer_.data(), rgb32_data, rgb32_size);
    }
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(rgb32_size), &buffer);
    if (FAILED(hr)) return;
    uint8_t* ptr = nullptr;
    DWORD max_len = 0, current_len = 0;
    hr = buffer->Lock(&ptr, &max_len, &current_len);
    if (FAILED(hr)) return;
    memcpy(ptr, rgb32_frame_buffer_.data(), rgb32_size);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(rgb32_size));
    ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) return;
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(time_100ns);
    sample->SetSampleDuration(duration_100ns);
    sink_writer_->WriteSample(stream_index_, sample.Get());
    return;
  }

  if (nv12_buffer_.size() != width_ * height_ * 3 / 2) {
    nv12_buffer_.resize(width_ * height_ * 3 / 2);
  }
  Rgb32ToNv12(rgb32_data, width_, height_, nv12_buffer_.data(), mirror_);

  ComPtr<IMFMediaBuffer> buffer;
  HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(nv12_buffer_.size()),
                                    &buffer);
  if (FAILED(hr)) return;

  uint8_t* ptr = nullptr;
  DWORD max_len = 0;
  DWORD current_len = 0;
  hr = buffer->Lock(&ptr, &max_len, &current_len);
  if (FAILED(hr)) return;
  memcpy(ptr, nv12_buffer_.data(), nv12_buffer_.size());
  buffer->Unlock();
  hr = buffer->SetCurrentLength(static_cast<DWORD>(nv12_buffer_.size()));
  if (FAILED(hr)) return;

  ComPtr<IMFSample> sample;
  hr = MFCreateSample(&sample);
  if (FAILED(hr)) return;
  hr = sample->AddBuffer(buffer.Get());
  if (FAILED(hr)) return;

  hr = sample->SetSampleTime(time_100ns);
  if (FAILED(hr)) return;
  sample->SetSampleDuration(duration_100ns);

  hr = sink_writer_->WriteSample(stream_index_, sample.Get());
  (void)hr;
}

void PreviewFrameRecorder::Stop() {
  std::lock_guard<std::mutex> lock(write_mutex_);
  if (!recording_) return;
  recording_ = false;
  if (sink_writer_) {
    sink_writer_->Finalize();
    sink_writer_.Reset();
  }
  media_sink_.Reset();
  byte_stream_.Reset();
  nv12_buffer_.clear();
}

bool PreviewFrameRecorder::CreateSinkWriter(uint32_t width, uint32_t height,
                                           uint32_t fps_num, uint32_t fps_den) {
  // 先释放可能残留的句柄，避免上次失败后文件仍被占用导致「另一个程序正在使用此文件」
  sink_writer_.Reset();
  media_sink_.Reset();
  byte_stream_.Reset();
  input_is_rgb32_ = false;

  // H.264/NV12 要求宽高为偶数
  width = (width + 1) & ~1u;
  height = (height + 1) & ~1u;

  std::wstring path_w = Utf16FromUtf8(file_path_);
  for (wchar_t& c : path_w) {
    if (c == L'/') c = L'\\';
  }

  ComPtr<IMFMediaType> input_type;
  HRESULT hr = MFCreateMediaType(&input_type);
  if (FAILED(hr)) {
    SetLastError("MFCreateMediaType(input)", hr);
    return false;
  }
  hr = input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  if (FAILED(hr)) { SetLastError("SetGUID(input)", hr); return false; }
  hr = input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  if (FAILED(hr)) { SetLastError("SetGUID(NV12)", hr); return false; }
  hr = MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE, width, height);
  if (FAILED(hr)) { SetLastError("MFSetAttributeSize(input)", hr); return false; }
  hr = MFSetAttributeRatio(input_type.Get(), MF_MT_FRAME_RATE, fps_num,
                            fps_den);
  if (FAILED(hr)) { SetLastError("MFSetAttributeRatio(input)", hr); return false; }
  hr = input_type->SetUINT32(MF_MT_INTERLACE_MODE,
                             MFVideoInterlace_Progressive);
  if (FAILED(hr)) { SetLastError("SetUINT32(INTERLACE)", hr); return false; }
  hr = input_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
  if (FAILED(hr)) { SetLastError("SetUINT32(ALL_SAMPLES_INDEPENDENT)", hr); return false; }
  // 部分编码器要求 NV12 的 stride（Y 平面每行字节数），失败则忽略
  input_type->SetUINT32(MF_MT_DEFAULT_STRIDE, width);

  // RGB32 与预览同源，部分 SinkWriter 管线接受该格式，优先尝试可避免 NV12 类型被拒
  ComPtr<IMFMediaType> input_type_rgb32;
  hr = MFCreateMediaType(&input_type_rgb32);
  if (SUCCEEDED(hr)) {
    input_type_rgb32->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input_type_rgb32->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    MFSetAttributeSize(input_type_rgb32.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(input_type_rgb32.Get(), MF_MT_FRAME_RATE, fps_num,
                        fps_den);
    input_type_rgb32->SetUINT32(MF_MT_INTERLACE_MODE,
                                MFVideoInterlace_Progressive);
    input_type_rgb32->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    input_type_rgb32->SetUINT32(MF_MT_DEFAULT_STRIDE,
                                static_cast<UINT32>(width * 4));
  }

  ComPtr<IMFMediaType> output_type;
  hr = MFCreateMediaType(&output_type);
  if (FAILED(hr)) {
    SetLastError("MFCreateMediaType(output)", hr);
    return false;
  }
  hr = output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  if (FAILED(hr)) { SetLastError("SetGUID(output)", hr); return false; }
  hr = output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  if (FAILED(hr)) { SetLastError("SetGUID(H264)", hr); return false; }
  hr = MFSetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE, width, height);
  if (FAILED(hr)) { SetLastError("MFSetAttributeSize(output)", hr); return false; }
  hr = MFSetAttributeRatio(output_type.Get(), MF_MT_FRAME_RATE, fps_num,
                           fps_den);
  if (FAILED(hr)) { SetLastError("MFSetAttributeRatio(output)", hr); return false; }

  // 先尝试标准 URL 流程（含 file:// 回退），并在 SetInputMediaType 失败时尝试各编码器的 NV12 类型。
  auto try_url_writer = [&](const wchar_t* url) -> bool {
    if (FAILED(MFCreateSinkWriterFromURL(url, nullptr, nullptr, &sink_writer_)))
      return false;
    if (FAILED(sink_writer_->AddStream(output_type.Get(), &stream_index_))) {
      sink_writer_.Reset();
      return false;
    }
    bool input_ok = false;
    if (input_type_rgb32 &&
        SUCCEEDED(sink_writer_->SetInputMediaType(
            stream_index_, input_type_rgb32.Get(), nullptr))) {
      input_ok = true;
      input_is_rgb32_ = true;
    }
    if (!input_ok) {
      hr = sink_writer_->SetInputMediaType(stream_index_, input_type.Get(),
                                           nullptr);
      if (SUCCEEDED(hr)) input_ok = true;
    }
    if (!input_ok) {
      MFT_REGISTER_TYPE_INFO out_info = {MFMediaType_Video, MFVideoFormat_H264};
      IMFActivate** activates = nullptr;
      UINT32 mft_count = 0;
      for (int pass = 0; pass < 2 && !input_ok; pass++) {
        UINT32 flags = pass == 0 ? MFT_ENUM_FLAG_SYNCMFT
                                 : MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT;
        if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr,
                             &out_info, &activates, &mft_count)) ||
            mft_count == 0)
          continue;
        for (UINT32 m = 0; m < mft_count && !input_ok; m++) {
          ComPtr<IMFTransform> transform;
          if (FAILED(activates[m]->ActivateObject(IID_PPV_ARGS(&transform))))
            continue;
          if (FAILED(transform->SetOutputType(0, output_type.Get(), 0)))
            continue;
          for (DWORD t = 0;; t++) {
            ComPtr<IMFMediaType> preferred;
            HRESULT hr_t =
                transform->GetInputAvailableType(0, t, &preferred);
            if (hr_t == MF_E_NO_MORE_TYPES) break;
            if (FAILED(hr_t)) break;
            GUID st = {};
            if (FAILED(preferred->GetGUID(MF_MT_SUBTYPE, &st)) ||
                st != MFVideoFormat_NV12)
              continue;
            ComPtr<IMFMediaType> trial;
            if (FAILED(MFCreateMediaType(&trial))) continue;
            if (FAILED(preferred->CopyAllItems(trial.Get()))) continue;
            if (FAILED(MFSetAttributeSize(trial.Get(), MF_MT_FRAME_SIZE,
                                          width, height)))
              continue;
            if (FAILED(MFSetAttributeRatio(trial.Get(), MF_MT_FRAME_RATE,
                                           fps_num, fps_den)))
              continue;
            if (SUCCEEDED(sink_writer_->SetInputMediaType(
                    stream_index_, trial.Get(), nullptr))) {
              input_ok = true;
              break;
            }
          }
        }
        for (UINT32 i = 0; i < mft_count; i++)
          if (activates[i]) activates[i]->Release();
        CoTaskMemFree(activates);
      }
    }
    if (!input_ok) {
      sink_writer_.Reset();
      return false;
    }
    if (FAILED(sink_writer_->BeginWriting())) {
      sink_writer_.Reset();
      return false;
    }
    return true;
  };

  if (try_url_writer(path_w.c_str())) return true;
  std::wstring url_str = L"file:///";
  for (wchar_t c : path_w) url_str += (c == L'\\') ? L'/' : c;
  if (try_url_writer(url_str.c_str())) return true;

  // URL 流程失败则用 MFCreateFile + MPEG4MediaSink，再逐个尝试编码器输入类型。
  hr = MFCreateFile(MF_ACCESSMODE_WRITE, MF_OPENMODE_DELETE_IF_EXIST,
                    MF_FILEFLAGS_NONE, path_w.c_str(), &byte_stream_);
  if (FAILED(hr)) {
    std::wstring url = L"file:///";
    for (wchar_t c : path_w) {
      url += (c == L'\\') ? L'/' : c;
    }
    hr = MFCreateFile(MF_ACCESSMODE_WRITE, MF_OPENMODE_DELETE_IF_EXIST,
                      MF_FILEFLAGS_NONE, url.c_str(), &byte_stream_);
  }
  if (FAILED(hr)) {
    SetLastError("MFCreateFile", hr);
    return false;
  }

  hr = MFCreateMPEG4MediaSink(byte_stream_.Get(), output_type.Get(), nullptr,
                              &media_sink_);
  if (FAILED(hr)) {
    SetLastError("MFCreateMPEG4MediaSink", hr);
    byte_stream_.Reset();
    return false;
  }

  hr = MFCreateSinkWriterFromMediaSink(media_sink_.Get(), nullptr,
                                       &sink_writer_);
  if (FAILED(hr)) {
    SetLastError("MFCreateSinkWriterFromMediaSink", hr);
    media_sink_.Reset();
    byte_stream_.Reset();
    return false;
  }

  stream_index_ = 0;
  bool input_type_set = false;
  if (input_type_rgb32 &&
      SUCCEEDED(sink_writer_->SetInputMediaType(
          stream_index_, input_type_rgb32.Get(), nullptr))) {
    input_type_set = true;
    input_is_rgb32_ = true;
  }
  if (!input_type_set) {
    hr = sink_writer_->SetInputMediaType(stream_index_, input_type.Get(),
                                         nullptr);
    if (SUCCEEDED(hr)) input_type_set = true;
  }
  const HRESULT hr_first_set_input = hr;
  if (!input_type_set) {
    MFT_REGISTER_TYPE_INFO out_info = {MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activates = nullptr;
    UINT32 mft_count = 0;
    for (int pass = 0; pass < 2 && !input_type_set; pass++) {
      const UINT32 flags = (pass == 0)
                               ? MFT_ENUM_FLAG_SYNCMFT
                               : MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT;
      hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &out_info,
                     &activates, &mft_count);
      if (FAILED(hr) || mft_count == 0) continue;
      for (UINT32 m = 0; m < mft_count && !input_type_set; m++) {
        ComPtr<IMFTransform> transform;
        if (FAILED(activates[m]->ActivateObject(IID_PPV_ARGS(&transform))))
          continue;
        if (FAILED(transform->SetOutputType(0, output_type.Get(), 0)))
          continue;
        for (DWORD t = 0;; t++) {
          ComPtr<IMFMediaType> preferred;
          HRESULT hr_t =
              transform->GetInputAvailableType(0, t, &preferred);
          if (hr_t == MF_E_NO_MORE_TYPES) break;
          if (FAILED(hr_t)) break;
          GUID st = {};
          if (FAILED(preferred->GetGUID(MF_MT_SUBTYPE, &st)) ||
              st != MFVideoFormat_NV12)
            continue;
          ComPtr<IMFMediaType> trial;
          if (FAILED(MFCreateMediaType(&trial))) continue;
          if (FAILED(preferred->CopyAllItems(trial.Get()))) continue;
          if (FAILED(MFSetAttributeSize(trial.Get(), MF_MT_FRAME_SIZE, width,
                                        height)))
            continue;
          if (FAILED(MFSetAttributeRatio(trial.Get(), MF_MT_FRAME_RATE,
                                         fps_num, fps_den)))
            continue;
          if (SUCCEEDED(sink_writer_->SetInputMediaType(stream_index_,
                                                        trial.Get(), nullptr))) {
            input_type_set = true;
            break;
          }
        }
      }
      for (UINT32 i = 0; i < mft_count; i++) {
        if (activates[i]) activates[i]->Release();
      }
      CoTaskMemFree(activates);
      activates = nullptr;
      mft_count = 0;
    }
  }

  if (!input_type_set) {
    SetLastError("SetInputMediaType", hr_first_set_input);
    sink_writer_.Reset();
    media_sink_.Reset();
    byte_stream_.Reset();
    return false;
  }

  hr = sink_writer_->BeginWriting();
  if (FAILED(hr)) {
    SetLastError("BeginWriting", hr);
    sink_writer_.Reset();
    media_sink_.Reset();
    byte_stream_.Reset();
    return false;
  }

  return true;
}

void PreviewFrameRecorder::Rgb32ToNv12(const uint8_t* rgb32, uint32_t width,
                                      uint32_t height, uint8_t* nv12_out,
                                      bool mirror) {
  Rgb32ToNv12Impl(rgb32, width, height, nv12_out, mirror);
}

}  // namespace camera_windows
