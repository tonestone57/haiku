// snmalloc_kernel_api.h
#pragma once

#include <sys/types.h> // For size_t
#include <KernelExport.h> // For KERNEL_EXPORT_DATA

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the snmalloc kernel allocator and its underlying PAL.
 *
 * This function must be called once during early kernel startup, after basic
 * VM is available but before this allocator is used. It sets up the
 * necessary VMArena and internal structures for snmalloc.
 *
 * @return B_OK on success, or an error code if initialization fails.
 *         If this function fails, the kernel allocator will not be available,
 *         which is typically a fatal condition for the kernel.
 */
status_t snmalloc_kernel_init(void);

/**
 * @brief Allocates memory from the snmalloc kernel heap.
 *
 * @param size The number of bytes to allocate.
 * @return A pointer to the allocated memory, or nullptr if allocation fails.
 *         The memory is not initialized.
 */
void* kernel_malloc(size_t size);

/**
 * @brief Frees memory previously allocated by kernel_malloc, kernel_calloc,
 *        kernel_realloc, or kernel_memalign.
 *
 * If ptr is nullptr, no operation is performed.
 *
 * @param ptr Pointer to the memory to free.
 */
void kernel_free(void* ptr);

/**
 * @brief Allocates memory for an array of nmemb elements of size bytes each
 *        and returns a pointer to the allocated memory. The memory is set to zero.
 *
 * @param nmemb Number of elements.
 * @param size Size of each element in bytes.
 * @return A pointer to the allocated memory, or nullptr if allocation fails.
 */
void* kernel_calloc(size_t nmemb, size_t size);

/**
 * @brief Changes the size of the memory block pointed to by ptr to size bytes.
 *
 * The contents will be unchanged in the range from the start of the region up to
 * the minimum of the old and new sizes. If the new size is larger than the old

 * size, the added memory will not be initialized.
 * If ptr is nullptr, the call is equivalent to kernel_malloc(size).
 * If size is 0 and ptr is not nullptr, the call is equivalent to kernel_free(ptr).
 *
 * @param ptr Pointer to the memory block to resize.
 * @param size The new size for the memory block in bytes.
 * @return A pointer to the (possibly relocated) resized memory block,
 *         or nullptr if the request fails. If nullptr is returned, the original
 *         block is left untouched.
 */
void* kernel_realloc(void* ptr, size_t size);

/**
 * @brief Allocates size bytes and returns a pointer to the allocated memory.
 *        The memory address will be a multiple of alignment, which must be a
 *        power of two.
 *
 * @param alignment The alignment constraint. Must be a power of two.
 * @param size The number of bytes to allocate.
 * @return A pointer to the allocated memory, or nullptr if allocation fails.
 */
void* kernel_memalign(size_t alignment, size_t size);

#ifdef __cplusplus
} // extern "C"
#endif
