/*
 * Copyright <Current Year>, <Your Name/Organization>
 * Copyright 2023-2024, Microsoft Research, University of Cambridge
 * SPDX-License-Identifier: MIT
 *
 * Haiku-specific hooks for snmalloc integration into libroot.
 */

#include <pthread.h> // For pthread_once
#include <support/TLS.h>    // For Haiku's TLS (tls_set, tls_get)

// Define the PAL type *before* including core snmalloc headers that might use it.
// This PALHaikuUser should be defined in "snmalloc/pal_haiku_user.h"
#include "snmalloc/pal_haiku_user.h" // Haiku-specific userland PAL from within /src
#define SNMALLOC_PAL_TYPE PALHaikuUser

// Main snmalloc include - this should bring in necessary core types and APIs.
// It will use SNMALLOC_PAL_TYPE.
#include <snmalloc/snmalloc_core.h>
// PAL Data Structures, which often contain fork handling logic.
#include <snmalloc/pal/pal_ds.h>
// Note: <snmalloc/pal/pal_haiku.h> from the reference tree is no longer directly included here.
// Our local pal_haiku_user.h provides the PALHaikuUser class.


// Global flag to track if snmalloc's core/global components are initialized.
// snmalloc often handles its own global initialization via static constructors
// or on first allocation. If an explicit global init is required by snmalloc,
// it would be called in init_snmalloc_once.
static pthread_once_t sSnmallocGlobalInitOnce = PTHREAD_ONCE_INIT;

static void
init_snmalloc_globals_once()
{
    // This function is called only once per process.
    // If snmalloc requires any explicit one-time global setup beyond
    // what its static initializers do, it would go here.
    // For example: snmalloc::GlobalAlloc::ensure_init(); (hypothetical)
    // Most modern versions of snmalloc handle this transparently.
    // Ensure the PAL is active/notified if needed.
}


// Called by libroot startup (eg. from crt0 or early libroot init)
extern "C" status_t
__init_heap()
{
    // Ensure any global snmalloc initialization happens once.
    pthread_once(&sSnmallocGlobalInitOnce, init_snmalloc_globals_once);

    // Traditionally, this function in Haiku might initialize malloc-specific
    // mutexes or global state for the previous allocator.
    // snmalloc manages its own global state.
    // We might need to initialize the TLS slot for the main thread here,
    // or ensure it's done before first allocation.
    // tls_set(TLS_MALLOC_SLOT, (void*)0); // Or some other initial value
    // If snmalloc uses TLS for its thread-local allocators, it typically
    // initializes it on first use by a thread.
    return B_OK;
}


// Called by libroot when a new thread is created (after actual thread creation)
extern "C" void
__heap_thread_init()
{
    // Ensure global snmalloc state is initialized before any thread operations.
    pthread_once(&sSnmallocGlobalInitOnce, init_snmalloc_globals_once);

    // If snmalloc requires an explicit call to register a new thread or
    // initialize its thread-local allocator.
    // Example: snmalloc::ThreadAlloc::ensure_init(); (hypothetical)
    // snmalloc is designed so that the first allocation on a thread
    // typically sets up the thread-local allocator automatically.
    // So, this function might remain empty or be very minimal.

    // Haiku's previous wrapper used TLS_MALLOC_SLOT for its own mutex striping.
    // snmalloc does not need this specific TLS slot for that purpose.
    // If snmalloc uses TLS for other reasons (e.g. its own thread data pointer),
    // it will manage that via its own mechanisms (e.g. `thread_local` keyword
    // or PAL-specific TLS like `Pal::ThreadLocalState`).
}


// Called by libroot when a thread exits
extern "C" void
__heap_thread_exit()
{
    // If snmalloc requires an explicit call to tear down thread-local
    // resources or return them to a global pool.
    // Example: snmalloc::ThreadAlloc::release_local_resources(); (hypothetical)
    // snmalloc usually handles this via thread-local destructors or by returning
    // resources to the global allocator when thread-local caches are full/emptied.
}


// Called by libroot in the parent process before fork()
extern "C" void
__heap_before_fork()
{
    // Notify snmalloc that a fork is about to happen.
    // This allows snmalloc to acquire any necessary internal locks
    // to ensure a consistent state for the child.
    snmalloc::PALDS::pal_pre_fork();
}


// Called by libroot in the child process after fork()
extern "C" void
__heap_after_fork_child()
{
    // Notify snmalloc that it's now running in the child process.
    // This allows snmalloc to re-initialize locks, reset thread-specific data
    // (as only the calling thread exists in the child), and clean up any
    // state that shouldn't be inherited or might be inconsistent.
    snmalloc::PALDS::pal_child_post_fork();
}


// Called by libroot in the parent process after fork()
extern "C" void
__heap_after_fork_parent()
{
    // Notify snmalloc that the parent process has resumed after fork.
    // This allows snmalloc to release any locks acquired in pre_fork.
    snmalloc::PALDS::pal_parent_post_fork();
}


// This function was a no-op in Haiku's OpenBSD malloc wrapper.
// It's likely safe to keep it as a no-op.
extern "C" void
__heap_terminate_after()
{
}


