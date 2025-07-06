// snmalloc_kernel_api.cpp

#include "snmalloc_kernel_api.h"
#include "pal_haiku_kernel.h" // For snmalloc::PALHaikuKernel for init

// Define the PAL type *before* including core snmalloc headers that might use it.
#define SNMALLOC_PAL_TYPE PALHaikuKernel

// snmalloc core includes
#include <snmalloc/snmalloc_core.h>
#include <snmalloc/adapters/ThreadAlloc.h> // For snmalloc::ThreadAlloc
#include <snmalloc/backend/global_virtual_range.h> // For BackendMeta::ensure_init_kernel (if used)

// Haiku kernel includes
#include <string.h> // For memset
#include <stdio.h>  // For dprintf
#include <debug.h>  // For ASSERT, panic
#include <kernel.h> // For B_OK, B_NO_MEMORY etc.
#include <new>      // For __builtin_mul_overflow

// Global flag to ensure init is called only once.
static bool s_snmalloc_initialized = false;
// TODO: Add appropriate locking if kmalloc_init could be called concurrently,
// though it's expected to be called once during single-threaded kernel startup.

status_t kmalloc_init(void)
{
    if (s_snmalloc_initialized) {
        return B_OK; // Already initialized
    }

    dprintf("kmalloc_init: Initializing snmalloc kernel allocator...\n");

    status_t pal_status = snmalloc::PALHaikuKernel::StaticInit();
    if (pal_status != B_OK) {
        panic("kmalloc_init: PALHaikuKernel::StaticInit() failed with error %s!\n", strerror(pal_status));
        return pal_status; // Should be unreachable due to panic
    }
    dprintf("kmalloc_init: PALHaikuKernel initialized successfully.\n");

    // Initialize snmalloc's global backend metadata using the Haiku Kernel PAL.
    // This step is crucial and typically involves telling snmalloc's core
    // about the PAL and allowing it to set up its internal structures.
    // The exact function might vary slightly based on snmalloc versions,
    // but `BackendMeta<Pal>::ensure_init_kernel()` is a common pattern.
    // If snmalloc uses a different mechanism, this needs to be adapted.
    // For some configurations, the first call to ThreadAlloc::get() might implicitly
    // initialize, but an explicit call is safer for kernel environments.
    snmalloc::BackendMeta<snmalloc::PALHaikuKernel>::ensure_init_kernel();
    dprintf("kmalloc_init: snmalloc BackendMeta initialized.\n");


    // Verify by a test allocation (optional, but good for early fault detection)
    void* test_alloc = snmalloc::ThreadAlloc::get().alloc(16);
    if (test_alloc == nullptr) {
        panic("kmalloc_init: Post-init test allocation failed!");
        // PALHaikuKernel::StaticTeardown(); // Attempt cleanup if possible
        return B_NO_MEMORY; // Should be unreachable
    }
    snmalloc::ThreadAlloc::get().dealloc(test_alloc);
    dprintf("kmalloc_init: snmalloc global metadata implicitly initialized (verified by test alloc).\n");


    s_snmalloc_initialized = true;
    dprintf("kmalloc_init: snmalloc kernel allocator successfully initialized.\n");
    return B_OK;
}

void* kmalloc(size_t size, uint32 flags)
{
    if (!s_snmalloc_initialized) {
        // This should not happen if init is called correctly.
        // KMALLOC_NO_WAIT isn't typically handled by panicking.
        if (!(flags & KMALLOC_NO_WAIT)) {
            panic("kmalloc: snmalloc not initialized!");
        }
        // Attempting init here is risky as it might not be safe context.
        // For now, fail the allocation if not initialized.
        return nullptr;
    }

    size_t alloc_size = (size == 0) ? 1 : size; // Allocate at least 1 byte for size 0
    void* ptr = snmalloc::ThreadAlloc::get().alloc(alloc_size);

    if (ptr != nullptr && (flags & KMALLOC_ZERO)) {
        memset(ptr, 0, alloc_size);
    }

    if (ptr == nullptr && !(flags & KMALLOC_NO_WAIT)) {
        // If not KMALLOC_NO_WAIT, a failed allocation might be critical enough to panic
        // depending on kernel policy. For now, we just return nullptr.
        // panic("kmalloc: failed to allocate %zu bytes", alloc_size);
    }
    return ptr;
}

void kfree(void* ptr)
{
    if (ptr == nullptr) {
        return;
    }
    if (!s_snmalloc_initialized) {
        panic("kfree: Attempt to free pointer %p before snmalloc is initialized!", ptr);
        return;
    }
    snmalloc::ThreadAlloc::get().dealloc(ptr);
}

void* kcalloc(size_t nmemb, size_t element_size, uint32 flags)
{
    size_t total_size;
    if (__builtin_mul_overflow(nmemb, element_size, &total_size)) {
        if (!(flags & KMALLOC_NO_WAIT)) {
            panic("kcalloc: integer overflow (%" B_PRIuSIZE " * %" B_PRIuSIZE ")", nmemb, element_size);
        }
        return nullptr; // Overflow
    }

    if (total_size == 0) {
        // kmalloc handles size 0 by allocating a minimal block.
        // KMALLOC_ZERO is implicit for kcalloc.
        return kmalloc(0, flags | KMALLOC_ZERO);
    }
    // kmalloc will handle KMALLOC_ZERO if set in flags.
    return kmalloc(total_size, flags | KMALLOC_ZERO);
}

