// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "record_handler.h"

#include <mfapi.h>
#include <mfcaptureengine.h>

#include <windows.h>

#include <cassert>
#include <chrono>
#include <sstream>
#include <string>

#include "logging.h"
#include "string_utils.h"

namespace camera_windows {

using Microsoft::WRL::ComPtr;

// Builds a *minimal* compressed video media type for the capture-engine record
// sink. CopyAllItems()+SetSubtype(H264) leaves MJPEG/NV12/etc.-specific keys that
// still trigger MF_E_INVALIDMEDIATYPE on many drivers even after deleting a few
// well-known attributes — so we only copy geometry/timing that H.264 accepts.
HRESULT BuildMediaTypeForVideoCapture(IMFMediaType* src_media_type,
                                      IMFMediaType** video_record_media_type,
                                      GUID capture_format) {
  assert(src_media_type);
  ComPtr<IMFMediaType> new_media_type;

  HRESULT hr = MFCreateMediaType(&new_media_type);
  if (FAILED(hr)) {
    return hr;
  }

  hr = new_media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  if (FAILED(hr)) {
    return hr;
  }

  GUID src_subtype = {};
  const bool src_has_subtype =
      SUCCEEDED(src_media_type->GetGUID(MF_MT_SUBTYPE, &src_subtype));

  // If the negotiated record stream is already H.264, keep that subtype.
  // Re-labeling as MFVideoFormat_H264 when the source is H264_ES (or similar)
  // can still produce MF_E_INVALIDMEDIATYPE on some devices.
  GUID output_subtype = capture_format;
  if (src_has_subtype &&
      (IsEqualGUID(src_subtype, MFVideoFormat_H264) ||
       IsEqualGUID(src_subtype, MFVideoFormat_H264_ES))) {
    output_subtype = src_subtype;
  }

  hr = new_media_type->SetGUID(MF_MT_SUBTYPE, output_subtype);
  if (FAILED(hr)) {
    return hr;
  }

  UINT32 frame_width = 0;
  UINT32 frame_height = 0;
  hr = MFGetAttributeSize(src_media_type, MF_MT_FRAME_SIZE, &frame_width,
                          &frame_height);
  if (FAILED(hr) || frame_width == 0 || frame_height == 0) {
    return FAILED(hr) ? hr : E_INVALIDARG;
  }
  hr = MFSetAttributeSize(new_media_type.Get(), MF_MT_FRAME_SIZE, frame_width,
                          frame_height);
  if (FAILED(hr)) {
    return hr;
  }

  // Do not copy MF_MT_FRAME_RATE from the source: MJPEG / variable sources
  // often carry values that are incompatible with the H.264 sink. FPS is
  // applied later from [PlatformMediaSettings] in InitRecordSink.

  UINT32 par_num = 1;
  UINT32 par_den = 1;
  if (SUCCEEDED(MFGetAttributeRatio(src_media_type, MF_MT_PIXEL_ASPECT_RATIO,
                                    &par_num, &par_den)) &&
      par_den > 0) {
    (void)MFSetAttributeRatio(new_media_type.Get(), MF_MT_PIXEL_ASPECT_RATIO,
                              par_num, par_den);
  }

  // Encoders / muxers often reject unknown interlace flags carried from the
  // source negotiation buffer.
  (void)new_media_type->SetUINT32(MF_MT_INTERLACE_MODE,
                                  MFVideoInterlace_Progressive);

  new_media_type.CopyTo(video_record_media_type);
  return S_OK;
}

// Queries interface object from collection.
template <class Q>
HRESULT GetCollectionObject(IMFCollection* pCollection, DWORD index,
                            Q** ppObj) {
  ComPtr<IUnknown> pUnk;
  HRESULT hr = pCollection->GetElement(index, pUnk.GetAddressOf());
  if (FAILED(hr)) {
    return hr;
  }
  return pUnk->QueryInterface(IID_PPV_ARGS(ppObj));
}

// Initializes media type for audo capture.
HRESULT BuildMediaTypeForAudioCapture(IMFMediaType** audio_record_media_type) {
  ComPtr<IMFAttributes> audio_output_attributes;
  ComPtr<IMFMediaType> src_media_type;
  ComPtr<IMFMediaType> new_media_type;
  ComPtr<IMFCollection> available_output_types;
  DWORD mt_count = 0;

  HRESULT hr = MFCreateAttributes(&audio_output_attributes, 1);
  if (FAILED(hr)) {
    return hr;
  }

  // Enumerates only low latency audio outputs.
  hr = audio_output_attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
  if (FAILED(hr)) {
    return hr;
  }

  DWORD mft_flags = (MFT_ENUM_FLAG_ALL & (~MFT_ENUM_FLAG_FIELDOFUSE)) |
                    MFT_ENUM_FLAG_SORTANDFILTER;

  hr = MFTranscodeGetAudioOutputAvailableTypes(
      MFAudioFormat_AAC, mft_flags, audio_output_attributes.Get(),
      available_output_types.GetAddressOf());
  if (FAILED(hr)) {
    return hr;
  }

  hr = GetCollectionObject(available_output_types.Get(), 0,
                           src_media_type.GetAddressOf());
  if (FAILED(hr)) {
    return hr;
  }

  hr = available_output_types->GetElementCount(&mt_count);
  if (FAILED(hr)) {
    return hr;
  }

  if (mt_count == 0) {
    // No sources found, mark process as failure.
    return E_FAIL;
  }

  // Create new media type to copy original media type to.
  hr = MFCreateMediaType(&new_media_type);
  if (FAILED(hr)) {
    return hr;
  }

  hr = src_media_type->CopyAllItems(new_media_type.Get());
  if (FAILED(hr)) {
    return hr;
  }

  new_media_type.CopyTo(audio_record_media_type);
  return hr;
}

// Helper function to set the frame rate on a video media type.
inline HRESULT SetFrameRate(IMFMediaType* pType, UINT32 numerator,
                            UINT32 denominator) {
  return MFSetAttributeRatio(pType, MF_MT_FRAME_RATE, numerator, denominator);
}

// Helper function to set the video bitrate on a video media type.
inline HRESULT SetVideoBitrate(IMFMediaType* pType, UINT32 bitrate) {
  return pType->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
}

// Helper function to set the audio bitrate on an audio media type.
inline HRESULT SetAudioBitrate(IMFMediaType* pType, UINT32 bitrate) {
  return pType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bitrate);
}

