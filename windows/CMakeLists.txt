# The Flutter tooling requires that developers have a version of Visual Studio
# installed that includes CMake 3.14 or later. You should not increase this
# version, as doing so will cause the plugin to fail to compile for some
# customers of the plugin.
cmake_minimum_required(VERSION 3.14)

set(PROJECT_NAME "camera_desktop")
project(${PROJECT_NAME} LANGUAGES CXX)

cmake_policy(VERSION 3.14...3.25)

# This value is used when generating builds using this plugin, so it must
# not be changed.
set(PLUGIN_NAME "camera_desktop_plugin")

list(APPEND PLUGIN_SOURCES
  "camera_plugin.h"
  "camera_plugin.cpp"
  "camera.h"
  "camera.cpp"
  "capture_controller.h"
  "capture_controller.cpp"
  "capture_controller_listener.h"
  "capture_engine_listener.h"
  "capture_engine_listener.cpp"
  "string_utils.h"
  "string_utils.cpp"
  "capture_device_info.h"
  "capture_device_info.cpp"
  "messages.g.h"
  "messages.g.cpp"
  "preview_handler.h"
  "preview_handler.cpp"
  "preview_frame_recorder.h"
  "preview_frame_recorder.cpp"
  "record_handler.h"
  "record_handler.cpp"
  "photo_handler.h"
  "photo_handler.cpp"
  "texture_handler.h"
  "texture_handler.cpp"
  "com_heap_ptr.h"
)

add_library(${PLUGIN_NAME} SHARED
  "camera_desktop_plugin_c_api.cpp"
  "include/camera_desktop/camera_desktop_plugin.h"
  ${PLUGIN_SOURCES}
)

apply_standard_settings(${PLUGIN_NAME})
set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)
target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(${PLUGIN_NAME} PRIVATE flutter flutter_wrapper_plugin)
target_link_libraries(${PLUGIN_NAME} PRIVATE mf mfplat mfuuid mfreadwrite ole32 d3d11)

set(camera_desktop_bundled_libraries
  ""
  PARENT_SCOPE
)

if (${include_${PROJECT_NAME}_tests})
set(TEST_RUNNER "${PROJECT_NAME}_test")
enable_testing()

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/release-1.11.0.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "Disable installation of googletest" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(${TEST_RUNNER}
  "test/camera_desktop_plugin_test.cpp"
  ${PLUGIN_SOURCES}
)
apply_standard_settings(${TEST_RUNNER})
target_include_directories(${TEST_RUNNER} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(${TEST_RUNNER} PRIVATE flutter_wrapper_plugin)
target_link_libraries(${TEST_RUNNER} PRIVATE gtest_main gmock)
target_link_libraries(${TEST_RUNNER} PRIVATE mf mfplat mfuuid mfreadwrite ole32 d3d11)

add_custom_command(TARGET ${TEST_RUNNER} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${FLUTTER_LIBRARY}" $<TARGET_FILE_DIR:${TEST_RUNNER}>
)

include(GoogleTest)
gtest_discover_tests(${TEST_RUNNER})
endif()
