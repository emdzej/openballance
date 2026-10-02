cmake_minimum_required(VERSION 3.24)
if(NOT CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Target architectures (use \"arm64;x86_64\" for universal)")
endif()
set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0 CACHE STRING "Minimum macOS version")
project(OpenBallance VERSION 0.1.2 LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(OB_WARN -Wall -Wextra -Wno-unused-parameter)

# The portable core: everything except host backends (no platform headers).
file(GLOB CORE_SRC src/*.c src/ck/*.c src/bb/*.c src/render/*.c src/phys/*.c)
set(HOST_SRC src/vfs_host.c)
foreach(f ${HOST_SRC} src/platform_gasm.c src/render/gpu_null.c)
  list(REMOVE_ITEM CORE_SRC "${CMAKE_SOURCE_DIR}/${f}")
endforeach()

# Platform: native (tools and headless tests, null GPU) or gasm (openballance.wasm, a gasm guest):
#   cmake -S . -B build-gasm -DOPENBALLANCE_PLATFORM=gasm -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_TOOLCHAIN_FILE=<gasm>/sdk/c/cmake/gasm-toolchain.cmake [-DWASI_SDK_PREFIX=<wasi-sdk>]
# (tools/fetch-gasm-sdk.sh fetches the released SDK and wasi-sdk into .deps/)
set(OPENBALLANCE_PLATFORM native CACHE STRING "Platform: native or gasm")
if(OPENBALLANCE_PLATFORM STREQUAL "gasm")
  include(Gasm)
  gasm_add_game(openballance ${CORE_SRC} src/platform_gasm.c)
  target_include_directories(openballance PRIVATE src)
  target_compile_options(openballance PRIVATE ${OB_WARN} -O2)
  # Level loading keeps a few large buffers on the stack: 1 MiB instead of 64 KiB.
  target_link_options(openballance PRIVATE -Wl,-z,stack-size=1048576 -Wl,--strip-debug)
  return()
endif()

add_library(ob_core STATIC ${CORE_SRC} src/render/gpu_null.c)
target_include_directories(ob_core PUBLIC src)
target_compile_options(ob_core PRIVATE ${OB_WARN})
target_link_libraries(ob_core PUBLIC m)

# Tests read the game data through the file layer (./data, or OPENBALLANCE_DATA=<dir|image>).
enable_testing()
foreach(t ck_test run_test cab_test)
  add_executable(${t} tests/${t}.c ${HOST_SRC})
  target_link_libraries(${t} PRIVATE ob_core)
  target_compile_options(${t} PRIVATE ${OB_WARN})
  add_test(NAME ${t} COMMAND ${t} WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endforeach()