HRESULT RecordHandler::InitRecordSink(IMFCaptureEngine* capture_engine,
                                      IMFMediaType* base_media_type) {
  assert(capture_engine);
  assert(base_media_type);
  if (file_path_.empty()) {
    return E_INVALIDARG;
  }

  HRESULT hr = S_OK;
  if (record_sink_) {
    // If record sink already exists, only update output filename.
    hr = record_sink_->SetOutputFileName(Utf16FromUtf8(file_path_).c_str());

    if (FAILED(hr)) {
      record_sink_ = nullptr;
    }
    return hr;
  }

  ComPtr<IMFMediaType> video_record_media_type;
  ComPtr<IMFCaptureSink> capture_sink;

  // Gets sink from capture engine with record type.

  hr = capture_engine->GetSink(MF_CAPTURE_ENGINE_SINK_TYPE_RECORD,
                               &capture_sink);
  if (FAILED(hr)) {
    return hr;
  }

  hr = capture_sink.As(&record_sink_);
  if (FAILED(hr)) {
    return hr;
  }

  // Removes existing streams if available.
  hr = record_sink_->RemoveAllStreams();
  if (FAILED(hr)) {
    return hr;
  }

  hr = BuildMediaTypeForVideoCapture(base_media_type,
                                     video_record_media_type.GetAddressOf(),
                                     MFVideoFormat_H264);
  if (FAILED(hr)) {
    return hr;
  }

  if (media_settings_.frames_per_second()) {
    assert(*media_settings_.frames_per_second() > 0);
    SetFrameRate(video_record_media_type.Get(),
                 static_cast<UINT32>(*media_settings_.frames_per_second()), 1);
  } else {
    // Minimal record types omit source frame rate; sinks still need a ratio.
    (void)SetFrameRate(video_record_media_type.Get(), 30, 1);
  }

  if (media_settings_.video_bitrate()) {
    assert(*media_settings_.video_bitrate() > 0);
    SetVideoBitrate(video_record_media_type.Get(),
                    static_cast<UINT32>(*media_settings_.video_bitrate()));
  }

  DWORD video_record_sink_stream_index;
  hr = record_sink_->AddStream(
      (DWORD)MF_CAPTURE_ENGINE_PREFERRED_SOURCE_STREAM_FOR_VIDEO_RECORD,
      video_record_media_type.Get(), nullptr, &video_record_sink_stream_index);
  if (FAILED(hr)) {
    return hr;
  }

  if (media_settings_.enable_audio()) {
    ComPtr<IMFMediaType> audio_record_media_type;
    HRESULT audio_capture_hr = S_OK;
    audio_capture_hr =
        BuildMediaTypeForAudioCapture(audio_record_media_type.GetAddressOf());

    if (SUCCEEDED(audio_capture_hr)) {
      if (media_settings_.audio_bitrate()) {
        assert(*media_settings_.audio_bitrate() > 0);
        SetAudioBitrate(audio_record_media_type.Get(),
                        static_cast<UINT32>(*media_settings_.audio_bitrate()));
      }

      DWORD audio_record_sink_stream_index;
      const HRESULT audio_stream_hr = record_sink_->AddStream(
          (DWORD)MF_CAPTURE_ENGINE_PREFERRED_SOURCE_STREAM_FOR_AUDIO,
          audio_record_media_type.Get(), nullptr,
          &audio_record_sink_stream_index);
      if (FAILED(audio_stream_hr)) {
        // Video-only recording: AAC / muxer mismatch is common on USB and
        // virtual cameras; do not fail the whole session.
        DebugLog("InitRecordSink: audio AddStream failed hr=" +
                 HrToString(audio_stream_hr) + ", continuing video-only");
      }
    }
  }

  hr = record_sink_->SetOutputFileName(Utf16FromUtf8(file_path_).c_str());

  return hr;
}

