# Copyright (c) the JPEG XL Project Authors. All rights reserved.
#
# Use of this source code is governed by a BSD-style
# license that can be found in the LICENSE file.

# Preview tools, added with JPEGXL_ENABLE_TOOLS:
# - preview_bench and preview_bench_worker (JPEGXL_ENABLE_BENCHMARK): the
#   command-line benchmark of full against preview decodes. Installed.
# - preview_bench_gui (JPEGXL_ENABLE_BENCHMARK) and preview_demo_gui, with
#   JPEGXL_ENABLE_VIEWERS and Qt 6. Not installed.
# - preview_api_test (BUILD_TESTING): exercises lib/extras/preview.h. Not
#   installed. BUILD_TESTING also registers CTests of preview_bench's command
#   line and workers.

# Copying the DLLs that the executables need next to them lets them run from
# the build tree on Windows.
set(JXL_PREVIEW_COPY_RUNTIME_DLLS FALSE)
if(WIN32 AND NOT JPEGXL_STATIC)
  if(CMAKE_VERSION VERSION_LESS "3.21")
    message(STATUS
      "CMake ${CMAKE_VERSION} cannot copy runtime DLLs (TARGET_RUNTIME_DLLS "
      "needs 3.21): the preview tools may need PATH adjustments to run from "
      "the build tree.")
  else()
    set(JXL_PREVIEW_COPY_RUNTIME_DLLS TRUE)
  endif()
endif()

function(jxl_preview_copy_runtime_dlls target_name)
  if(NOT JXL_PREVIEW_COPY_RUNTIME_DLLS)
    return()
  endif()
  # A target without runtime DLLs (as in a static build) runs "cmake -E true":
  # "cmake -E copy_if_different" fails without files to copy.
  add_custom_command(TARGET ${target_name} POST_BUILD
    COMMAND "$<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:${target_name}>>,${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_RUNTIME_DLLS:${target_name}>;$<TARGET_FILE_DIR:${target_name}>,${CMAKE_COMMAND};-E;true>"
    COMMAND_EXPAND_LISTS
    VERBATIM
  )
endfunction()

# The settings tools/CMakeLists.txt gives to TOOL_BINARIES, and the runtime
# DLLs.
function(jxl_preview_executable target_name)
  target_include_directories(${target_name} PRIVATE "${PROJECT_SOURCE_DIR}")
  if(EMSCRIPTEN)
    set(link_flags "-s USE_LIBPNG=1 -s ALLOW_MEMORY_GROWTH=1")
    if(JPEGXL_ENABLE_WASM_THREADS)
      string(APPEND link_flags " -s USE_PTHREADS=1 -s PTHREAD_POOL_SIZE=16")
    endif()
    set_target_properties(${target_name} PROPERTIES LINK_FLAGS "${link_flags}")
  endif()
  # Attach the manifest that tells Windows to use UTF-8 for e.g. fopen.
  if(WIN32 AND NOT MINGW)
    target_sources(${target_name} PRIVATE
      "${PROJECT_SOURCE_DIR}/tools/utf8.manifest")
  elseif(WIN32)
    target_sources(${target_name} PRIVATE "${PROJECT_SOURCE_DIR}/tools/utf8.rc")
  endif()
  jxl_preview_copy_runtime_dlls(${target_name})
endfunction()

