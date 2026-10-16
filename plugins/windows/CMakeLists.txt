cmake_minimum_required(VERSION 3.20)
project(lfi_wic C)

# Explorer loads this into its own processes, so it can't count on the Visual C++ runtime being installed
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)

add_library(lfi_wic SHARED lfi_wic.c lfi_wic.def)
target_include_directories(lfi_wic PRIVATE ../../dist ..)
target_link_libraries(lfi_wic PRIVATE windowscodecs ole32 shell32 advapi32 uuid)
if(MSVC)
	target_compile_options(lfi_wic PRIVATE /W4)
endif()
