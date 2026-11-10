# Flows Embed (embed/): the compiler and DSP engine as one single-threaded,
# runtime-free WebAssembly module. Unlike the browser app it uses no pthreads
# or shared memory, so pages that embed it need no cross-origin isolation.
#
# The browser app compiles every object with -pthread (see the top-level
# CMakeLists). Objects with atomics cannot link into a module without shared
# memory, so the engine and compiler are compiled again here with the
# directory's thread flags replaced.
set(FLOWS_EMBED_COMPILE_OPTIONS -fwasm-exceptions -msimd128 -Wall -Wextra -Wpedantic -fno-fast-math)

# Size matters for a page download: the compiler and host are optimised for
# size; the DSP engine keeps -O3 (the release default) for render speed. This
# saves about a quarter of the download at no measurable render cost.
add_library(flows_embed_dsp STATIC src/engine/engine.cpp)
add_library(flows_embed_compiler STATIC src/compiler/program.cpp src/engine/sample.cpp)
target_include_directories(flows_embed_dsp PUBLIC src)
target_include_directories(flows_embed_compiler PUBLIC src)

add_executable(flows_embed embed/native/EmbedHost.cpp)
target_link_libraries(flows_embed PRIVATE flows_embed_compiler flows_embed_dsp)
set_target_properties(flows_embed_dsp PROPERTIES COMPILE_OPTIONS "${FLOWS_EMBED_COMPILE_OPTIONS}")
set_target_properties(flows_embed_compiler flows_embed PROPERTIES COMPILE_OPTIONS "${FLOWS_EMBED_COMPILE_OPTIONS};-Oz")
# A .wasm output is a standalone module: its only imports are a few WASI
# calls that embed/src/wasm.ts provides. No Emscripten JavaScript runtime.
set_target_properties(flows_embed PROPERTIES OUTPUT_NAME flows-embed SUFFIX ".wasm"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/embed"
    LINK_OPTIONS "-Oz;-fwasm-exceptions;--no-entry;-sSTANDALONE_WASM=1;-sFILESYSTEM=0;-sALLOW_MEMORY_GROWTH=1;-sINITIAL_MEMORY=4MB;-sSTACK_SIZE=256KB;-sERROR_ON_UNDEFINED_SYMBOLS=1")
