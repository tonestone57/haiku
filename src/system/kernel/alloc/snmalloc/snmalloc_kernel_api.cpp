// snmalloc_kernel_api.cpp

#include "snmalloc_kernel_api.h"

// snmalloc core includes
// These paths assume snmalloc headers are available in the include path.
// The exact paths might need adjustment based on Haiku's build system and
// how snmalloc is integrated as a third-party library.
#include <snmalloc/snmalloc_core.h> // Or snmalloc_front.h / snmalloc.h - main entry point for ThreadAlloc
#include <snmalloc/adapters/ThreadAlloc.h> // For snmalloc::ThreadAlloc
#include <snmalloc/pal/pal_haiku_kernel.h> // For snmalloc::PALHaikuKernel for init

// Haiku kernel includes
#include <string.h> // For memset
#include <stdio.h>  // For dprintf (used for errors if panic is too harsh for some)
#include <debug.h>  // For ASSERT, panic
#include <kernel.h> // For B_OK, B_NO_MEMORY etc.

// Global flag to ensure init is called only once.
static bool s_snmalloc_initialized = false;
// TODO: Add appropriate locking if snmalloc_kernel_init could be called concurrently,
// though it's expected to be called once during single-threaded kernel startup.

status_t snmalloc_kernel_init(void)
{
    if (s_snmalloc_initialized) {
        return B_OK; // Already initialized
    }

    dprintf("snmalloc_kernel_init: Initializing snmalloc kernel allocator...\n");

    status_t pal_status = snmalloc::PALHaikuKernel::StaticInit();
    if (pal_status != B_OK) {
        panic("snmalloc_kernel_init: PALHaikuKernel::StaticInit() failed with error %s!\n", strerror(pal_status));
        return pal_status; // Should be unreachable due to panic
    }
    dprintf("snmalloc_kernel_init: PALHaikuKernel initialized successfully.\n");

    // For snmalloc, often the first call to ThreadAlloc::get() or similar
    // will trigger the initialization of its internal global metadata, using the
    // now-initialized PAL. Some allocators might offer an explicit global init function
    // like snmalloc::BackendMeta<snmalloc::PALHaikuKernel>::ensure_init_kernel();
    // For now, we rely on the implicit initialization via first use, ensuring the PAL is ready.
    // A test allocation could be done here to confirm.
    // For example:
    // void* test_alloc = snmalloc::ThreadAlloc::get().alloc(16);
    // if (test_alloc == nullptr) {
    //     panic("snmalloc_kernel_init: Post-PAL-init test allocation failed!");
    //     return B_NO_MEMORY; // Should be unreachable
    // }
    // snmalloc::ThreadAlloc::get().dealloc(test_alloc);
    // dprintf("snmalloc_kernel_init: snmalloc global metadata implicitly initialized (verified by test alloc).\n");

    // If snmalloc has an explicit function to initialize its global state after PAL init:
    // e.g. if (snmalloc::some_global_init_function<snmalloc::PALHaikuKernel>() != success) {
    //    panic("snmalloc_kernel_init: snmalloc_global_init_function failed!");
    //    return B_ERROR;
    // }
    // Lacking that specific knowledge, we assume PAL init is the main explicit step required from us.

    s_snmalloc_initialized = true;
    dprintf("snmalloc_kernel_init: snmalloc kernel allocator successfully initialized.\n");
    return B_OK;
}

void* kernel_malloc(size_t size)
{
    if (!s_snmalloc_initialized) {
        // This case should ideally not happen if init is called correctly at boot.
        // However, as a safeguard:
        if (snmalloc_kernel_init() != B_OK) {
             panic("kernel_malloc: snmalloc not initialized and init failed!");
             return nullptr;
        }
    }
    if (size == 0) {
        // Standard malloc behavior: return nullptr or a unique pointer that can be freed.
        // snmalloc might handle size 0 internally, returning a minimal chunk.
        // To be safe and explicit for a kernel, one might return nullptr or a specific
        // small allocation if that's the desired contract. Let's assume snmalloc handles it.
    }
    return snmalloc::ThreadAlloc::get().alloc(size);
}

