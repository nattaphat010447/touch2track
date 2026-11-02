cmake_minimum_required(VERSION 3.16)
project(touch2track CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(touch2track WIN32 src/touch2track.cpp res/app.rc)
set_source_files_properties(res/app.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/res/app.ico)
target_compile_definitions(touch2track PRIVATE UNICODE _UNICODE)
target_link_libraries(touch2track PRIVATE hid winmm shell32 user32 gdi32 advapi32)

if(MINGW)
  target_link_options(touch2track PRIVATE -static -municode)
endif()
if(MSVC)
  target_compile_options(touch2track PRIVATE /W3 /utf-8)
endif()
