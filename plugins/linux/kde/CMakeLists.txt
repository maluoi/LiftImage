cmake_minimum_required(VERSION 3.20)
project(lfithumbnail CXX)

# GCC skips the decoder's loops below -O3
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

find_package(ECM 6.0 REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs)
include(KDECMakeSettings)
find_package(KF6 REQUIRED COMPONENTS KIO)

# Dolphin's thumbnails
kcoreaddons_add_plugin(lfithumbnail SOURCES lfithumbnail.cpp INSTALL_NAMESPACE "kf6/thumbcreator")
target_include_directories(lfithumbnail PRIVATE ../../../dist ../..)
target_link_libraries(lfithumbnail KF6::KIOGui)
target_compile_options(lfithumbnail PRIVATE -Wall -Wextra)

# QImageReader support, for Gwenview and other Qt apps
add_library(lfiimageformat MODULE lfiimageformat.cpp)
target_include_directories(lfiimageformat PRIVATE ../../../dist ../..)
target_link_libraries(lfiimageformat Qt6::Gui)
target_compile_options(lfiimageformat PRIVATE -Wall -Wextra)
install(TARGETS lfiimageformat DESTINATION ${KDE_INSTALL_QTPLUGINDIR}/imageformats)