void kernel_free(void* ptr)
{
    if (ptr == nullptr) {
        return;
    }
    if (!s_snmalloc_initialized) {
        // This is problematic. Freeing without the allocator being ready.
        panic("kernel_free: Attempt to free pointer %p before snmalloc is initialized!", ptr);
        return;
    }
    snmalloc::ThreadAlloc::get().dealloc(ptr);
}

void* kernel_calloc(size_t nmemb, size_t size)
{
    // Check for overflow
    size_t total_size;
    if (__builtin_mul_overflow(nmemb, size, &total_size)) {
        return nullptr; // Overflow
    }

    void* ptr = kernel_malloc(total_size);
    if (ptr != nullptr) {
        memset(ptr, 0, total_size);
    }
    return ptr;
}

void* kernel_realloc(void* ptr, size_t size)
{
    if (!s_snmalloc_initialized) {
        if (snmalloc_kernel_init() != B_OK) {
             panic("kernel_realloc: snmalloc not initialized and init failed!");
             return nullptr;
        }
    }
    // snmalloc's realloc should handle ptr == nullptr (behaves like malloc)
    // and size == 0 (behaves like free).
    return snmalloc::ThreadAlloc::get().realloc(ptr, size);
}

void* kernel_memalign(size_t alignment, size_t size)
{
    if (!s_snmalloc_initialized) {
        if (snmalloc_kernel_init() != B_OK) {
             panic("kernel_memalign: snmalloc not initialized and init failed!");
             return nullptr;
        }
    }
    // snmalloc requires alignment to be a power of two.
    // The PAL also enforces page_size minimum for its own alignment if it were to be used directly.
    // ThreadAlloc::alloc_aligned should handle this.
    if (alignment == 0) { // Or not a power of two, though snmalloc might assert/handle.
        // Standard memalign behavior for alignment=0 is often to default to malloc.
        // However, snmalloc expects power-of-two.
        // Let's ensure it's at least sizeof(void*) if 0, or rely on snmalloc's checks.
        // For simplicity, we pass it through; snmalloc should validate.
    }
    return snmalloc::ThreadAlloc::get().alloc_aligned(alignment, size);
}

// It's crucial that snmalloc_kernel_init() is called from a single thread
// during kernel startup before any other kernel_*alloc functions are invoked.
// The s_snmalloc_initialized flag is a basic safeguard.
//
// Thread Safety: snmalloc::ThreadAlloc is designed to be thread-safe. Each call
// to ::get() provides a thread-local allocator instance. The underlying PAL and
// global metadata are protected by their own locks or are designed for concurrency.
//
// Kernel Considerations:
// - Interrupts: snmalloc itself doesn't know about kernel interrupt contexts.
//   Calls to kernel_malloc/free from interrupt handlers would be problematic
//   if snmalloc tries to acquire locks that are not interrupt-safe or if it
//   triggers page faults (though our PAL provides wired memory).
//   Typically, kernel allocators used in interrupt context have special pools
//   or are non-blocking. This API, as defined, is likely NOT safe for direct
//   use from arbitrary interrupt handlers without further analysis of snmalloc's
//   internal locking and behavior. It's intended for thread context.
// - Preemption: Spinlocks in PALHaikuKernel disable interrupts, which also
//   disables preemption on the current CPU. This is standard for kernel spinlocks.
//   snmalloc's internal operations should be compatible with this.

#ifdef __cplusplus
// This ensures that if snmalloc uses C++ static initializers for any global
// setup, they are correctly handled when this object file is linked into the kernel.
// However, explicit initialization via snmalloc_kernel_init() is preferred.
// For some allocators, a global constructor might initialize some state.
// We are relying on snmalloc_kernel_init() for all explicit setup.
static class SnmallocKernelGlobalInitializer {
public:
    SnmallocKernelGlobalInitializer() {
        // This constructor could potentially call a very early, minimal snmalloc init
        // if the library required it before even the PAL is up.
        // However, our model is PAL init first, then snmalloc.
        // So, this is likely a no-op or for other C++ global setup.
        // dprintf("SnmallocKernelGlobalInitializer constructor (should be no-op for snmalloc logic itself)\n");
    }
} s_global_snmalloc_initializer_obj;
#endif
