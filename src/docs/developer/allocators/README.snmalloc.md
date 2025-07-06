# snmalloc Integration in Haiku

## 1. Overview of `snmalloc` Integration

This document outlines the integration of `snmalloc` as the primary dynamic memory allocator for both the Haiku kernel and its userland C library (`libroot.so`). This initiative aims to replace the existing OpenBSD-derived `malloc` in userland and the slab allocator in the kernel with a modern, secure, and performant allocation solution.

## 2. Rationale for Integration

The decision to integrate `snmalloc` is driven by several factors:

*   **Modern Design:** `snmalloc` is a contemporary allocator written in C++, leveraging modern techniques for performance and scalability.
*   **Security Features:** It incorporates various hardening techniques (e.g., improved metadata protection, checks) designed to mitigate common memory corruption vulnerabilities. This is a key driver for enhancing Haiku's overall security posture.
*   **Performance:** `snmalloc` is designed for high performance across a range of allocation patterns and scales well in multi-threaded environments. While comprehensive benchmarking on Haiku is pending, its design principles suggest potential performance benefits.
*   **Active Maintenance:** `snmalloc` is an actively developed and maintained project (originally from Microsoft Research), ensuring ongoing improvements and bug fixes.
*   **Cross-Platform Nature:** While requiring a Platform Abstraction Layer (PAL), its core logic is platform-agnostic, making integration feasible.
*   **Potential for Unification:** Using the same underlying allocator technology (though with different PALs and configurations) for both kernel and userland can simplify understanding and potentially share some core improvements, even if the actual instances are separate.

## 3. Core `snmalloc` Library Location

The source code for the `snmalloc` library (licensed under the MIT License) is vendored within the Haiku source tree to ensure a consistent build environment and version control.

*   **Path:** `src/third_party/snmalloc_lib/src/snmalloc/`

This directory contains the core `snmalloc` implementation, including its internal data structures, algorithms, platform abstraction layer interfaces, and override files for standard allocation functions. When building Haiku, include paths will be configured to allow Haiku-specific code to include `snmalloc` headers using paths like `<snmalloc/snmalloc_core.h>`.

## 4. Kernel Integration Details

Integrating `snmalloc` into the Haiku kernel required a custom Platform Abstraction Layer (PAL) and an API wrapper to expose its functionality to kernel code.

### 4.1. Platform Abstraction Layer (PAL)

*   **Purpose:** The Kernel PAL provides `snmalloc` with the necessary Haiku kernel-specific primitives for memory reservation, virtual address space management, and synchronization.
*   **Location:** `src/system/kernel/alloc/snmalloc/pal_haiku_kernel.h`
*   **Strategy:**
    *   **VMArea:** On initialization, the PAL creates a dedicated, large, initially uncommitted kernel `VMArea` ("snmalloc_kernel_heap_arena"). This area serves as the exclusive source of virtual addresses for `snmalloc`'s kernel instance.
    *   **Page Management:** When `snmalloc` requests memory, the PAL allocates physical pages (using `vm_page_allocate_page_run`) and maps them into the allocated virtual address range within the dedicated VMArea.
    *   **VA Management:** The PAL includes a free-list based virtual address space manager (`_allocate_va_range`, `_free_va_range`) to manage allocations and deallocations within the VMArea, supporting splitting and coalescing of VA extents.
    *   **Synchronization:** Haiku kernel spinlocks are used to protect shared PAL data structures.
    *   **Metadata:** Internal static pools are used for PAL metadata (e.g., `VAExtent`, `HaikuKernelSubMapping`) to avoid recursion.

### 4.2. Kernel API

*   **Purpose:** To provide a standard set of allocation functions (`kernel_malloc`, `kernel_free`, `kernel_calloc`, `kernel_realloc`, `kernel_memalign`) for kernel code to use, backed by `snmalloc`.
*   **Location:**
    *   Header: `src/system/kernel/alloc/snmalloc/snmalloc_kernel_api.h`
    *   Implementation: `src/system/kernel/alloc/snmalloc/snmalloc_kernel_api.cpp`
