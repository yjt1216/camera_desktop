#include "include/camera_recorder/camera_recorder_plugin_c_api.h"

#include <flutter/plugin_registrar_windows.h>

#include "camera_plugin.h"

void CameraRecorderPluginCApiRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  camera_windows::CameraPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