if(JPEGXL_ENABLE_BENCHMARK)
  add_library(preview_benchmark_core STATIC
    benchmark_core.cc
    benchmark_core.h
  )
  target_include_directories(preview_benchmark_core PUBLIC
    "${PROJECT_SOURCE_DIR}")
  # The static jxl-internal library (through jxl_extras-internal) provides the
  # decoder's preview hooks, which the shared library does not export.
  target_link_libraries(preview_benchmark_core PUBLIC
    jxl_extras-internal
    jxl_threads
    jxl_tool
  )
  if(WIN32)
    target_link_libraries(preview_benchmark_core PRIVATE Psapi)
  endif()

  add_executable(preview_bench_worker preview_bench_worker_main.cc)
  target_link_libraries(preview_bench_worker PRIVATE preview_benchmark_core)
  jxl_preview_executable(preview_bench_worker)

  add_executable(preview_bench preview_bench_main.cc)
  target_link_libraries(preview_bench PRIVATE preview_benchmark_core)
  jxl_preview_executable(preview_bench)
  # preview_bench runs preview_bench_worker from its own directory.
  add_dependencies(preview_bench preview_bench_worker)

  # Command-line tools.
  set_target_properties(preview_bench preview_bench_worker PROPERTIES
    MACOSX_BUNDLE OFF)
  install(TARGETS preview_bench preview_bench_worker
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()  # JPEGXL_ENABLE_BENCHMARK

if(JPEGXL_ENABLE_VIEWERS)
  find_package(Qt6 QUIET COMPONENTS Widgets)
  if(NOT Qt6_FOUND)
    message(STATUS
      "Qt6 was not found. preview_bench_gui and preview_demo_gui will not be "
      "built.")
  endif()
endif()

if(JPEGXL_ENABLE_VIEWERS AND Qt6_FOUND)
  set(JXL_PREVIEW_WINDEPLOYQT "")
  if(WIN32 AND TARGET Qt6::qmake)
    get_target_property(qmake_executable Qt6::qmake IMPORTED_LOCATION)
    if(qmake_executable)
      get_filename_component(qt_bin_dir "${qmake_executable}" DIRECTORY)
      if(EXISTS "${qt_bin_dir}/windeployqt.exe")
        set(JXL_PREVIEW_WINDEPLOYQT "${qt_bin_dir}/windeployqt.exe")
      else()
        message(WARNING
          "windeployqt.exe was not found in ${qt_bin_dir}. The preview GUIs "
          "may require Qt on PATH to run from the build tree.")
      endif()
    endif()
  endif()

  function(jxl_preview_gui target_name)
    set_target_properties(${target_name} PROPERTIES AUTOMOC ON AUTOUIC ON)
    target_include_directories(${target_name} PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}"
      "${CMAKE_CURRENT_BINARY_DIR}"
    )
    jxl_preview_executable(${target_name})
    if(JXL_PREVIEW_WINDEPLOYQT)
      add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND "${JXL_PREVIEW_WINDEPLOYQT}"
                --compiler-runtime
                --no-translations
                --no-opengl-sw
                "$<TARGET_FILE:${target_name}>"
        VERBATIM
      )
    endif()
  endfunction()

  if(JPEGXL_ENABLE_BENCHMARK)
    add_executable(preview_bench_gui WIN32
      main.cc
      preview_benchmark_window.cc
      preview_benchmark_window.h
      ../comparison_viewer/settings.cc
      ../comparison_viewer/settings.h
      ../comparison_viewer/settings.ui
      ../comparison_viewer/split_image_renderer.cc
      ../comparison_viewer/split_image_renderer.h
      ../comparison_viewer/split_image_view.cc
      ../comparison_viewer/split_image_view.h
      ../comparison_viewer/split_image_view.ui
    )
    target_include_directories(preview_bench_gui PRIVATE
      "${PROJECT_SOURCE_DIR}/tools/comparison_viewer")
    target_link_libraries(preview_bench_gui PRIVATE
      preview_benchmark_core
      Qt6::Widgets
    )
    jxl_preview_gui(preview_bench_gui)
    add_dependencies(preview_bench_gui preview_bench_worker)
  endif()

  # Interactive preview API explorer: single-file decode, placeholder
  # progression, and batch gallery.
  add_executable(preview_demo_gui WIN32
    preview_demo_main.cc
    preview_demo_window.cc
    preview_demo_window.h
  )
  target_link_libraries(preview_demo_gui PRIVATE
    jxl_extras-internal
    jxl_threads
    Qt6::Widgets
  )
  jxl_preview_gui(preview_demo_gui)
endif()  # JPEGXL_ENABLE_VIEWERS AND Qt6_FOUND

# Exerciser of the one-shot preview API (lib/extras/preview.h).
# Usage: preview_api_test <sdr.jxl> [<hdr.jxl>]
if(BUILD_TESTING)
  add_executable(preview_api_test preview_api_test.cc)
  target_link_libraries(preview_api_test PRIVATE
    jxl_extras-internal
    jxl_threads
  )
  jxl_preview_executable(preview_api_test)
  set_target_properties(preview_api_test PROPERTIES MACOSX_BUNDLE OFF)

  set(preview_api_test_inputs
    "${JPEGXL_TEST_DATA_PATH}/jxl/splines.jxl"
    "${JPEGXL_TEST_DATA_PATH}/jxl/pq_gradient.jxl"
  )
  set(preview_api_test_missing)
  foreach(input IN LISTS preview_api_test_inputs)
    if(NOT EXISTS "${input}")
      list(APPEND preview_api_test_missing "${input}")
    endif()
  endforeach()
  if(EMSCRIPTEN)
    # Reading the inputs needs the file system flags of the library tests.
    message(STATUS "preview_api_test is not registered with CTest for WASM.")
  elseif(preview_api_test_missing)
    message(STATUS
      "preview_api_test is not registered with CTest, missing test data: "
      "${preview_api_test_missing}")
  else()
    add_test(NAME preview_api_test
      COMMAND preview_api_test ${preview_api_test_inputs})
  endif()

  if(TARGET preview_bench AND NOT EMSCRIPTEN)
    # preview_bench rejects an invalid command line, with `message`, before it
    # looks at its input.
    function(jxl_preview_bench_reject_test name message)
      add_test(NAME preview_bench_rejects_${name}
        COMMAND preview_bench ${ARGN} input.jxl)
      set_tests_properties(preview_bench_rejects_${name} PROPERTIES
        PASS_REGULAR_EXPRESSION "${message}")
    endfunction()
    jxl_preview_bench_reject_test(invalid_factor
      "must be 1, 2, 4 or 8" --preview_downsampling=3)
    jxl_preview_bench_reject_test(negative_threads
      "Invalid value for --threads" --threads=-1)
    jxl_preview_bench_reject_test(empty_iterations
      "Invalid value for --iterations" --iterations=)
    jxl_preview_bench_reject_test(option_prefix
      "Unknown option: --threadsX" --threadsX=4)
    jxl_preview_bench_reject_test(nothing_to_do
      "needs --save-previews" --iterations=0)

    # A benchmark in preview_bench_worker processes, which parse the arguments
    # preview_bench passes them.
    set(preview_bench_input "${JPEGXL_TEST_DATA_PATH}/jxl/splines.jxl")
    if(EXISTS "${preview_bench_input}")
      add_test(NAME preview_bench_worker_run
        COMMAND preview_bench --iterations=1 "${preview_bench_input}")
    else()
      message(STATUS
        "preview_bench_worker_run is not registered with CTest, missing test "
        "data: ${preview_bench_input}")
    endif()
  endif()
endif()  # BUILD_TESTING
