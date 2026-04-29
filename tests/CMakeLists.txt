cmake_minimum_required(VERSION 3.20)
project(MemoryDllDemo LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

# --- DLL ---
add_library(TestDll SHARED TestDll.cpp)
set_property(TARGET TestDll PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

# --- EXE ---
add_executable(TestExe WIN32 TestExe.cpp)
set_property(TARGET TestExe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

# --- Loader executable ---
add_executable(TestLoader test.cpp)
target_link_libraries(TestLoader PRIVATE MemoryModule)
set_property(TARGET TestLoader PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

# Put DLL and EXE next to exe after build
add_custom_command(TARGET TestLoader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:TestDll>
            $<TARGET_FILE_DIR:TestLoader>)

add_custom_command(TARGET TestLoader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:TestExe>
            $<TARGET_FILE_DIR:TestLoader>)

add_test(NAME memorymodule_tests COMMAND $<TARGET_FILE:TestLoader>)
set_tests_properties(memorymodule_tests PROPERTIES
    WORKING_DIRECTORY $<TARGET_FILE_DIR:TestLoader>)