void* krealloc(void* ptr, size_t new_size, uint32 flags)
{
    if (!s_snmalloc_initialized) {
         if (!(flags & KMALLOC_NO_WAIT)) {
            panic("krealloc: snmalloc not initialized!");
        }
        return nullptr;
    }

    if (ptr == nullptr) {
        return kmalloc(new_size, flags);
    }

    if (new_size == 0) {
        kfree(ptr);
        // krealloc(ptr, 0) should return a minimal valid block or NULL.
        // kmalloc(0, flags) will return a minimal block.
        return kmalloc(0, flags);
    }

    // Get old size before realloc if we need to zero the new part
    size_t old_usable_size = 0;
    if (flags & KMALLOC_ZERO) {
         // Pass original ptr to usable_size as realloc might invalidate it
        old_usable_size = snmalloc::ThreadAlloc::get_usable_size(ptr);
    }

    void* new_ptr = snmalloc::ThreadAlloc::get().realloc(ptr, new_size);

    if (new_ptr != nullptr && (flags & KMALLOC_ZERO)) {
        if (new_size > old_usable_size) {
             memset(static_cast<char*>(new_ptr) + old_usable_size, 0, new_size - old_usable_size);
        }
    } else if (new_ptr == nullptr && !(flags & KMALLOC_NO_WAIT)) {
        // panic("krealloc: failed to reallocate to %zu bytes", new_size);
    }

    return new_ptr;
}

size_t kmalloc_usable_size(const void* ptr)
{
    if (ptr == nullptr) {
        return 0;
    }
    if (!s_snmalloc_initialized) {
        // Cannot determine size if allocator isn't up.
        // This situation implies a deeper problem.
        dprintf("kmalloc_usable_size: called before snmalloc initialized for ptr %p\n", ptr);
        return 0;
    }
    // snmalloc's get_usable_size is often on ThreadAlloc or a similar instance.
    return snmalloc::ThreadAlloc::get_usable_size(const_cast<void*>(ptr));
}

void* kmalloc_aligned(size_t alignment, size_t size, uint32 flags)
{
    if (!s_snmalloc_initialized) {
        if (!(flags & KMALLOC_NO_WAIT)) {
            panic("kmalloc_aligned: snmalloc not initialized!");
        }
        return nullptr;
    }

    // snmalloc requires alignment to be a power of two.
    // And typically alignment >= sizeof(void*).
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        // Not a power of two, or zero.
        // Kernel code should ensure valid alignment. For now, fail.
        if (!(flags & KMALLOC_NO_WAIT)) {
             panic("kmalloc_aligned: invalid alignment %zu", alignment);
        }
        return nullptr;
    }

    size_t alloc_size = (size == 0) ? 1 : size;
    void* ptr = snmalloc::ThreadAlloc::get().alloc_aligned(alignment, alloc_size);

    if (ptr != nullptr && (flags & KMALLOC_ZERO)) {
        memset(ptr, 0, alloc_size);
    }

    if (ptr == nullptr && !(flags & KMALLOC_NO_WAIT)) {
        // panic("kmalloc_aligned: failed to allocate %zu bytes with alignment %zu", alloc_size, alignment);
    }
    return ptr;
}

// Standard C API wrappers, calling the k* variants
// These ensure that kernel code calling malloc/free directly uses snmalloc when it's active.

#ifdef __cplusplus
extern "C" {
#endif

void* malloc(size_t size)
{
    return kmalloc(size, KMALLOC_NORMAL);
}

void free(void* ptr)
{
    kfree(ptr);
}

void* calloc(size_t nmemb, size_t size)
{
    // kcalloc already handles KMALLOC_ZERO implicitly in its call to kmalloc
    return kcalloc(nmemb, size, KMALLOC_NORMAL);
}

void* realloc(void* ptr, size_t new_size)
{
    return krealloc(ptr, new_size, KMALLOC_NORMAL);
}

void* memalign(size_t alignment, size_t size)
{
    // Check if alignment is a power of two, as kmalloc_aligned expects this.
    // Standard memalign might be more lenient, but POSIX says it should be power of two.
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        // Haiku's current kernel memalign (from heap.cpp) returns NULL for bad alignment.
        return nullptr;
    }
    return kmalloc_aligned(alignment, size, KMALLOC_NORMAL);
}

void* valloc(size_t size)
{
    // valloc is page-aligned allocation.
    // Need B_PAGE_SIZE from somewhere, typically <OS.h> or <vm_page.h>
    // pal_haiku_kernel.h has `static constexpr size_t page_size = B_PAGE_SIZE;`
    return kmalloc_aligned(snmalloc::PALHaikuKernel::page_size, size, KMALLOC_NORMAL);
}

#ifdef __cplusplus
} // extern "C"
#endif


#ifdef __cplusplus
// Ensures C++ static initializers used by snmalloc (if any beyond PAL) are handled.
// kmalloc_init() is the primary explicit initializer.
static class SnmallocKernelGlobalInitializer {
public:
    SnmallocKernelGlobalInitializer() {
        // dprintf("SnmallocKernelGlobalInitializer constructor (no-op for snmalloc logic)\n");
    }
    // No destructor needed, kernel allocator lives forever.
} s_global_snmalloc_initializer_obj_kernel_api;
#endif
