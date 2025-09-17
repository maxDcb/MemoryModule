cmake_minimum_required(VERSION 3.20)
project(MemoryDllDemo LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

# --- DLL ---
add_library(TestDll SHARED TestDll.cpp)

# --- Loader executable ---
add_executable(TestLoader test.cpp)
target_link_libraries(TestLoader PRIVATE MemoryModule)

# Put DLL next to exe after build
add_custom_command(TARGET TestLoader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:TestDll>
            $<TARGET_FILE_DIR:TestLoader>)
