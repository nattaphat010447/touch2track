cmake_minimum_required(VERSION 3.16)
project(touch2track CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The app itself --------------------------------------------------------------------------
add_executable(touch2track WIN32 src/touch2track.cpp src/core.h res/app.rc)
set_source_files_properties(res/app.rc PROPERTIES
  OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/res/app.ico;${CMAKE_CURRENT_SOURCE_DIR}/res/app.manifest")
target_compile_definitions(touch2track PRIVATE UNICODE _UNICODE)
target_link_libraries(touch2track PRIVATE hid winmm shell32 user32 gdi32 advapi32 ole32 oleaut32 wtsapi32 uuid)

if(MINGW)
  target_link_options(touch2track PRIVATE -static -municode)
endif()
if(MSVC)
  # Link the C/C++ runtime statically so the exe has no Visual C++ Redistributable dependency.
  set_property(TARGET touch2track PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
  target_compile_options(touch2track PRIVATE /W3 /utf-8)
  # The manifest is embedded by res/app.rc; don't let the linker add a second one.
  target_link_options(touch2track PRIVATE /MANIFEST:NO)
endif()

# Unit tests (platform-independent logic in src/core.h) --------------------------------------
option(TOUCH2TRACK_BUILD_TESTS "Build the unit tests" ON)
if(TOUCH2TRACK_BUILD_TESTS)
  enable_testing()
  add_executable(core_test tests/core_test.cpp src/core.h)
  if(MINGW)
    target_link_options(core_test PRIVATE -static)
  endif()
  if(MSVC)
    set_property(TARGET core_test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_options(core_test PRIVATE /W3 /utf-8)
  endif()
  add_test(NAME core_test COMMAND core_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/touchpad_frames.txt")
endif()