*   **Interface:** These API functions typically wrap calls to `snmalloc::ThreadAlloc::get()` to obtain a thread-local `snmalloc` allocator instance and then call its respective allocation/deallocation methods.

### 4.3. Initialization

*   **Function:** `snmalloc_kernel_init()` (defined in `snmalloc_kernel_api.cpp`).
*   **Process:**
    1.  Calls `snmalloc::PALHaikuKernel::StaticInit()` to initialize the kernel PAL (e.g., create the VMArea, set up VA management pools).
    2.  Relies on `snmalloc`'s internal mechanisms (often triggered by the first call to `snmalloc::ThreadAlloc::get()`) to initialize its global metadata using the now-ready PAL.
*   **Call Site:** `snmalloc_kernel_init()` must be called early in the Haiku kernel's boot sequence, after basic VM is operational but before the new allocator is needed (and before the old slab allocator it replaces is heavily used).

## 5. Userland (`libroot.so`) Integration Details

For userland applications, `snmalloc` is integrated into `libroot.so`, Haiku's primary C library, replacing the previous OpenBSD-derived `malloc`.

### 5.1. Override Mechanism

*   `snmalloc` provides a set of C++ source files (typically located in its `override/` directory, e.g., `malloc.cc`, `new.cc`, `malloc_extensions.cc`) that define standard allocation symbols (`malloc`, `free`, `calloc`, `realloc`, `memalign`, C++ `operator new`, `operator delete`, `aligned_alloc`, `malloc_usable_size`, etc.).
*   These override files are compiled and linked directly into `libroot.so`. When an application calls a standard allocation function, it resolves to `snmalloc`'s implementation.
*   The core `snmalloc` library files needed by these overrides are vendored at `src/third_party/snmalloc_lib/src/snmalloc/`.

### 5.2. Haiku-Specific Hooks

*   **Purpose:** To allow `snmalloc` to correctly interact with Haiku-specific process and thread lifecycle events.
*   **Location:** `src/system/libroot/posix/malloc/snmalloc_hooks.cpp`
*   **Covered Hooks:**
    *   `__init_heap()`: Ensures one-time global initialization for `snmalloc` within the process.
    *   `__heap_thread_init()` / `__heap_thread_exit()`: Hooks for thread creation/destruction. `snmalloc` typically manages its thread-local allocators automatically via `ThreadAlloc::get()`.
    *   `__heap_before_fork()`: Calls `snmalloc::PALDS::pal_pre_fork()` to prepare `snmalloc`'s internal state before a `fork()`.
    *   `__heap_after_fork_parent()`: Calls `snmalloc::PALDS::pal_parent_post_fork()` in the parent after `fork()`.
    *   `__heap_after_fork_child()`: Calls `snmalloc::PALDS::pal_child_post_fork()` in the child after `fork()` to reinitialize locks and thread-local state.

### 5.3. `mspace` API Compatibility

*   **Background:** Haiku's previous `malloc` implementation (`wrapper.c`) provided `dlmalloc`-style `mspace` functions (e.g., `create_mspace`, `mspace_malloc`).
*   **`snmalloc` Approach:** `snmalloc` is a global allocator and does not have a direct concept of isolated `mspaces`.
*   **Current Solution:** To maintain API compatibility for any existing code that might still use these functions, the `mspace` API is implemented as a set of **stubs** within `snmalloc_hooks.cpp`.
    *   `create_mspace` returns a dummy non-NULL handle.
    *   All other `mspace_*` allocation functions (e.g., `mspace_malloc`, `mspace_free`) ignore the `mspace` handle and redirect to their global equivalents (e.g., standard `malloc`, `free`), which are provided by `snmalloc`.
    *   **Important:** This means that true `mspace` isolation (separate heaps) is **not** provided by these stubs. All allocations go to the global `snmalloc` heap.