HRESULT RecordHandler::StartRecord(const std::string& file_path,
                                   IMFCaptureEngine* capture_engine,
                                   IMFMediaType* base_media_type) {
  assert(capture_engine);
  assert(base_media_type);

  if (file_path.empty()) {
    WCHAR temp_dir[MAX_PATH];
    const DWORD tn = GetTempPathW(MAX_PATH, temp_dir);
    std::wstring wpath;
    const auto now =
        std::chrono::steady_clock::now().time_since_epoch().count();
    if (tn > 0 && tn < MAX_PATH) {
      std::wostringstream ss;
      ss << temp_dir << L"camera_desktop_video_" << now << L".mp4";
      wpath = ss.str();
    } else {
      std::wostringstream ss;
      ss << L"camera_desktop_video_" << now << L".mp4";
      wpath = ss.str();
    }
    file_path_ = Utf8FromUtf16(wpath);
  } else {
    file_path_ = file_path;
  }
  recording_start_timestamp_us_ = -1;
  recording_duration_us_ = 0;

  HRESULT hr = InitRecordSink(capture_engine, base_media_type);
  if (FAILED(hr)) {
    return hr;
  }

  recording_state_ = RecordState::kStarting;
  return capture_engine->StartRecord();
}

HRESULT RecordHandler::StopRecord(IMFCaptureEngine* capture_engine) {
  if (recording_state_ == RecordState::kRunning) {
    recording_state_ = RecordState::kStopping;
    return capture_engine->StopRecord(true, false);
  }
  return E_FAIL;
}

void RecordHandler::OnRecordStarted() {
  if (recording_state_ == RecordState::kStarting) {
    recording_state_ = RecordState::kRunning;
  }
}

void RecordHandler::OnRecordStopped() {
  if (recording_state_ == RecordState::kStopping) {
    file_path_ = "";
    recording_start_timestamp_us_ = -1;
    recording_duration_us_ = 0;
    recording_state_ = RecordState::kNotStarted;
  }
}

void RecordHandler::UpdateRecordingTime(uint64_t timestamp) {
  if (recording_start_timestamp_us_ < 0) {
    recording_start_timestamp_us_ = timestamp;
  }

  recording_duration_us_ = (timestamp - recording_start_timestamp_us_);
}

}  // namespace camera_windows
