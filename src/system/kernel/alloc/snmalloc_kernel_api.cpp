/*
 * Copyright <Current Year>, <Your Name/Organization>
 * Distributed under the terms of the MIT License.
 *
 * Kernel C-style API wrappers for snmalloc.
 */

#include "snmalloc_kernel_api.h"

// Core snmalloc includes. These paths assume snmalloc headers are accessible.
// The exact includes might depend on how snmalloc is structured and configured.
#include <snmalloc.h> // This should provide snmalloc::alloc, snmalloc::free etc.

// Include the Haiku Kernel PAL for snmalloc
#include "snmalloc/pal/pal_haiku_kernel.h"


// Global initialization for the snmalloc PAL and potentially snmalloc itself.
// This needs to be called once during kernel startup.
status_t kmalloc_init()
{
    // Initialize the Haiku Kernel PAL's static components (e.g., the VMArena)
    status_t pal_status = snmalloc::PALHaikuKernel::StaticInit();
    if (pal_status != B_OK) {
        panic("kmalloc_init: PALHaikuKernel::StaticInit failed with error %s!", strerror(pal_status));
        return pal_status; // Should be unreachable due to panic
    }

    // If snmalloc requires any explicit global initialization beyond what its PAL
    // or static constructors do, it would be called here.
    // Example: snmalloc::ensure_allocator_initialized(); // Hypothetical
    // Most modern snmalloc versions handle this transparently if the PAL is set up.

    dprintf("kmalloc_init: snmalloc kernel allocator initialized with PALHaikuKernel.\n");
    return B_OK;
}

// Optional: Teardown function if ever needed (rare for kernel allocators)
// void kmalloc_teardown()
// {
//     snmalloc::PALHaikuKernel::StaticTeardown();
//     // Any snmalloc global teardown, if applicable.
// }

void* kmalloc(size_t size, uint32 flags)
{
    // snmalloc's core alloc function doesn't typically take OS-level flags like KMALLOC_NO_WAIT.
    // The NO_WAIT behavior is primarily a concern for the PAL's `reserve` method when it
    // requests pages from the OS. If `PALHaikuKernel::reserve` is implemented to be
    // non-blocking (or to respect a similar hint), then allocations might fail early
    // if memory isn't available. For now, this kmalloc API doesn't directly pass
    // a "no_wait" hint down into snmalloc core beyond what the PAL does.

    if (size == 0) {
        // Standard malloc behavior: allocate a minimum valid block for size 0.
        // snmalloc might handle size 0 by returning a unique non-null pointer
        // or allocating a minimum size. Let's ensure we request at least 1 byte.
        size = 1;
    }

    void* ptr = nullptr;

    if (flags & KMALLOC_ZERO) {
        // Use snmalloc's templated version for zeroed allocation if available and efficient.
        // This is usually `snmalloc::alloc<snmalloc::ZeroMem::YesZero>(size)`.
        // If snmalloc::calloc is exposed at this level, it's another option.
        // For this C API, we can use the generic alloc then memset.
        ptr = snmalloc::alloc(size); // Uses the globally configured snmalloc instance
        if (ptr) {
            memset(ptr, 0, size);
        }
    } else {
        ptr = snmalloc::alloc(size);
    }

    if ((flags & KMALLOC_NO_WAIT) && ptr == nullptr) {
        // If allocation failed and NO_WAIT was specified, this is an acceptable outcome.
        // The PAL's reserve method should have tried its best not to block.
        // No special action here other than returning nullptr.
    }

    // dprintf("kmalloc(%zu, 0x%x) -> %p\n", size, flags, ptr);
    return ptr;
}

void kfree(void* ptr)
{
    if (ptr == nullptr) {
        return;
    }
    // dprintf("kfree(%p)\n", ptr);
    snmalloc::free(ptr);
}

void* kcalloc(size_t n_elements, size_t element_size, uint32 flags)
{
    size_t total_size = 0;
    // Check for overflow before multiplication
    if (n_elements > 0 && element_size > 0 &&
        n_elements > SIZE_MAX / element_size) {
        // Overflow would occur
        if (!(flags & KMALLOC_NO_WAIT)) {
             // Only panic if not NO_WAIT, otherwise failing with nullptr is acceptable.
            panic("kcalloc: integer overflow (%" B_PRIuSIZE " * %" B_PRIuSIZE ")", n_elements, element_size);
        }
        return nullptr;
    }
    total_size = n_elements * element_size;

    if (total_size == 0) {
        // Consistent with malloc(0), allocate a minimal block.
        // kcalloc is specified to return a pointer that can be passed to free.
        // The ZERO flag is implicit for kcalloc.
        return kmalloc(1, flags | KMALLOC_ZERO);
    }

    return kmalloc(total_size, flags | KMALLOC_ZERO);
}

void* krealloc(void* ptr, size_t new_size, uint32 flags)
{
    // dprintf("krealloc(%p, %zu, 0x%x)\n", ptr, new_size, flags);
    if (ptr == nullptr) {
        return kmalloc(new_size, flags);
    }

    if (new_size == 0) {
        kfree(ptr);
        // krealloc(ptr, 0) should return a minimal valid block or NULL.
        // Let's return a minimal block, consistent with malloc(0) via kmalloc.
        return kmalloc(1, flags);
    }

    // snmalloc::realloc should handle the actual reallocation.
    // The zeroing of newly allocated portions if the block grows is a
    // nuanced requirement for realloc. Standard realloc doesn't guarantee it.
    // If KMALLOC_ZERO is specified for krealloc, we should ensure this.
    void* new_ptr = snmalloc::realloc(ptr, new_size);

    if (new_ptr != nullptr && (flags & KMALLOC_ZERO)) {
        // If the block grew and KMALLOC_ZERO is set, zero the new portion.
        // This requires knowing the old usable size.
        // Note: snmalloc::realloc might return the same pointer if it could resize in place,
        // or a new pointer if it had to move the allocation.
        size_t old_usable_size = snmalloc::malloc_usable_size(new_ptr == ptr ? new_ptr : ptr);
        if (new_size > old_usable_size) {
             memset(static_cast<char*>(new_ptr) + old_usable_size, 0, new_size - old_usable_size);
        }
    }
    return new_ptr;
}

size_t kmalloc_usable_size(const void* ptr)
{
    if (ptr == nullptr) {
        return 0;
    }
    // snmalloc::malloc_usable_size typically takes non-const.
    // Casting away const here is generally safe if snmalloc's implementation
    // doesn't modify the pointed-to metadata in a way that breaks constness
    // of the user's view of their data block.
    return snmalloc::malloc_usable_size(const_cast<void*>(ptr));
}

// TODO: Implement kmalloc_aligned if added to the header.
// void* kmalloc_aligned(size_t size, size_t alignment, uint32 flags) {
//     if (alignment == 0 || !bits::is_pow2(alignment)) return nullptr; // Or some error
//     if (size == 0) size = 1;
//     void* ptr = snmalloc::aligned_alloc(alignment, size);
//     if (ptr && (flags & KMALLOC_ZERO)) {
//         memset(ptr, 0, size); // snmalloc::aligned_alloc doesn't zero by default
//     }
//     return ptr;
// }