## 6. Build System Integration Notes

Integrating `snmalloc` requires modifications to Haiku's `Jamfile`s for both the kernel and `libroot.so`.

*   **Core `snmalloc` Source:** The `snmalloc` library, copied to `src/third_party/snmalloc_lib/src/snmalloc/`, serves as the source for headers and compiled files.
*   **Include Paths:**
    *   For kernel: `KernelGRPATHS` (or equivalent) is updated to include `src/third_party/snmalloc_lib/src/` so that kernel code can use `<snmalloc/header.h>`.
    *   For `libroot`: `LibrootCXXGRPATHS` (or equivalent) is updated similarly.
*   **Kernel Build:**
    *   `snmalloc_kernel_api.cpp` and `pal_haiku_kernel.h` (via includes) are compiled into the kernel.
    *   Any necessary core `snmalloc` `.cpp` files (if not entirely header-driven for kernel use with the PAL) would also be compiled.
*   **`libroot.so` Build:**
    *   The original OpenBSD `malloc` C source files are removed from compilation.
    *   `snmalloc_hooks.cpp` is added and compiled as C++.
    *   `snmalloc`'s C++ override files (e.g., `malloc.cc`, `new.cc` from `src/third_party/snmalloc_lib/src/snmalloc/override/`) are added and compiled as C++.
*   **Compiler Flags:** Ensure C++17 (or the standard required by the vendored `snmalloc` version) is used for compiling all `snmalloc`-related C++ code.
*   **Linker Dependencies:** `libroot.so` will now have a direct dependency on the C++ standard library (e.g., `libstdc++.so` or `libc++.so`) due to the inclusion of `snmalloc` C++ code.

## 7. Current Status and Future Work

This document describes the planned and partially implemented integration of `snmalloc`.

**Current Status (as of this documentation draft):**

*   Design for kernel PAL and userland hooks is established.
*   Initial code for the kernel PAL, kernel API, userland hooks, and `mspace` stubs has been drafted.
*   Conceptual build system changes have been outlined.
*   File locations within the Haiku `/src` tree have been defined.

**Required Next Steps (User/Developer Actions):**

1.  **Vendor `snmalloc`:** Physically copy the chosen version of `snmalloc` source code into `src/third_party/snmalloc_lib/src/snmalloc/`.
2.  **Implement `Jamfile` Changes:** Accurately implement the described build system modifications in the relevant Haiku `Jamfile`s.
3.  **Call `snmalloc_kernel_init()`:** Integrate the call to `snmalloc_kernel_init()` into the kernel's boot sequence.
4.  **Thorough Testing (Iterative Process):**
    *   **Kernel PAL Standalone Tests:** Develop and run tests specifically for `pal_haiku_kernel.h`.
    *   **Initial Kernel Integration Test:** Replace a single, non-critical slab allocator use case and test system stability.
    *   **Userland Application Testing:** After `libroot.so` is rebuilt with `snmalloc`, conduct extensive testing across a wide range of Haiku applications and command-line tools.
    *   **Systematic Kernel Replacement:** Incrementally replace existing kernel slab allocator uses with the new `kernel_malloc` API, testing thoroughly at each stage.
5.  **Benchmarking:** Conduct performance comparisons between `snmalloc` and the original allocators in both userland and kernel contexts.
6.  **Address Compatibility & Performance Issues:** Debug and resolve any crashes, functional regressions, performance issues, or memory usage anomalies found during testing and benchmarking.
7.  **`mspace` API Review:** Evaluate the impact of the `mspace` stubbing strategy. If true `mspace` functionality is deemed critical for certain applications, further investigation into alternatives or refactoring of those applications will be needed.
8.  **Final Code Review & Polish:** Once stable and performant, conduct a final review of all integrated code.
9.  **Submission to Haiku Project:** Structure the changes into logical commits and submit them for review and merging.

This integration is a significant undertaking and will require careful, iterative development and testing.
