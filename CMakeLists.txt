cmake_minimum_required(VERSION 3.24)
project(FH2VR VERSION 0.1.0 LANGUAGES CXX)

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

add_library(FH2VR SHARED src/FH2VR.cpp)
target_include_directories(FH2VR PRIVATE include)
target_compile_definitions(FH2VR PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX OPENVR_BUILD_STATIC)
target_compile_options(FH2VR PRIVATE /W4 /WX /EHsc /utf-8)
target_link_libraries(FH2VR PRIVATE openvr_api64 user32)
set_target_properties(FH2VR PROPERTIES OUTPUT_NAME FH2VR)

install(TARGETS FH2VR RUNTIME DESTINATION . COMPONENT FH2VR)
install(FILES mod.json README.md THIRD_PARTY.md DESTINATION . COMPONENT FH2VR)
