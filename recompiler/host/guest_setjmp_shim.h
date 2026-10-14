// setjmp.h shim for guests compiled by the in-browser clang (pipeline_graphics.js
// mounts this as /project/shim/setjmp.h ahead of the WASIX sysroot).
//
// Two reasons it exists:
//   * Layout. Runtime structs the GUEST allocates on its stack embed jmp_buf
//     (avm2/avm2_error.h Avm2TryFrame), and the HOST writes/reads their fields
//     at Emscripten's offsets: its wasm-sjlj jmp_buf is 156 bytes, the WASIX
//     one a handful. Without this shim every try-bearing AVM2 method body
//     hands the host a too-small frame and gets its locals smashed
//     (assessment doc §3). guest_loader.js checks the two sizes agree.
//   * Linking. The in-browser clang cannot lower setjmp and the WASIX libc
//     version (stack_checkpoint syscalls) must not be linked; declaring it as a
//     plain function leaves it undefined, so it becomes a guest import the
//     loader stubs (try body runs as if nothing throws — the interim both AVM1
//     and AVM2 try/catch live with until the runtime-helper emission mode).
#pragma once
#define SWF_GUEST_JMP_BUF_SIZE 156
typedef struct { unsigned char __opaque[SWF_GUEST_JMP_BUF_SIZE]; } jmp_buf[1];
typedef jmp_buf sigjmp_buf;
// Renamed: a plain `setjmp` would still resolve to the WASIX libc's definition
// at link time (the driver links libc by default); this name is undefined
// there, so it is imported and guest_loader.js stubs it.
int __swf_guest_setjmp(jmp_buf env);
#define setjmp(env) __swf_guest_setjmp(env)
#define _setjmp(env) __swf_guest_setjmp(env)
void longjmp(jmp_buf env, int val);
#define _longjmp longjmp
