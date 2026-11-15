cmake_minimum_required(VERSION 3.16)
project(myelin_native C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(NULLFRAME_ENGINE_SOURCES
    src/main/c/surface_generation.c
    src/main/c/terrain_sampling.c
    src/main/c/terrain_noise_cache.c
    src/main/c/vegetation.c
    src/main/c/tree_shapes.c
)

find_package(JNI REQUIRED)

# Collect all upstream Cubiomes .c files so they build into our shared library.
file(GLOB CUBIOMES_SOURCES "src/main/c/cubiomes/*.c")
list(REMOVE_ITEM CUBIOMES_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main/c/cubiomes/tests.c")

add_library(cubiomes_bridge SHARED
    src/main/c/cubiomes_bridge.c
    ${NULLFRAME_ENGINE_SOURCES}
    src/main/c/quad_baker.c
    src/main/c/avx512_kernels.c
    ${CUBIOMES_SOURCES}
)

target_include_directories(cubiomes_bridge PRIVATE
    ${JNI_INCLUDE_DIRS}
    src/main/c
    src/main/c/cubiomes
)

# ── AVX2 (Phase 1): global SIMD flags. Requires an AVX2-capable CPU (all
# 2013+ x86 chips). MSVC: /arch:AVX2 (implies FMA). GCC/Clang: -mavx2 -mfma.
# cubiomes_bridge.c detects these at compile time and enables the hand-written
# __m256 bilerp fill; the rest of the library auto-vectorizes.
# NOTE: this root CMakeLists drives the Windows build and defines the Windows
# benchmark targets; src/main/c/CMakeLists.txt is used for the WSL native build.
if(MSVC)
    target_compile_options(cubiomes_bridge PRIVATE /arch:AVX2 /O2)
else()
    target_compile_options(cubiomes_bridge PRIVATE -O3 -mavx2 -mfma)
endif()

# ── AVX-512 (optional, runtime dispatched) ──
# avx512_kernels.c is the ONLY file compiled with AVX-512 codegen. Its kernels
# never execute unless om_cpu_has_avx512() passes at runtime (CPUID + XCR0
# opmask/ZMM state check); everything else stays AVX2, so the same binary
# runs on any AVX2 CPU. Per-file flags override the target's /arch:AVX2.
set(OM_AVX512_SRC src/main/c/avx512_kernels.c)
if(MSVC)
    set_source_files_properties(${OM_AVX512_SRC} PROPERTIES COMPILE_OPTIONS "/arch:AVX512")
else()
    set_source_files_properties(${OM_AVX512_SRC} PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512dq")
endif()

if(WIN32)
    set_target_properties(cubiomes_bridge PROPERTIES OUTPUT_NAME "cubiomes_bridge")
else()
    set_target_properties(cubiomes_bridge PROPERTIES OUTPUT_NAME "cubiomes_bridge")
endif()

set(NATIVE_RES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/main/resources/native")
add_custom_command(TARGET cubiomes_bridge POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${NATIVE_RES_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:cubiomes_bridge>"
        "${NATIVE_RES_DIR}/$<TARGET_FILE_NAME:cubiomes_bridge>"
    COMMENT "Copying native library to src/main/resources/native/"
)

# Benchmarks use Windows threading and UI APIs.
if(WIN32)
# ── C-only benchmark programs ──
add_executable(cubiomes_bench benchmarks/bench.c src/main/c/quad_baker.c src/main/c/avx512_kernels.c ${NULLFRAME_ENGINE_SOURCES} ${CUBIOMES_SOURCES})
target_include_directories(cubiomes_bench PRIVATE
    ${JNI_INCLUDE_DIRS}
    src/main/c
    src/main/c/cubiomes
)
if(MSVC)
    target_compile_options(cubiomes_bench PRIVATE /arch:AVX2 /O2)
else()
    target_compile_options(cubiomes_bench PRIVATE -O3 -mavx2 -mfma)
endif()
# Bench-only: measure terrain WITHOUT the staircase shaping post-pass
# (the game DLL always keeps it). Guarded in cubiomes_bridge.c + bench.c.
target_compile_definitions(cubiomes_bench PRIVATE MYELIN_BENCH_NO_STAIRCASE)

# Sweep.exe — worker-count (1-10) × sampling-step scaling benchmark.
# Same sources/flags as cubiomes_bench; runs ~20-30 min, user-driven.
add_executable(sweep benchmarks/sweep.c src/main/c/quad_baker.c src/main/c/avx512_kernels.c ${NULLFRAME_ENGINE_SOURCES} ${CUBIOMES_SOURCES})
target_include_directories(sweep PRIVATE
    ${JNI_INCLUDE_DIRS}
    src/main/c
    src/main/c/cubiomes
)
if(MSVC)
    target_compile_options(sweep PRIVATE /arch:AVX2 /O2)
else()
    target_compile_options(sweep PRIVATE -O3 -mavx2 -mfma)
endif()
target_compile_definitions(sweep PRIVATE MYELIN_BENCH_NO_STAIRCASE)

# Billion_Chunks_Benchmark.exe — every sampling step (1:1..1:128) runs a full
# 1,000,000,000 chunks on 10 pinned workers. Same internals as cubiomes_bench
# (direct cubiomes_bridge.c include, pure-C path, no JNI). Live progress +
# summary table; pauses on Enter at exit so double-click windows stay open.
add_executable(Billion_Chunks_Benchmark benchmarks/Billion_Chunks_Benchmark.c src/main/c/quad_baker.c src/main/c/avx512_kernels.c ${NULLFRAME_ENGINE_SOURCES} ${CUBIOMES_SOURCES})
target_include_directories(Billion_Chunks_Benchmark PRIVATE
    ${JNI_INCLUDE_DIRS}
    src/main/c
    src/main/c/cubiomes
)
if(MSVC)
    target_compile_options(Billion_Chunks_Benchmark PRIVATE /arch:AVX2 /O2)
else()
    target_compile_options(Billion_Chunks_Benchmark PRIVATE -O3 -mavx2 -mfma)
endif()
target_compile_definitions(Billion_Chunks_Benchmark PRIVATE MYELIN_BENCH_NO_STAIRCASE)

# Billion_Neon_Bench.exe — 1:128 ONLY, 1B chunks, 10 pinned workers. Same
# internals as Billion_Chunks_Benchmark but with an animated ANSI UI: stat
# box, in-place progress bar, live box-drawing line graph (UTF-8), finale
# sparkline. Green/white palette, yellow alarm color ("Only AVX2" tag).
add_executable(Billion_Neon_Bench benchmarks/Billion_Neon_Bench.c src/main/c/quad_baker.c src/main/c/avx512_kernels.c ${NULLFRAME_ENGINE_SOURCES} ${CUBIOMES_SOURCES})
target_include_directories(Billion_Neon_Bench PRIVATE
    ${JNI_INCLUDE_DIRS}
    src/main/c
    src/main/c/cubiomes
)
if(MSVC)
    target_compile_options(Billion_Neon_Bench PRIVATE /arch:AVX2 /O2)
else()
    target_compile_options(Billion_Neon_Bench PRIVATE -O3 -mavx2 -mfma)
endif()
target_compile_definitions(Billion_Neon_Bench PRIVATE MYELIN_BENCH_NO_STAIRCASE)


endif()
