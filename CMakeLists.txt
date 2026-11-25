cmake_minimum_required(VERSION 3.20)
project(picoflate
  VERSION 0.1.0
  DESCRIPTION "Sub-microsecond zero-allocation C++23/26 RFC 1951 raw-deflate decompressor for small-packet streaming"
  LANGUAGES CXX)

include(GNUInstallDirs)

option(PICOFLATE_BUILD_TESTS "Build picoflate RFC 1951 ASan/UBSan & differential fuzz tests" OFF)
option(PICOFLATE_BUILD_BENCH "Build picoflate microbenchmarks" OFF)
option(PICOFLATE_USE_CXX26   "Compile tests/benchmarks with C++26 instead of C++23" OFF)

add_library(picoflate INTERFACE)
add_library(picoflate::picoflate ALIAS picoflate)
target_include_directories(picoflate INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(picoflate INTERFACE cxx_std_23)

install(DIRECTORY include/picoflate DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS picoflate EXPORT picoflateTargets)
install(EXPORT picoflateTargets
  FILE picoflateConfig.cmake
  NAMESPACE picoflate::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/picoflate)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
  set(PICOFLATE_BUILD_TESTS ON)
  set(PICOFLATE_BUILD_BENCH ON)

  if(PICOFLATE_USE_CXX26)
    set(PICOFLATE_STD cxx_std_26)
  else()
    set(PICOFLATE_STD cxx_std_23)
  endif()

  if(NOT DEFINED BUILD_TARGET_ARCH)
    if(APPLE)
      set(PICOFLATE_ARCH_FLAG "-mcpu=native")
    else()
      set(PICOFLATE_ARCH_FLAG "-march=native")
    endif()
  else()
    set(PICOFLATE_ARCH_FLAG "-march=${BUILD_TARGET_ARCH}")
  endif()

  find_package(ZLIB REQUIRED)
  find_package(libdeflate CONFIG QUIET)

  if(PICOFLATE_BUILD_TESTS)
    enable_testing()
    add_executable(picoflate_tests tests/picoflate_test.cc)
    target_link_libraries(picoflate_tests PRIVATE picoflate ZLIB::ZLIB)
    target_compile_features(picoflate_tests PRIVATE ${PICOFLATE_STD})
    target_compile_options(picoflate_tests PRIVATE
      -Wall -Wextra -Wpedantic -Werror ${PICOFLATE_ARCH_FLAG})
    add_test(NAME picoflate_tests COMMAND picoflate_tests)
  endif()

  if(PICOFLATE_BUILD_BENCH)
    add_executable(picoflate_bench bench/picoflate_bench.cc)
    target_link_libraries(picoflate_bench PRIVATE picoflate ZLIB::ZLIB)
    target_compile_features(picoflate_bench PRIVATE ${PICOFLATE_STD})
    target_compile_options(picoflate_bench PRIVATE
      -O3 -Wall -Wextra -Wpedantic -Werror ${PICOFLATE_ARCH_FLAG})
    if(TARGET libdeflate::libdeflate_static)
      target_link_libraries(picoflate_bench PRIVATE libdeflate::libdeflate_static)
      target_compile_definitions(picoflate_bench PRIVATE PICOFLATE_HAS_LIBDEFLATE=1)
    elseif(TARGET libdeflate::libdeflate_shared)
      target_link_libraries(picoflate_bench PRIVATE libdeflate::libdeflate_shared)
      target_compile_definitions(picoflate_bench PRIVATE PICOFLATE_HAS_LIBDEFLATE=1)
    elseif(TARGET libdeflate::libdeflate)
      target_link_libraries(picoflate_bench PRIVATE libdeflate::libdeflate)
      target_compile_definitions(picoflate_bench PRIVATE PICOFLATE_HAS_LIBDEFLATE=1)
    endif()
  endif()
endif()
