# Building bpftime, and the API for the second interception backend

Two path placeholders are used throughout: `<bpftime>` is the bpftime source
checkout and `<bpftime>/build` is the build directory configured inside it.
Substitute your own paths. Every code block, function signature, and command
line below is copied character for character from the source or from a run,
without rewriting or simplification.

## 1. Summary of conclusions

- bpftime **builds successfully** with the options below: configure exit code 0, build exit code 0, all 140 build steps completed.
- Key preconditions: LLVM JIT must be turned off (`-DBPFTIME_LLVM_JIT=0`), the daemon must be turned off (`-DBUILD_BPFTIME_DAEMON=0`), libbpf must be kept (`-DBPFTIME_BUILD_WITH_LIBBPF=1`), and the targets to build must be named explicitly to ninja (a whole-project build does not work, because it tries to build bpftool and fails).
- The artifacts are under `<bpftime>/build/`; the full list is in section 2.
- **Route A is the route this repository takes for the second backend**: our own `.so` does a `dlopen` of the zpoline transformer in its constructor, calls `setup_syscall_tracer()`, and installs a hook shaped as a plain C function pointer via `set_call_hook()`. This route was run end to end: it intercepted syscalls issued directly via inline assembly and also syscalls going through libc; returning directly from the hook (without calling the original syscall function) is enough to "skip the original syscall and return your own value."
- Route A lets `src/bpftime_hook.c` stay a `.c` file (the hook function type is a plain C function pointer), and that is what it is. On route B (using bpftime's attach layer) the callback type is `std::function`, so that file would have to become `.cpp` or gain a C++ wrapper layer.

## 2. Build results

**Toolchain**

- Kernel `6.18.0-rc5`.
- The compilers actually used are `/usr/bin/cc` and `/usr/bin/c++`, i.e. gcc/g++. In CMakeCache.txt: `CMAKE_C_COMPILER:FILEPATH=/usr/bin/cc`, `CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/c++`.
- The only clang present was a self-built `clang version 22.0.0git` from a ClangIR branch, not a distribution clang.
- cmake 4.2.3, ninja available, ccache installed at `/usr/bin/ccache`, Boost 1.90.0.

**Source**

- Repository `<bpftime>`, a full clone from https://github.com/eunomia-bpf/bpftime.git.
- commit `2a459363243df882bf579e9a40dc06c34064ef28`, commit title `ci: fetch release tags without submodules`.
- `git submodule update --init --recursive` succeeded, exit code 0, all submodules fetched (including bpftool, spdlog, ebpf-verifier, argparse, Catch2, ubpf, llvmbpf, and their nested submodules).

**cmake options**

All options are defined in `cmake/StandardSettings.cmake`. The key options and their default values, copied verbatim:

- `option(BPFTIME_LLVM_JIT "Use LLVM as jit backend." ON)` — line 10
- `option(BPFTIME_UBPF_JIT "Use uBPF as jit backend." ON)` — line 11
- `option(BPFTIME_ENABLE_UNIT_TESTING "Enable unit tests for the projects (from the `test` subfolder)." OFF)` — line 32
- `option(BUILD_BPFTIME_DAEMON "Whether to build the bpftime daemon" ON)` — line 90
- `option(BPFTIME_BUILD_WITH_LIBBPF "Whether to build with libbpf and other linux headers" ON)` — line 99
- `option(ENABLE_EBPF_VERIFIER "Whether to enable ebpf verifier" OFF)` — line 87

The actual option name for turning off LLVM JIT is `BPFTIME_LLVM_JIT`. Configuration prints `-- Supporting ubpf-jit`, confirming that the ubpf interpreter/JIT is used rather than LLVM.

**Configuration command finally adopted**, copied character for character:

```
cd <bpftime>
cmake -Bbuild -DCMAKE_BUILD_TYPE=Release \
  -DBPFTIME_ENABLE_UNIT_TESTING=0 \
  -DBPFTIME_LLVM_JIT=0 \
  -DBPFTIME_UBPF_JIT=1 \
  -DBPFTIME_BUILD_WITH_LIBBPF=1 \
  -DBUILD_BPFTIME_DAEMON=0 \
  -GNinja
```

**Build command**, copied character for character — the targets must be named explicitly; a whole-project build without `--target` does not work:

```
cmake --build build -j64 --target bpftime-agent bpftime_text_segment_transformer bpftime-syscall-server bpftime-cli-cpp bpftimetool
```

Configure exit code 0, build exit code 0, all 140 build steps completed.

**Build time**

- A from-scratch configure-plus-build was run with ccache disabled (`CCACHE_DISABLE=1`) at concurrency `-j64`.
- With ccache hits, the rebuild is faster.
- Fetching the submodules took additional wall-clock time and was not separately timed.

Note: this project compiles quickly once LLVM JIT is turned off. LLVM JIT is the expensive part of a default build, and `-DBPFTIME_LLVM_JIT=0` removes it.

**Paths of the artifacts**

```
<bpftime>/build/runtime/agent/libbpftime-agent.so                                        27761008 bytes
<bpftime>/build/runtime/syscall-server/libbpftime-syscall-server.so                       3706320 bytes
<bpftime>/build/attach/text_segment_transformer/libbpftime-agent-transformer.so          16283416 bytes
<bpftime>/build/tools/cli/bpftime                                                        99955232 bytes
<bpftime>/build/tools/bpftimetool/bpftimetool                                             3739168 bytes
<bpftime>/build/attach/syscall_trace_attach_impl/libbpftime_syscall_trace_attach_impl.a    167146 bytes
<bpftime>/build/runtime/libruntime.a                                                      4905244 bytes
<bpftime>/build/libbpf/libbpf.a                                                           3495934 bytes
<bpftime>/build/third_party/spdlog/libspdlog.a                                            1314378 bytes
<bpftime>/build/FridaGum-prefix/src/FridaGum/libfrida-gum.a                              89144322 bytes
```

Two more are generated at build time and needed later for linking:

```
<bpftime>/build/attach/syscall_trace_attach_impl/syscall_id_list.h   (header file of the syscall number table, generated at build time)
<bpftime>/build/FridaGum-prefix/src/FridaGum/frida-gum.h             (Frida Gum devkit header file)
```

The `bpftime` command-line tool runs; the subcommands printed by `--help` are `{attach,detach,load,start,trace}`.

## 3. Two build failures encountered, and how they were worked around

**Failure 1**: configuring with LLVM JIT, libbpf, and the daemon left at their defaults (`cmake -Bbuild -DCMAKE_BUILD_TYPE=Release -DBPFTIME_ENABLE_UNIT_TESTING=0`) followed by `cmake --build build -j64` gives exit code 1, with two errors.

The first is a bpftool build failure. ninja reports `FAILED: [code=2] bpftool/src/bpftool-stamp/bpftool-build`; the root cause is that bpftool's bundled libbpf treats warnings as errors under gcc 15:

```
libbpf.c:8207:13: error: assignment discards ‘const’ qualifier from pointer target type [-Werror=discarded-qualifiers]
libbpf.c:11515:31: error: assignment discards ‘const’ qualifier from pointer target type [-Werror=discarded-qualifiers]
libbpf.c:12103:35: error: assignment discards ‘const’ qualifier from pointer target type [-Werror=discarded-qualifiers]
```

The second is a clang crash while compiling the eBPF program in the daemon. ninja reports `FAILED: [code=1] daemon/bpf_tracer.bpf.o`; clang hits an assertion failure:

```
clang: /home/<user>/clangir/llvm/lib/IR/Value.cpp:1147: void llvm::ValueHandleBase::AddToExistingUseList(ValueHandleBase **): Assertion `getValPtr() == Next->getValPtr() && "Added to wrong list?"' failed.
clang: error: clang frontend command failed with exit code 134
```

The crash is in the `Target: bpf` pass, i.e. `clang -target bpf` compiling eBPF bytecode. The clang used is the self-built ClangIR-branch clang (`Build config: +unoptimized, +assertions`). **No distribution clang was available to compile eBPF programs**, which directly affects route C below.

**Workaround**:

1. `-DBUILD_BPFTIME_DAEMON=0` keeps the daemon out of the build graph, so `clang -target bpf` is no longer triggered.
2. bpftool is added unconditionally in `cmake/libbpf.cmake` via `ExternalProject_Add(bpftool ...)` and belongs to the ALL target by default, so it cannot be turned off with a switch. The way around it is to **name the wanted targets explicitly to ninja**; bpftool is not a dependency of those targets, so it is never built. This is also why the `--target ...` command above is required.
3. The libbpf that bpftime itself uses (`third_party/libbpf` is a symlink to `./bpftool/libbpf/`) builds successfully, and the artifact `<bpftime>/build/libbpf/libbpf.a` exists. Only the bpftool command-line tool itself fails.

### A trap worth recording: libbpf cannot be turned off

At one point `-DBPFTIME_BUILD_WITH_LIBBPF=0` was tried; the build gave exit code 0 and all artifacts appeared, but the agent built that way **does not include syscall tracing**. The reason is in `runtime/agent/agent.cpp` line 44:

```cpp
#if __linux__ && BPFTIME_BUILD_WITH_LIBBPF
#include "syscall_trace_attach_impl.hpp"
#include "syscall_trace_attach_private_data.hpp"
#endif
```

The same macro also guards the syscall trace attach implementation registration at lines 951–976, the `set_to_global()` at lines 1020–1022, and the two functions `syscall_callback` and `_bpftime__setup_syscall_trace_callback` at lines 1143–1175.

Symbol comparison between the two builds:

- For the `libbpftime-agent.so` built with `BPFTIME_BUILD_WITH_LIBBPF=0`, `nm -D --defined-only` shows only the three symbols `bpftime_agent_control`, `bpftime_agent_main`, `bpftime_hooked_main`, and **not** `_bpftime__setup_syscall_trace_callback`.
- The one built with `BPFTIME_BUILD_WITH_LIBBPF=1` additionally has `_bpftime__setup_syscall_trace_callback` (address `000000000027d9c0`), `syscall_callback`, and `injected_with_frida`.

And `_bpftime__setup_syscall_trace_callback` is exactly the symbol the zpoline transformer looks for after opening the agent with dlmopen (`attach/text_segment_transformer/agent-transformer.cpp` lines 89–96). Without it, the transformer prints `Malformed agent so, expected symbol _bpftime__setup_syscall_hooker_callback` and returns immediately, and the whole zpoline route is dead.

**Conclusion: the zpoline route used here requires `-DBPFTIME_BUILD_WITH_LIBBPF=1`.**

## 4. The exact API the second backend must call

First, rule out an entry point that is easy to get wrong. `runtime/syscall-server/syscall_context.hpp` is often read as the interface for syscall hooks, but **it is not**. The `class syscall_context` in this header is bpftime's "syscall server": its job is to pretend, in user space, to implement syscalls such as `bpf()` and `perf_event_open()` so that libbpf believes it is talking to the kernel. Its member functions are of the kind `handle_sysbpf`, `handle_perfevent`, `handle_mmap64`, `handle_openat`, all obtained by intercepting libc wrapper functions via LD_PRELOAD. What it solves is "how to load an eBPF program into the user-space runtime," not "how to intercept the target program's syscalls."

The real syscall hook interface is under the two directories `attach/syscall_trace_attach_impl/` and `attach/text_segment_transformer/`.

Terminology: **zpoline** is a technique for intercepting syscalls in user space — it rewrites the `syscall` instruction (the two machine-code bytes `0f 05`) in the process's executable segments in place into a `call` instruction, so it jumps into your own hook instead of trapping into the kernel. The "transformer" below refers to the component in bpftime that implements this rewriting (the artifact `libbpftime-agent-transformer.so`).

### 4.1 Route A: use only the zpoline transformer + your own hook function (the route taken here)

Header: `<bpftime>/attach/text_segment_transformer/text_segment_transformer.hpp`, in full below, copied character for character:

```cpp
#ifndef _TEST_SEGMENT_TRANSFORMER_H
#define _TEST_SEGMENT_TRANSFORMER_H
// C++ standard library could not be found by clangd on my machine.. Will fix in
// a later time
#include <cinttypes>
using syscall_hooker_func_t = int64_t (*)(int64_t sys_nr, int64_t arg1,
					  int64_t arg2, int64_t arg3,
					  int64_t arg4, int64_t arg5,
					  int64_t arg6);
namespace bpftime
{
	// Setup userspace syscall trace
void setup_syscall_tracer();
// Get current callback function when a syscall was invoked. Default to be a function that directly calls the syscall
syscall_hooker_func_t get_call_hook();
// Set the syscall callback function
void set_call_hook(syscall_hooker_func_t hook);
} // namespace bpftime

#endif
```

Key point: the hook function type `syscall_hooker_func_t` is a **plain C ABI function pointer**, with seven `int64_t` parameters (the syscall number + six arguments) and an `int64_t` return. No `std::function`, no C++ object, so the hook function itself can be written in C.

The exported symbol names of these three functions in the built `.so`, i.e. the C++-mangled names as reported by `nm -D`:

```
_ZN7bpftime20setup_syscall_tracerEv          -> bpftime::setup_syscall_tracer()
_ZN7bpftime13get_call_hookEv                 -> bpftime::get_call_hook()
_ZN7bpftime13set_call_hookEPFllllllllE       -> bpftime::set_call_hook(syscall_hooker_func_t)
```

There is also a symbol that must be called first: `_frida_cs_arch_register_x86`. `setup_syscall_tracer()` internally uses Capstone disassembly to find `syscall` instructions, and the Capstone bundled by Frida requires the architecture to be registered first. **Without calling it first, `setup_syscall_tracer()` reports `Failed to open capstone instance: 2, Invalid/unsupported architecture(CS_ERR_ARCH)` and then `exit(1)`.** Note the `_frida_` prefix in the name: `frida-gum.h` line 22 has `#define cs_arch_register_x86 _frida_cs_arch_register_x86`, so `dlsym` on `cs_arch_register_x86` returns NULL and only `dlsym` on `_frida_cs_arch_register_x86` resolves.

This route was run end to end. The full runnable call sequence:

```cpp
#include <dlfcn.h>
using hook_t = int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

void *h = dlopen("<bpftime>/build/attach/text_segment_transformer/libbpftime-agent-transformer.so",
                 RTLD_NOW | RTLD_GLOBAL);
auto csreg = (void (*)())dlsym(h, "_frida_cs_arch_register_x86");
auto setup = (void (*)())dlsym(h, "_ZN7bpftime20setup_syscall_tracerEv");
auto getk  = (hook_t (*)())dlsym(h, "_ZN7bpftime13get_call_hookEv");
auto setk  = (void (*)(hook_t))dlsym(h, "_ZN7bpftime13set_call_hookEPFllllllllE");

csreg();          // must be before setup
setup();          // map page zero + rewrite the syscall instructions in all executable segments
hook_t orig = getk();   // save the function that "directly executes the original syscall"
setk(my_hook);          // install your own hook
```

The hook function expresses two outcomes:

```cpp
static int64_t my_hook(int64_t nr, int64_t a1, int64_t a2, int64_t a3,
                       int64_t a4, int64_t a5, int64_t a6)
{
    if (/* EXITOS_TAKEOVER */)
        return my_own_return_value;          // skip the original syscall, return your own value
    return orig(nr, a1, a2, a3, a4, a5, a6); // EXITOS_PASS: execute the original syscall as usual
}
```

In other words, on this route, "skip the original syscall and give my own return value" is simply **returning directly from the hook function without calling `orig`**. There is no other mechanism, and none is needed. A negative return value denotes errno (the kernel's syscall return convention), e.g. `return -EACCES;`.

Output of the test program:

```
real getpid() before tracer = 401254
[info] [text_segment_transformer.cpp:246] Page zero setted up..
[info] [text_segment_transformer.cpp:274] Rewriting executable segments..
RAW-ASM getpid after tracer  = 123456  (expect 123456 if intercepted)
hook saw 2 syscalls total, 1 of them write
libc-write-after-tracer
```

What this output means: the test program uses inline assembly `__asm__ volatile("syscall" : "=a"(ret) : "a"(39) ...)` to issue `getpid` (syscall number 39) directly, entirely bypassing libc; the hook caught it and returned its own `123456`, and the real `getpid` was not executed. In the same run, libc's `write(2, ...)` (syscall number 1) was also caught. **This is the capability the second backend needs: writes that bypass libc are still caught.**

How to compile and link: the `dlopen` route needs only `-ldl`, without linking any of bpftime's static libraries and without the Frida headers. An alternative is to compile `attach/text_segment_transformer/text_segment_transformer.cpp` directly into your own object files, which requires:

```
-I<FRIDA_GUM_DIR> -I<bpftime>/third_party/spdlog/include -I<bpftime>/attach/text_segment_transformer
<FRIDA_GUM_DIR>/libfrida-gum.a <bpftime>/build/third_party/spdlog/libspdlog.a -lpthread -ldl -lresolv -lm
```

where `<FRIDA_GUM_DIR>` = `<bpftime>/build/FridaGum-prefix/src/FridaGum`. This alternative was also compiled and run end to end.

What `setup_syscall_tracer()` does (`attach/text_segment_transformer/text_segment_transformer.cpp`, around lines 165–200): map one page at address `0x0` with `MAP_FIXED`, fill the first 512 bytes with `0x90` (nop), then place a sequence `push %rax; movabs $syscall_hooker_asm, %rax; jmp *%rax`, and then rewrite in place every `syscall` instruction (the two bytes `0f 05`) in all executable segments listed in `/proc/self/maps` into `ff d0` (`call *%rax`). This is the standard zpoline technique. It is implemented only under `__x86_64__`; the `__aarch64__` branch is empty (the source says `// TODO: implement syscall trace trampoline`).

### 4.2 Route B: use bpftime's syscall trace attach implementation (register a native C++ callback)

Use this layer if you need to dispatch by syscall number, need to attach at the enter and exit points separately, or need multiple callbacks to coexist. Header: `<bpftime>/attach/syscall_trace_attach_impl/include/syscall_trace_attach_impl.hpp`.

The key types and functions, copied character for character:

```cpp
namespace bpftime { namespace attach {

// Represent the syscall hooker function
using syscall_hooker_func_t = int64_t (*)(int64_t sys_nr, int64_t arg1,
					  int64_t arg2, int64_t arg3,
					  int64_t arg4, int64_t arg5,
					  int64_t arg6);

// Attach type id of syscall trace
constexpr size_t ATTACH_SYSCALL_TRACE = 2;

class syscall_trace_attach_impl final : public base_attach_impl {
    public:
	// Dispatch a syscall from text transformer
	int64_t dispatch_syscall(int64_t sys_nr, int64_t arg1, int64_t arg2,
				 int64_t arg3, int64_t arg4, int64_t arg5,
				 int64_t arg6);
	// Set the function of calling original syscall
	void set_original_syscall_function(syscall_hooker_func_t func)
	{
		orig_syscall = func;
	}
	// Set this syscall trace attach impl instance to the global ones, which
	// could be accessed by text segment transformer
	void set_to_global()
	{
		global_syscall_trace_attach_impl = this;
	}
	int detach_by_id(int id);
	int create_attach_with_ebpf_callback(
		ebpf_run_callback &&cb, const attach_private_data &private_data,
		int attach_type);
	syscall_trace_attach_impl(const syscall_trace_attach_impl &) = delete;
	syscall_trace_attach_impl &
	operator=(const syscall_trace_attach_impl &) = delete;
	syscall_trace_attach_impl()
	{
	}
};

} }
```

The callback type (`<bpftime>/attach/base_attach_impl/base_attach_impl.hpp` lines 24–25), copied character for character:

```cpp
// A wrapper function for an entry function of an ebpf program
using ebpf_run_callback = std::function<int(void *memory, size_t memory_size,
					    uint64_t *return_value)>;
```

Although the name contains ebpf, it is just a `std::function`; **you can put a plain C++ lambda straight into it, with no need to write an eBPF program at all**. The probe program in section 5 does exactly that.

**Because `ebpf_run_callback` is a `std::function`, `src/bpftime_hook.c` would have to become C++ (`.cpp`) on this route, or keep a thin C++ wrapper layer. This is the main cost of route B relative to route A.**

Which syscall to select, and whether to attach at enter or exit, is expressed with this struct (`attach/syscall_trace_attach_impl/include/syscall_trace_attach_private_data.hpp`), copied character for character:

```cpp
// Private data of syscall trace attach
struct syscall_trace_attach_private_data : public attach_private_data {
	// Syscall id to be attached. -1 for all syscalls
	int sys_nr;
	// True for syscall entry, false for syscall exit
	bool is_enter;
	// Initializa this private data instance from the string format of
	// tracepoint id
	int initialize_from_string(const std::string_view &sv);
};
```

`sys_nr` takes the syscall number directly (on x86_64, `SYS_write` = 1, `SYS_getpid` = 39, etc.), and `-1` means all syscalls. You can also call `initialize_from_string()` with a kernel tracepoint id string and let it parse it, but assigning these two fields directly is simpler, and direct assignment is what the probe program used.

The context structs the callback receives (same header), copied character for character:

```cpp
// Used for ebpf arguments
struct trace_event_raw_sys_enter {
	struct trace_entry ent;
	long int id;
	long unsigned int args[6];
	char __data[0];
};
// Used for ebpf arguments
struct trace_event_raw_sys_exit {
	struct trace_entry ent;
	long int id;
	long int ret;
	char __data[0];
};
```

The enter callback receives `trace_event_raw_sys_enter`, where `id` is the syscall number and `args[0..5]` are the six arguments; the exit callback receives `trace_event_raw_sys_exit`, where `ret` is the return value of the original syscall.

The full procedure to register a hook:

```cpp
syscall_trace_attach_impl att;
att.set_original_syscall_function(my_orig_syscall_fn);  // provide the function that "executes the real syscall"

syscall_trace_attach_private_data d;
d.sys_nr   = 1;      // SYS_write
d.is_enter = true;   // attach at the enter point

int id = att.create_attach_with_ebpf_callback(
    [](void *memory, size_t memory_size, uint64_t *return_value) -> int {
        // ... decision logic ...
        return 0;
    },
    d, ATTACH_SYSCALL_TRACE);
// id >= 0 means success; remove it with att.detach_by_id(id)
```

Then connect `att.dispatch_syscall(...)` to the zpoline transformer's `set_call_hook` (or call `att.set_to_global()` to let bpftime's own agent connect it).

Linking requires this command, which compiles:

```
g++ -std=c++20 -O1 -o probe probe.cpp \
  -I<bpftime>/attach/base_attach_impl \
  -I<bpftime>/attach/syscall_trace_attach_impl/include \
  -I<bpftime>/third_party/spdlog/include \
  -I<bpftime>/build/attach/syscall_trace_attach_impl \
  <bpftime>/build/attach/syscall_trace_attach_impl/libbpftime_syscall_trace_attach_impl.a \
  <bpftime>/build/third_party/spdlog/libspdlog.a -lpthread
```

Note the `-I<bpftime>/build/attach/syscall_trace_attach_impl` entry: `syscall_id_list.h` is generated at build time by `generate_syscall_id_table.sh` and exists only in the build directory.

### 4.3 Route C: the full eBPF flow (not exercised)

That is, use `bpftime load` to start the syscall server, load the compiled `.bpf.o` into shared memory, and then use `bpftime start -s` to start the target program. This route **was not run end to end, nor was it attempted**. The reason: compiling an eBPF program requires `clang -target bpf`, and the only clang available was the self-built ClangIR-branch version, which crashes on an assertion failure when compiling bpftime's bundled `daemon/bpf_tracer.c` (see section 3). Whether it can compile a simpler eBPF program was not tested. This route is also the most roundabout one here — the decision logic is already existing C code, and there is no reason to translate it into eBPF bytecode.

## 5. How "skip the original syscall and give my own return value" is expressed in bpftime

Explained at two layers.

**On route A (your own hook function)**: simply `return` directly, without calling `orig`. See section 4.1.

**On route B / route C (bpftime's attach layer and eBPF)**: it relies on two helper functions. They are declared in `<bpftime>/attach/base_attach_impl/base_attach_impl.hpp` lines 76–110, copied character for character:

```cpp
// The use of extern "C" allows the function to be called from C code
extern "C" {

// Set the return value of the current context
inline uint64_t bpftime_set_retval(uint64_t value)
{
	using namespace bpftime::attach;
	if (curr_thread_override_return_callback.has_value()) {
		curr_thread_override_return_callback.value()(0, value);
	} else {
		spdlog::error(
			"Called bpftime_set_retval, but no retval callback was set");
		throw std::invalid_argument(
			"Called bpftime_set_retval, but no retval callback was set");
	}
	return 0;
}

// Override the return value of the current context
inline uint64_t bpftime_override_return(uint64_t ctx, uint64_t value)
{
	using namespace bpftime::attach;
	if (curr_thread_override_return_callback.has_value()) {
		spdlog::debug("Overriding return value for ctx {:x} with {}",
			      ctx, value);
		curr_thread_override_return_callback.value()(ctx, value);
	} else {
		spdlog::error(
			"Called bpftime_override_return, but no retval callback was set");
		throw std::invalid_argument(
			"Called bpftime_override_return, but no retval callback was set");
	}
	return 0;
}

} // extern "C"
```

On the eBPF-program side, the helper numbers these two functions are registered as (`runtime/src/bpf_helper.cpp`):

- `BPF_FUNC_override_return = 58`, helper name `"bpf_override_return"`, implementation pointing to `bpftime_override_return` (line 895, lines 1199–1203)
- `BPF_FUNC_set_retval = 187`, helper name `"bpf_set_retval"`, implementation pointing to `bpftime_set_retval` (line 1024, lines 1217–1221)

**Where the semantics land**: `dispatch_syscall` in `attach/syscall_trace_attach_impl/src/syscall_trace_attach_impl.cpp`, the three key pieces:

- Lines 34–39: before entering the enter callback, it installs an `override_return_set_callback` that sets `is_overrided` to true and stores the value in `user_ret`.
- Lines 69–72: after the enter callback runs, `if (is_overrided) { return user_ret; }` — **it returns right here, and the `orig_syscall(...)` at line 80 is never executed at all**. This is "skipping the original syscall."
- Line 80 executes the original syscall; lines 91–93 are the `if (is_overrided) { return user_ret; }` after the exit callback — by then the original syscall **has already executed**, and only the return value is replaced.

So: **to "skip," you must attach at enter (`is_enter = true`) and call `bpftime_override_return` or `bpftime_set_retval` in the callback; calling these two functions at exit can only change the return value, not undo the fact that it "has already executed."**

**A probe program was written to run through all four cases.** The program uses a counter function as the "original syscall," and its output is as follows:

```
A: attach id = 1
A: dispatch_syscall returned 777, orig_syscall called 0 time(s)
B: dispatch_syscall returned -13 (as errno: -13), orig_syscall called 0 time(s)
C: dispatch_syscall returned 4242, orig_syscall called 1 time(s)
D: dispatch_syscall returned 999, orig_syscall called 1 time(s)
```

The four cases are:

- A: calling `bpftime_override_return(0, 777)` in the enter callback → returns 777, and the original syscall is called **0 times**.
- B: calling `bpftime_set_retval((uint64_t)-13)` in the enter callback → returns -13 (i.e. `-EACCES`), and the original syscall is called **0 times**. This shows that injecting an errno-style failure also goes through this route.
- C: the enter callback does nothing → the original syscall is called 1 time and returns its own 4242.
- D: calling `bpftime_override_return(0, 999)` in the exit callback → returns 999, but the original syscall **has already been called 1 time**.

## 6. Injection methods

The `bpftime` command-line tool (`tools/cli/main.cpp`) provides five subcommands; the `--help` output:

```
Subcommands:
  attach                 Inject bpftime-agent to a certain pid
  detach                 Detach all attached agents
  load                   Start an application with bpftime-server injected
  start                  Start an application with bpftime-agent injected
  trace                  Run a loader with bpftime-server injected, then attach bpftime-agent to a running process
```

What each actually does (`tools/cli/main.cpp` lines 985–1050, `run_command()` lines 106–175, `inject_by_frida()`):

- `bpftime load <command>`: `LD_PRELOAD=<...>/libbpftime-syscall-server.so` and then `execvpe` that command. Used to run the "loader" (the process that installs eBPF programs into shared memory).
- `bpftime start <command>`: without `-s`, it is `LD_PRELOAD=<...>/libbpftime-agent.so`; **with `-s` / `--enable-syscall-trace`, it becomes `LD_PRELOAD=<...>/libbpftime-agent-transformer.so` and additionally sets `AGENT_SO=<...>/libbpftime-agent.so`**. The latter is the zpoline route.
- `bpftime attach <PID>`: injects into an already-running process with Frida; with `-s`, it injects the transformer and passes the agent path as an argument.
- `bpftime trace`: first starts the loader with the syscall-server, then injects the agent into the target process; supports `--pid`, `--pidof`, `--auto-refresh-ms`.

**So for the zpoline route, it is essentially two environment variables** (`attach/text_segment_transformer/README.md` says the same):

```bash
export AGENT_SO=<bpftime>/build/runtime/agent/libbpftime-agent.so
LD_PRELOAD=<bpftime>/build/attach/text_segment_transformer/libbpftime-agent-transformer.so your_program
```

If `AGENT_SO` is not set, the transformer prints `Please set AGENT_SO to the bpftime-agent when use this tranformer` and does nothing (`agent-transformer.cpp` lines 56–71).

**Route A does not need bpftime's injection mechanism at all.** `src/bpftime_hook` is itself a `.so`; it is loaded with this repository's existing LD_PRELOAD mechanism, and in its constructor it `dlopen`s the transformer, calls `setup_syscall_tracer()`, and installs its own hook. This way backend 2's startup matches backend 1, and the test scripts change minimally.

## 7. Deployment preconditions and pitfalls

**1. Permission for `mmap(0x0, ...)`.** `setup_syscall_tracer()` maps one page at address 0 with `MAP_FIXED`. Where `vm.mmap_min_addr` is 65536, the usual default, a minimal C program doing only that one mmap behaves like this:

- Run as root (uid 0): `mmap(0x0)=(nil) errno=0 (Success)` — success. root has `CAP_SYS_RAWIO` and bypasses `mmap_min_addr`.
- Run as nobody (uid 65534): `mmap(0x0)=0xffffffffffffffff errno=1 (Operation not permitted)` — failure, EPERM.

**In other words: either run as root (or give the process `CAP_SYS_RAWIO`), or set `vm.mmap_min_addr` to 0.** On failure the transformer just does `exit(1)` and does not degrade gracefully. Note that lowering `vm.mmap_min_addr` is a system-wide setting and affects every process on the machine.

An aside: bpftime's own error log reports the wrong errno. When run as non-root it prints `Failed to perform mmap: errno=25, message=Inappropriate ioctl for device`, whereas the real errno for that same mmap, measured separately, is 1 (EPERM). 25 is ENOTTY, which has nothing to do with mmap; it is presumably the logging library's initialization clobbering errno. **On seeing errno=25 do not go looking up ENOTTY; the real cause is permissions.**

**2. `setup_syscall_tracer()` scans only once; libraries brought in later by `dlopen` are not caught.** The mechanism (`attach/text_segment_transformer/text_segment_transformer.cpp` lines 193–289, 290 lines total): `setup_syscall_tracer()` opens `/proc/self/maps` and reads it once, collects all segments with `x` permission at that moment into a vector, and then calls `rewrite_segment()` on each to rewrite the `syscall` instructions. In the whole file there is **no rescan mechanism whatsoever**: no dlopen hook, no `dl_iterate_phdr` callback, no periodic rescan. `grep -n "dlopen\|dl_iterate_phdr\|rescan"` in these two .cpp files matches only the single `bpftime::setup_syscall_tracer();` call itself at `agent-transformer.cpp:74`.

The consequence was reproduced on the test host. The test program first calls `setup_syscall_tracer()` and installs the hook (the hook returns a made-up 555555 for syscall number 39, i.e. `getpid`), and only then `dlopen`s a self-written `liblate.so` containing a function that issues `getpid` directly via inline assembly. Its output:

```
in-main raw getpid   = 555555 (expect 555555 = intercepted)
late-dlopen getpid   = 405674 (555555 = intercepted; a real pid = MISSED)
hook invocations during dlopen+call = 9
```

The second line is the real process id 405674 rather than 555555, showing that **the `syscall` instructions in the library loaded later were not rewritten, and its syscalls escaped interception entirely**. (The 9 in the third line is the openat/mmap and similar calls that dlopen itself issues inside libc; that code was already rewritten at setup time, so it was caught — which conversely confirms that only the segments already mapped at the moment of setup count.)

Mitigation: call `setup_syscall_tracer()` **once more** after the dlopen, and the new library is caught. The same test, changed to measure once, call setup once more, then measure again, outputs:

```
before 2nd setup: late-dlopen getpid = 405758
after  2nd setup: late-dlopen getpid = 555555 (555555 = now intercepted)
```

This only means **it worked this one time in testing**, not that it is an officially guaranteed usage: the second call re-maps page zero with `MAP_FIXED` and re-disassembles all executable segments, and the locations already changed to `ff d0` in the first pass are no longer `syscall` instructions in the second pass; whether the disassembler's instruction boundaries get misaligned as a result was not further verified. During the rescan the hook also has to be removed first and reinstalled afterward (the tested code does exactly this), otherwise the syscalls that setup itself issues would enter the hook.

Consequence for deployment: if backend 2 calls `setup_syscall_tracer()` in its own `.so` constructor, then any library the target program `dlopen`s afterward is not intercepted. For write programs like fio and dd that load all their libraries at startup this is not a problem; but for programs that load plugins/backends only while running (for example, fio's ioengine is loaded dynamically), you must either trigger a rescan after the plugin has loaded, or write this limitation explicitly into the preconditions of the test scripts. **This point has not been checked against this repository's actual write path**, only against the minimal reproduction program above.

**3. Architecture limitation.** The zpoline implementation is x86_64 only; the aarch64 branch is empty.

**4. There is no `.so`-level static-link interface.** `bpftime_base_attach_impl` is an INTERFACE library (`attach/base_attach_impl/CMakeLists.txt` line 1, `add_library(bpftime_base_attach_impl INTERFACE)`), header-only, with no `.a` artifact. Its two helpers `bpftime_set_retval` / `bpftime_override_return` are `inline`, so including the header is enough.

## 8. Limitations

- The full eBPF flow (`bpftime load` + `.bpf.o` + `bpftime start -s`) was never run.
- Nothing was run using bpftime's shared-memory mechanism. When the transformer + agent were started on their own via `LD_PRELOAD`, the target process received SIGSEGV (exit code 139); this happened without a syscall server and without shared memory, and was **not investigated further**. Route A does not involve the agent or shared memory, so this problem is not blocking; but if the "let bpftime's own agent take over" branch of route B is used later, this crash must be diagnosed first.
- No testing was done on this repository's actual write path (`write` / `pwrite64` / `fsync` / `fdatasync` / `ftruncate` / `openat` / `close`).
- Whether clang can compile a simple eBPF program was not tested (only that it crashes compiling bpftime's bundled `daemon/bpf_tracer.c`).
- Multithreaded behavior was not tested. The zpoline hook is a process-global function pointer (`static syscall_hooker_func_t call_hook`), and its thread safety was not examined.

## 9. Four facts that are easy to get wrong

- **Do not read `runtime/syscall-server/syscall_context.hpp` to find the hook signature.** That file has no syscall hook interface; it is where bpftime fakes the `bpf()` syscall in user space. What to read is `attach/text_segment_transformer/text_segment_transformer.hpp` (route A) and `attach/syscall_trace_attach_impl/include/syscall_trace_attach_impl.hpp` (route B).
- **The language choice for `src/bpftime_hook.c` depends on which route is taken.** On route A it stays a `.c` file (the hook function type is a plain C function pointer, symbols are fetched with `dlopen`/`dlsym`, no C++ needed); on route B it must become `.cpp` or gain a C++ wrapper layer, because `ebpf_run_callback` is a `std::function`.
- **Neither configuration nor build uses clang** (gcc is used); clang crashes only at the step of compiling eBPF bytecode, which is the reason the daemon is turned off.
- **The target name `bpftime_syscall_server` does not exist**; the real target name is `bpftime-syscall-server` (a hyphen in the middle, not an underscore — `runtime/syscall-server/CMakeLists.txt` line 6, `add_library(bpftime-syscall-server SHARED`).
