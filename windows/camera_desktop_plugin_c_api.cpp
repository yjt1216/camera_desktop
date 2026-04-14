#include "include/camera_desktop/camera_desktop_plugin.h"

#include <flutter/plugin_registrar_windows.h>

#include "camera_plugin.h"

void CameraDesktopPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  camera_windows::CameraPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
