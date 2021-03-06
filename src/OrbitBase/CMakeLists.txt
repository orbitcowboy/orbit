# Copyright (c) 2020 The Orbit Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

cmake_minimum_required(VERSION 3.15)

project(OrbitBase)

add_library(OrbitBase STATIC)

target_compile_options(OrbitBase PRIVATE ${STRICT_COMPILE_FLAGS})

target_compile_features(OrbitBase PUBLIC cxx_std_17)

target_include_directories(OrbitBase PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include)

target_include_directories(OrbitBase PRIVATE
        ${CMAKE_CURRENT_LIST_DIR})

target_sources(OrbitBase PRIVATE
        include/OrbitBase/Action.h
        include/OrbitBase/ExecutablePath.h
        include/OrbitBase/Logging.h
        include/OrbitBase/MakeUniqueForOverwrite.h
        include/OrbitBase/GetProcessIds.h
        include/OrbitBase/Profiling.h
        include/OrbitBase/ReadFileToString.h
        include/OrbitBase/SafeStrerror.h
        include/OrbitBase/ThreadConstants.h
        include/OrbitBase/ThreadPool.h
        include/OrbitBase/ThreadUtils.h
        include/OrbitBase/Tracing.h
        include/OrbitBase/UniqueResource.h)

target_sources(OrbitBase PRIVATE
        ExecutablePath.cpp
        ReadFileToString.cpp
        Logging.cpp
        SafeStrerror.cpp
        ThreadPool.cpp
        Tracing.cpp)

if (WIN32)
target_sources(OrbitBase PRIVATE
        ExecutablePathWindows.cpp
        ThreadUtilsWindows.cpp)
else()
target_sources(OrbitBase PRIVATE
        ExecutablePathLinux.cpp
        GetProcessIdsLinux.cpp
        ThreadUtilsLinux.cpp)
endif()

target_link_libraries(OrbitBase PUBLIC
        CONAN_PKG::Outcome
        CONAN_PKG::abseil
        std::filesystem)


add_executable(OrbitBaseTests)

target_compile_options(OrbitBaseTests PRIVATE ${STRICT_COMPILE_FLAGS})

target_sources(OrbitBaseTests PRIVATE
        ExecutablePathTest.cpp
        ReadFileToStringTest.cpp
        OrbitApiTest.cpp
        ProfilingTest.cpp
        ThreadUtilsTest.cpp
        TracingTest.cpp
        UniqueResourceTest.cpp
)

if (NOT WIN32)
target_sources(OrbitBaseTests PRIVATE
        ExecutablePathLinuxTest.cpp
        GetProcessIdsLinuxTest.cpp)
endif()

# Threadpool test contains some sleeps we couldn't work around - disable them on the CI
# since they are flaky there (but they work fine on local workstations).
if (NOT (WIN32 AND "$ENV{QT_QPA_PLATFORM}" STREQUAL "offscreen"))
target_sources(OrbitBaseTests PRIVATE
        ThreadPoolTest.cpp
)
endif()

target_link_libraries(OrbitBaseTests PRIVATE
        OrbitBase
        GTest::GTest
        GTest::Main)

add_custom_command(TARGET OrbitBaseTests POST_BUILD COMMAND ${CMAKE_COMMAND}
                   -E remove_directory $<TARGET_FILE_DIR:OrbitBaseTests>/testdata/OrbitBase)

add_custom_command(TARGET OrbitBaseTests POST_BUILD COMMAND ${CMAKE_COMMAND}
                   -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/testdata/
                   $<TARGET_FILE_DIR:OrbitBaseTests>/testdata/OrbitBase)

register_test(OrbitBaseTests)
