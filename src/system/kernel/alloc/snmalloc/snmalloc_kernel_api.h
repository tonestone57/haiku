#pragma once
#ifndef SNMALLOC_KERNEL_API_H
#define SNMALLOC_KERNEL_API_H

/**
 * @file snmalloc_kernel_api.h
 * @brief C-style API for kernel memory allocation using snmalloc.
 *
 * This header defines the functions that kernel code will use to allocate,
 * free, and manage dynamically sized memory blocks once snmalloc is integrated
 * as the primary kernel allocator.
 */

#include <types.h> // For size_t, status_t, etc.
#include <SupportDefs.h> // For _EXPORT, B_OK, etc.

#ifdef __cplusplus
extern "C" {
#endif

/** @defgroup kmalloc_flags Kernel Malloc Flags
 *  Flags that can be passed to kmalloc(), kcalloc(), and krealloc().
 *  @{
 */
#define KMALLOC_NORMAL    0x0000 ///< No special flags.
#define KMALLOC_ZERO      0x0001 ///< Zero the allocated memory block before returning.
#define KMALLOC_NO_WAIT   0x0002 ///< Best-effort attempt to not block/wait if memory is not
                                 ///< immediately available. Allocation may still fail (return NULL).
/** @} */


/**
 * @brief Allocates a block of memory from the kernel heap.
 *
 * If `size` is 0, a minimal valid block is allocated that can be passed to kfree().
 *
 * @param size The number of bytes to allocate.
 * @param flags Allocation flags (see @ref kmalloc_flags).
 * @return A pointer to the allocated memory block, or NULL if the allocation fails.
 *         The memory is suitably aligned for any built-in type.
 */
_EXPORT void* kmalloc(size_t size, uint32 flags);

/**
 * @brief Frees a previously allocated block of memory.
 *
 * If `ptr` is NULL, kfree() does nothing.
 * Attempting to free a pointer not obtained from kmalloc(), kcalloc(),
 * krealloc(), or kmalloc_aligned() results in undefined behavior.
 *
 * @param ptr Pointer to the memory block to free.
 */
_EXPORT void  kfree(void* ptr);

/**
 * @brief Allocates memory for an array of `n_elements`, each of `element_size` bytes.
 *
 * The memory is initialized to zero.
 * If `n_elements` or `element_size` is 0, a minimal valid block is allocated.
 * Handles potential integer overflow when calculating total size.
 *
 * @param n_elements Number of elements to allocate.
 * @param element_size Size of each element in bytes.
 * @param flags Allocation flags (see @ref kmalloc_flags). KMALLOC_ZERO is implied.
 * @return A pointer to the allocated memory block, or NULL if allocation fails or overflow occurs.
 */
_EXPORT void* kcalloc(size_t n_elements, size_t element_size, uint32 flags);

/**
 * @brief Changes the size of a previously allocated memory block.
 *
 * If `ptr` is NULL, krealloc() behaves like kmalloc(new_size, flags).
 * If `new_size` is 0, krealloc() behaves like kfree(ptr) and returns a minimal valid block.
 * The contents of the block will be unchanged up to the minimum of the old and new sizes.
 * If the block is enlarged and KMALLOC_ZERO is specified in `flags`, the new portion
 * of the block is zeroed.
 *
 * @param ptr Pointer to the memory block to resize.
 * @param new_size The new size for the memory block in bytes.
 * @param flags Allocation flags (see @ref kmalloc_flags).
 * @return A pointer to the resized memory block (which may be different from `ptr`),
 *         or NULL if the request fails. If NULL is returned, the original block `ptr`
 *         is not freed.
 */
_EXPORT void* krealloc(void* ptr, size_t new_size, uint32 flags);

/**
 * @brief Returns the usable size of an allocated memory block.
 *
 * The usable size may be larger than the requested size due to internal padding
 * or alignment by the allocator.
 * If `ptr` is NULL, returns 0.
 *
 * @param ptr Pointer to the allocated memory block.
 * @return The number of usable bytes in the block.
 */
_EXPORT size_t kmalloc_usable_size(const void* ptr);

/**
 * @brief Initializes the snmalloc kernel allocator.
 *
 * This function must be called once during the kernel's early startup sequence,
 * after basic VM and spinlock functionality is available, but before any
 * significant kernel subsystems attempt to use dynamic memory allocation via
 * kmalloc() etc. It initializes the snmalloc PAL and any global state required
 * by snmalloc.
 *
 * @return B_OK on success, or an error code if initialization fails (typically panics on failure).
 */
_EXPORT status_t kmalloc_init(void);

/**
 * @brief Allocates size bytes and returns a pointer to the allocated memory.
 *        The memory address will be a multiple of alignment, which must be a
 *        power of two.
 *
 * @param alignment The alignment constraint. Must be a power of two.
 * @param size The number of bytes to allocate.
 * @param flags Allocation flags (see @ref kmalloc_flags).
 * @return A pointer to the allocated memory, or nullptr if allocation fails.
 */
_EXPORT void* kmalloc_aligned(size_t alignment, size_t size, uint32 flags);

// Standard C library allocation functions, also exported for kernel use
// when snmalloc is the selected kernel allocator. These will wrap the
// kmalloc/kfree implementations.
_EXPORT void* malloc(size_t size);
_EXPORT void  free(void* ptr);
_EXPORT void* calloc(size_t nmemb, size_t size);
_EXPORT void* realloc(void* ptr, size_t new_size);
_EXPORT void* memalign(size_t alignment, size_t size);
// valloc is typically page-aligned memalign
_EXPORT void* valloc(size_t size);


#ifdef __cplusplus
}
#endif

#endif // SNMALLOC_KERNEL_API_H
