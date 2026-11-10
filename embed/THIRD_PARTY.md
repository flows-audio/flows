# Third-party notices

`@flows-audio/embed` is licensed under Apache-2.0 (see `LICENSE`). It contains
the Flows compiler (`src/compiler`) and DSP engine (`src/engine`), both
Apache-2.0, compiled to WebAssembly with Emscripten 6.0.13.

`flows-embed.wasm` includes parts of Emscripten's runtime and system
libraries, which keep their own licenses. Their texts are in `dist/licenses/`:

| Component | License | File |
| --- | --- | --- |
| Emscripten runtime | MIT / University of Illinois NCSA | `emscripten-LICENSE.txt` |
| musl libc | MIT | `musl-COPYRIGHT.txt` |
| LLVM libc++ | Apache-2.0 WITH LLVM-exception | `libcxx-LICENSE.txt` |
| LLVM libc++abi | Apache-2.0 WITH LLVM-exception | `libcxxabi-LICENSE.txt` |
| LLVM libunwind | Apache-2.0 WITH LLVM-exception | `libunwind-LICENSE.txt` |
| LLVM compiler-rt | Apache-2.0 WITH LLVM-exception | `compiler-rt-LICENSE.txt` |

The JavaScript has no runtime dependencies.
