cmake_minimum_required(VERSION 3.24)
project(FH2VR VERSION 0.4.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT MSVC)
  message(FATAL_ERROR "FH2VR currently requires Windows x64 and MSVC")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_FRAMEWORK OFF CACHE BOOL "" FORCE)
set(BUILD_UNIVERSAL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  openvr
  GIT_REPOSITORY https://github.com/ValveSoftware/openvr.git
  GIT_TAG v2.15.6
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(openvr)

FetchContent_Declare(
  minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG v1.3.4
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(minhook)

add_library(FH2VR SHARED
  src/FH2VR.cpp
  src/StandaloneEntry.cpp
  src/HeadsetPreview.cpp
  src/RendererM10.cpp
  src/FovMatch.cpp
  src/UiModeDetector.cpp
  src/GameBaseLocator.cpp
  src/CameraResearchV2.cpp
  src/CameraResearchBootstrap.cpp
)
target_include_directories(FH2VR PRIVATE include)
target_compile_definitions(FH2VR PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX OPENVR_BUILD_STATIC)
target_compile_options(FH2VR PRIVATE /W4 /WX /EHsc /utf-8)
target_link_libraries(FH2VR PRIVATE openvr_api64 minhook user32 gdi32 d3d11 d3d12 d3dcompiler dxgi)
set_target_properties(FH2VR PROPERTIES OUTPUT_NAME FH2VR)

install(TARGETS FH2VR RUNTIME DESTINATION . COMPONENT FH2VR)
install(FILES mod.json README.md THIRD_PARTY.md DESTINATION . COMPONENT FH2VR)
install(FILES "${openvr_SOURCE_DIR}/LICENSE"
        DESTINATION licenses
        RENAME OpenVR-LICENSE.txt
        COMPONENT FH2VR)
install(FILES "${minhook_SOURCE_DIR}/LICENSE.txt"
        DESTINATION licenses
        RENAME MinHook-LICENSE.txt
        COMPONENT FH2VR)
