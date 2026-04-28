cmake_minimum_required (VERSION 3.24)

project(MemoryModule VERSION 0.0.0 LANGUAGES CXX C)

set (PLATFORM "x86_64" CACHE STRING "Platform to compile for")
message (STATUS "Compile for ${PLATFORM} platform")

add_library (MemoryModule STATIC MemoryModule.cpp MemoryModule.hpp)
set_property(TARGET MemoryModule PROPERTY  MSVC_RUNTIME_LIBRARY "MultiThreaded")
target_include_directories(MemoryModule PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

if(C2CORE_BUILD_TESTS)
  add_subdirectory(tests)
endif()