// --- mspace API Stubs ---
// The following functions provide stubs for the dlmalloc-style mspace API.
// snmalloc does not have a concept of isolated mspaces. These stubs redirect
// to the global snmalloc allocator, meaning mspace isolation is NOT provided.
// They are for API compatibility if older code still uses these interfaces.

#include <stdio.h> // For dprintf (conditionally)
#include <stdlib.h> // For malloc, free etc. (which snmalloc will override)
// For aligned_alloc and malloc_usable_size, <malloc.h> is often the provider
// on POSIX systems. If snmalloc's overrides ensure these are declared through
// stdlib.h or its own headers, then <malloc.h> might not be strictly needed.
// Let's include it to be safe for now for these specific functions.
#include <malloc.h>


extern "C" {

typedef void* mspace;

/**
 * @brief Creates a new memory allocation space (mspace).
 * Stub implementation for snmalloc. Ignores parameters as snmalloc uses a global heap.
 * @param capacity Initial capacity (ignored).
 * @param locked Locking mode (ignored).
 * @return A dummy non-NULL mspace handle on supposed success, always (mspace)1.
 */
mspace create_mspace(size_t capacity, int locked)
{
	(void)capacity; // Ignored
	(void)locked;   // Ignored

#ifdef DEBUG_SNMALLOC_HOOKS
	dprintf("snmalloc_hooks: create_mspace(capacity: %lu, locked: %d) called. Returning dummy mspace handle. Mspace isolation not provided.\n", capacity, locked);
#endif
	// Return a non-NULL dummy handle. Any non-NULL value will do,
	// as it's not actually used by the other mspace_* stubs.
	return (mspace)1;
}

/**
 * @brief Destroys an mspace.
 * Stub implementation for snmalloc. This is a no-op.
 * @param msp The mspace handle (ignored).
 * @return Always 0 (success).
 */
size_t destroy_mspace(mspace msp)
{
	(void)msp; // Ignored

#ifdef DEBUG_SNMALLOC_HOOKS
	dprintf("snmalloc_hooks: destroy_mspace(msp: %p) called. No-op.\n", msp);
#endif
	// dlmalloc's destroy_mspace returns 0 on success, or the number of blocks still allocated.
	// As this is a stub for a global allocator, returning 0 is the simplest "success".
	return 0;
}

/**
 * @brief Allocates memory from an mspace.
 * Stub implementation for snmalloc. Ignores mspace and uses global malloc.
 * @param msp The mspace handle (ignored).
 * @param bytes Number of bytes to allocate.
 * @return Pointer to allocated memory, or NULL on failure.
 */
void* mspace_malloc(mspace msp, size_t bytes)
{
	(void)msp; // Ignored
	return malloc(bytes);
}

/**
 * @brief Frees memory allocated from an mspace.
 * Stub implementation for snmalloc. Ignores mspace and uses global free.
 * @param msp The mspace handle (ignored).
 * @param mem Pointer to the memory to free.
 */
void mspace_free(mspace msp, void* mem)
{
	(void)msp; // Ignored
	free(mem);
}

/**
 * @brief Allocates and zero-initializes memory from an mspace.
 * Stub implementation for snmalloc. Ignores mspace and uses global calloc.
 * @param msp The mspace handle (ignored).
 * @param n_elements Number of elements.
 * @param elem_size Size of each element.
 * @return Pointer to allocated and zeroed memory, or NULL on failure.
 */
void* mspace_calloc(mspace msp, size_t n_elements, size_t elem_size)
{
	(void)msp; // Ignored
	return calloc(n_elements, elem_size);
}

/**
 * @brief Reallocates memory from an mspace.
 * Stub implementation for snmalloc. Ignores mspace and uses global realloc.
 * @param msp The mspace handle (ignored).
 * @param mem Pointer to the previously allocated memory.
 * @param newsize New size in bytes.
 * @return Pointer to reallocated memory, or NULL on failure.
 */
void* mspace_realloc(mspace msp, void* mem, size_t newsize)
{
	(void)msp; // Ignored
	return realloc(mem, newsize);
}

/**
 * @brief Allocates aligned memory from an mspace.
 * Stub implementation for snmalloc. Ignores mspace and uses global aligned_alloc.
 * @param msp The mspace handle (ignored).
 * @param alignment Alignment constraint.
 * @param size Number of bytes to allocate.
 * @return Pointer to allocated aligned memory, or NULL on failure.
 */
void* mspace_memalign(mspace msp, size_t alignment, size_t size)
{
	(void)msp; // Ignored
	// aligned_alloc is the C11 standard. memalign is a common POSIX name for it.
	// snmalloc typically provides aligned_alloc.
	return aligned_alloc(alignment, size);
}

/**
 * @brief Gets the usable size of an allocation from an mspace.
 * Stub implementation for snmalloc. Ignores mspace and uses global malloc_usable_size.
 * @param msp The mspace handle (ignored).
 * @param mem Pointer to the allocated memory.
 * @return Usable size of the allocation.
 */
size_t mspace_usable_size(mspace msp, void* mem)
{
	(void)msp; // Ignored
	return malloc_usable_size(mem);
}

// These _b* internal functions were likely BeOS R5 specific variants or internals.
// Stubbing them to global malloc/free for basic API compatibility.
void* _bmalloc_internal(mspace msp, size_t bytes)
{
	(void)msp; // Ignored
#ifdef DEBUG_SNMALLOC_HOOKS
	dprintf("snmalloc_hooks: _bmalloc_internal(msp: %p, bytes: %lu) called. Redirecting to malloc().\n", msp, bytes);
#endif
	return malloc(bytes);
}

void _bfree_internal(mspace msp, void* ptr)
{
	(void)msp; // Ignored
#ifdef DEBUG_SNMALLOC_HOOKS
	dprintf("snmalloc_hooks: _bfree_internal(msp: %p, ptr: %p) called. Redirecting to free().\n", msp, ptr);
#endif
	free(ptr);
}

// This function likely controlled dlmalloc's internal debug level.
// snmalloc has its own debugging mechanisms, often compile-time.
int mspace_set_debug_level(int level)
{
#ifdef DEBUG_SNMALLOC_HOOKS
	dprintf("snmalloc_hooks: mspace_set_debug_level(level: %d) called. No-op for snmalloc.\n", level);
#endif
	(void)level;
	return 0; // Return 0, indicating no specific dlmalloc debug level is active.
}

// This function was likely for dlmalloc's internal heap analysis.
void mspace_analyze(mspace msp)
{
	(void)msp; // Ignored
#ifdef DEBUG_SNMALLOC_HOOKS
	dprintf("snmalloc_hooks: mspace_analyze(msp: %p) called. No-op for snmalloc.\n", msp);
#endif
	// No operation.
}

// For dlmalloc, this checked if a specific mspace had no allocations.
// The global snmalloc heap is unlikely to be "empty" in this sense.
int mspace_is_empty(mspace msp)
{
	(void)msp; // Ignored
#ifdef DEBUG_SNMALLOC_HOOKS
	dprintf("snmalloc_hooks: mspace_is_empty(msp: %p) called. Returning 0 (false).\n", msp);
#endif
	return 0; // False, global heap is likely not empty.
}

// Standard C library allocation functions that might not be in snmalloc's
// primary override set but could be expected by some POSIX/Haiku code.
// snmalloc's `override/malloc.cc` and `override/malloc-extensions.cc` should
// cover most of these. If any are missing, they can be added here,
// calling the appropriate snmalloc functions.

// extern "C" void* valloc(size_t size) IS_WEAK ALIAS(aligned_alloc);
//  -> snmalloc provides aligned_alloc. valloc is typically page-aligned.
//     We can ensure this is correctly aliased or implemented if not directly covered.
//     A simple implementation:
// #include "snmalloc/pal/pal.h" // For OS_PAGE_SIZE
// extern "C" void* valloc(size_t size) {
//     return snmalloc::aligned_alloc(snmalloc::OS_PAGE_SIZE, size);
// }


// extern "C" void* memalign(size_t alignment, size_t size) IS_WEAK ALIAS(aligned_alloc);
//  -> snmalloc provides aligned_alloc. If `memalign` symbol is specifically needed:
// extern "C" void* memalign(size_t alignment, size_t size) {
//    return snmalloc::aligned_alloc(alignment, size);
// }

// Check for other Haiku-specific symbols from the old wrapper.c:
// _bmalloc_internal, _bfree_internal, create_mspace, destroy_mspace,
// mspace_malloc, mspace_free, mspace_calloc, mspace_realloc,
// mspace_memalign, mspace_usable_size, mspace_set_debug_level,
// mspace_analyze, mspace_is_empty.
//
// These appear to be related to a dlmalloc-style "mspace" concept.
// snmalloc does not have a direct equivalent. If these are critical for R5
// compatibility or some specific Haiku libraries:
// 1. They could be stubbed to call the global snmalloc allocator (losing the "mspace" isolation).
// 2. They could be marked as deprecated and potentially removed if no longer used.
// 3. A more complex effort could try to simulate mspaces using multiple snmalloc
//    allocator instances if snmalloc's architecture permits easy instantiation of
//    many independent allocators (less common for high-performance allocators like snmalloc).
// For a first pass, these would likely be stubbed or result in link errors if not provided,
// indicating further investigation is needed if they are actually called.

// Example stub for a mspace function:
// extern "C" void* mspace_malloc(void* msp, size_t bytes) {
//     (void)msp; // Ignore the mspace parameter
//     return snmalloc::malloc(bytes);
// }
// ... and so on for other mspace functions. This loses mspace semantics.
// A proper solution would require deeper analysis of mspace usage in Haiku.

// For now, we assume snmalloc's standard overrides cover the primary interface
// and these Haiku-specific hooks are the main integration points needed from this file.

// Ensure that C++ global constructors/destructors used by snmalloc are called.
// The Haiku runtime (crtbegin.c/crtend.c) should handle this for libroot.so.

// End of snmalloc_hooks.cpp
