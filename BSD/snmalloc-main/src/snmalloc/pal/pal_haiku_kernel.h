// BSD/snmalloc-main/src/snmalloc/pal/pal_haiku_kernel.h
#pragma once

// This guard ensures this PAL is only included when building for Haiku Kernel
#if defined(__HAIKU__) && defined(_KERNEL_MODE)

#include "../aal/aal.h"
#include "pal_consts.h"
#include "pal_ds.h" // For PalNotificationObject if used later
#include "pal_timer_default.h"

// Haiku Kernel Headers
#include <KernelExport.h>
#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/VMAddressSpace.h>
#include <vm/VMTranslationMap.h>
#include <vm/VMCache.h>
#include <vm/VMArea.h>
#include <vm/vm_priv.h>      // For vm_page_init_reservation, vm_unreserve_memory etc.
#include <string.h> // For memset, memcpy for kernel
#include <debug.h>  // For ASSERT, panic, dprintf
#include <kernel.h> // For create_area_etc, delete_area, area_id, system_time, thread_get_current_thread_id
#include <slab/Slab.h> // For object_cache_alloc, if used for tracking structs
#include <kernel/thread.h> // For spinlock, disable_interrupts, etc.


// Forward declaration
struct PageRunInfo; // If used by HaikuKernelSubMapping or other PAL structs

// Structure to track free extents of virtual address space within the main VMArena.
struct VAExtent {
    addr_t base;        // Base address of the free VA extent
    size_t size;        // Size of the free VA extent
    VAExtent* next_free; // Pointer to the next VAExtent in a singly linked list (e.g. free list or pool list)
};

// Maximum number of VAExtent structures we can statically allocate.
// This limits the degree of VA fragmentation the PAL can handle.
// Each allocation or deallocation that splits/coalesces VA ranges might consume/release these.
#define SNMALLOC_PAL_VA_EXTENT_POOL_SIZE 256 // Arbitrary, adjust based on expected fragmentation

// Static storage for VAExtent structures.
static VAExtent g_pal_va_extent_pool[SNMALLOC_PAL_VA_EXTENT_POOL_SIZE]; // Renamed from g_pal_va_extent_pool_storage
// Head of the free list for VAExtent structures.
static VAExtent* g_pal_va_extent_pool_free_list = nullptr;
// Spinlock to protect the VAExtent pool.
static spinlock g_pal_va_extent_pool_lock = B_SPINLOCK_INITIALIZER; // Already present, ensure matches

// Head of the actual VA free list (extents available for allocation)
static VAExtent* g_snmalloc_va_free_list_head = nullptr;
// Spinlock for the VA free list (g_snmalloc_va_free_list_head)
// static spinlock g_snmalloc_va_list_lock = B_SPINLOCK_INITIALIZER; // Already present and initialized


// --- PAL Globals for managing the dedicated VMArena ---

/** The VMArea structure representing snmalloc's dedicated kernel heap space. */
static VMArea* g_snmalloc_kernel_vm_area = nullptr;
/** The VMCache associated with g_snmalloc_kernel_vm_area. Used for page operations. */
static VMCache* g_snmalloc_kernel_vm_cache = nullptr;
/** The area_id for snmalloc's dedicated kernel heap space. */
static area_id g_snmalloc_kernel_area_id = -1;
/** Spinlock protecting the initialization and state of the global PAL resources above. */
static spinlock g_snmalloc_pal_lock = B_SPINLOCK_INITIALIZER;

/** Initial size for snmalloc's kernel VMArena. Can be tuned. */
#define SNMALLOC_KERNEL_ARENA_INITIAL_SIZE (64 * 1024 * 1024) // 64MB

/**
 * @brief Manages virtual address space within the dedicated snmalloc kernel VMArena.
 *
 * TODO: This is a critical component that needs a robust implementation.
 * The current PAL uses a simplistic bump pointer which does not allow VA reuse
 * upon deallocation, leading to VA fragmentation within the arena.
 * A production implementation should use a more sophisticated VA management
 * strategy (e.g., a free list of VA extents, a bitmap, or a buddy allocator for VA ranges).
 */
static addr_t g_snmalloc_kernel_va_next_fit = 0;
/** Spinlock protecting the virtual address space allocator for the snmalloc arena. */
static spinlock g_snmalloc_va_lock = B_SPINLOCK_INITIALIZER;

/**
 * @struct HaikuKernelSubMapping
 * @brief Tracks an individual contiguous block of physical pages mapped into
 *        the snmalloc kernel VMArena.
 *
 * This structure is used by the PAL to remember the details of allocations
 * it has made from the VM subsystem, allowing it to correctly unmap and free
 * these resources later.
 *
 * Instances of this struct are themselves allocated from a global kernel
 * object cache (gGenericObjectCache), which creates a dependency. For a
 * fully self-contained snmalloc, these tracking structures might be allocated
 * from a small, statically sized pool managed by the PAL itself, or using
 * snmalloc's bootstrap capabilities once it's minimally functional.
 */
struct HaikuKernelSubMapping {
  void* virtual_address;      ///< The kernel virtual address where the memory is mapped.
  size_t size_in_bytes;       ///< The total size of this mapped region.
  page_num_t num_pages;       ///< The number of physical pages backing this region.
  vm_page* first_page_struct; ///< Pointer to the vm_page structure for the first physical page.
                              ///< Assumes pages were allocated as a contiguous run via vm_page_allocate_page_run.
  HaikuKernelSubMapping* next; ///< Pointer for linked list of active mappings OR next free in pool.
};
/** Head of the linked list for active sub-mappings. */
static HaikuKernelSubMapping* s_kernel_mapping_list = nullptr;
/** Spinlock protecting s_kernel_mapping_list. */
static spinlock s_kernel_mapping_list_lock = B_SPINLOCK_INITIALIZER;

// --- PAL Internal Pool for HaikuKernelSubMapping structs ---
#define SNMALLOC_PAL_MAPPING_POOL_SIZE 256 // Max concurrent tracked mappings by PAL directly
static HaikuKernelSubMapping g_pal_mapping_pool[SNMALLOC_PAL_MAPPING_POOL_SIZE];
static HaikuKernelSubMapping* g_pal_mapping_pool_free_list = nullptr;
static spinlock g_pal_mapping_pool_lock = B_SPINLOCK_INITIALIZER;


namespace snmalloc
{
  /**
   * @class PALHaikuKernel
   * @brief Platform Abstraction Layer (PAL) for using snmalloc within the Haiku kernel.
   *
   * This PAL interfaces with Haiku's kernel Virtual Memory (VM) subsystem to
   * reserve, commit, and decommit memory. It operates by:
   * 1. Creating a large, dedicated, initially uncommitted Virtual Memory Area (VMArea)
   *    during `StaticInit()`. This VMArea serves as snmalloc's primary heap space.
   * 2. When snmalloc's backend requests memory (via `reserve` or `reserve_aligned`),
   *    this PAL:
   *    a. Allocates a virtual address (VA) range from the dedicated VMArea
   *       (currently using a simple bump pointer; TODO: implement robust VA management).
   *    b. Reserves physical memory accounting (`vm_try_reserve_memory`).
   *    c. Reserves `vm_page` structures (`vm_page_reserve_pages`).
   *    d. Allocates a contiguous run of physical pages (`vm_page_allocate_page_run`).
   *    e. Maps these physical pages into the allocated VA range within the dedicated VMArea,
   *       associating them with the VMArea's `VMCache`.
   * 3. When snmalloc no longer needs a memory range (via `notify_not_using`), this PAL:
   *    a. Unmaps the VA range.
   *    b. Removes the `vm_page`s from the VMArea's `VMCache`.
   *    c. Frees the `vm_page` structures back to the system (`vm_page_free_etc`).
   *    d. Deallocates the VA range (returning it to the VMArena's VA manager).
   *    e. Updates physical memory accounting (`vm_unreserve_memory`).
   *
   * It uses Haiku kernel spinlocks for synchronization of its internal state.
   * This PAL aims to replace Haiku's existing slab allocator.
   */
  class PALHaikuKernel : public PalTimerDefaultImpl<PALHaikuKernel>
  {
  public:
    /**
     * @see PalFeatures for descriptions of these flags.
     * - AlignedAllocation: Provided, as memory is at least page-aligned.
     * - Entropy: A placeholder is used; requires a proper kernel RNG source.
     * - Time: Provided by PalTimerDefaultImpl using kernel timers.
     * - Print: Uses kernel dprintf/panic.
     * - LazyCommit is NOT claimed because `reserve_logic` returns committed, mapped memory.
     *   The underlying VMArena itself might be lazy initially, but snmalloc gets usable pages.
     */
    static constexpr uint64_t pal_features =
      AlignedAllocation | Entropy | Time | Print;

    /** Haiku kernel's page size. */
    static constexpr size_t page_size = B_PAGE_SIZE;
    /** Architecture's address bit width, from AAL. */
    static constexpr size_t address_bits = Aal::address_bits;
    /** Minimum allocation size this PAL will deal with from snmalloc's backend. */
    static constexpr size_t minimum_alloc_size = page_size;

  private:
    /**
     * @brief Initializes the static pool of VAExtent structures.
     * Links them all into g_pal_va_extent_pool_free_list.
     * Must be called with g_pal_va_extent_pool_lock held.
     * This function should be called once during PAL initialization (e.g., in StaticInit).
     */
    static void _initialize_va_extent_pool_locked()
    {
        // Assumes g_pal_va_extent_pool_lock is held.
        g_pal_va_extent_pool_free_list = nullptr;
        // Link them in reverse order so the list is 0, 1, ..., N-1
        for (int i = SNMALLOC_PAL_VA_EXTENT_POOL_SIZE - 1; i >= 0; --i) {
            g_pal_va_extent_pool[i].next_free = g_pal_va_extent_pool_free_list;
            g_pal_va_extent_pool_free_list = &g_pal_va_extent_pool[i];
        }
        // dprintf("PALHaikuKernel: _initialize_va_extent_pool_locked: Initialized %d VAExtents into free list.\n", SNMALLOC_PAL_VA_EXTENT_POOL_SIZE);
    }

    /**
     * @brief Allocates a VAExtent structure from the PAL's internal static pool.
     * Must be called with g_pal_va_extent_pool_lock held.
     * @return Pointer to an initialized VAExtent struct, or nullptr if the pool is exhausted.
     */
    static VAExtent* _allocate_va_extent_struct_locked()
    {
        // Assumes g_pal_va_extent_pool_lock is held by caller
        VAExtent* extent = g_pal_va_extent_pool_free_list;
        if (extent) {
            g_pal_va_extent_pool_free_list = extent->next_free;
            extent->next_free = nullptr; // Clear it before use
            extent->base = 0;            // Initialize fields
            extent->size = 0;
            // dprintf("PALHaikuKernel: _allocate_va_extent_struct_locked: Allocated VAExtent %p\n", extent);
        } else {
            // Pool exhausted - this is a critical state for this PAL design.
            dprintf("PALHaikuKernel: CRITICAL - VAExtent static pool exhausted!\n");
            // Panic is an option here, as it's a fixed-size pool and exhaustion is serious.
            // panic("PALHaikuKernel: VAExtent static pool exhausted!");
        }
        return extent;
    }

    /**
     * @brief Frees a VAExtent structure back to the PAL's internal static pool.
     * Must be called with g_pal_va_extent_pool_lock held.
     * @param extent Pointer to the VAExtent structure to free. Must not be null.
     */
    static void _free_va_extent_struct_locked(VAExtent* extent)
    {
        // Assumes g_pal_va_extent_pool_lock is held by caller
        if (extent == nullptr) {
             dprintf("PALHaikuKernel: WARNING - _free_va_extent_struct_locked called with nullptr.\n");
             return;
        }
        // dprintf("PALHaikuKernel: _free_va_extent_struct_locked: Freeing VAExtent %p (base: 0x%lx, size: %lu)\n",
        //    extent, extent->base, extent->size);

        // Optionally clear fields for safety, though _allocate will re-initialize.
        // extent->base = 0;
        // extent->size = 0;
        extent->next_free = g_pal_va_extent_pool_free_list;
        g_pal_va_extent_pool_free_list = extent;
    }

    /**
     * @brief Allocates a virtual address range from the dedicated snmalloc VMArena.
     *
     * Implements a first-fit strategy on an address-sorted free list of VA extents.
     * Handles splitting of free extents and requested alignment.
     * The free list (g_snmalloc_va_free_list_head) is sorted by base address.
     *
     * @param req_size Requested size of the VA range (must be page-aligned by caller).
     * @param req_alignment Requested alignment for the base of the VA range (must be power of 2, >= page_size).
     * @return Base virtual address of the allocated range, or 0 (nullptr) on failure.
     */
    static addr_t _allocate_va_range(size_t req_size, size_t req_alignment)
    {
        // Basic assertions: req_size should be > 0 and page aligned. Alignment should be power of 2 and >= page_size.
        SNMALLOC_ASSERT(req_size > 0 && (req_size % page_size) == 0);
        SNMALLOC_ASSERT(req_alignment >= page_size && bits::is_pow2(req_alignment));

        cpu_status lock_state = disable_interrupts(); // Overall interrupt disabling for this operation
        acquire_spinlock(&g_snmalloc_va_list_lock); // Lock for main VA free list

        VAExtent** Pprev_next_ptr = &g_snmalloc_va_free_list_head; // Pointer to the link we might modify (e.g. current->next_free)
        VAExtent* current_free_extent = g_snmalloc_va_free_list_head;
        addr_t allocated_va_base = 0;

        while (current_free_extent != nullptr) {
            addr_t current_extent_base = current_free_extent->base;
            size_t current_extent_size = current_free_extent->size;

            // Calculate the earliest aligned address *within or at the start of* the current_free_extent
            addr_t aligned_block_start = Aal::align_up(current_extent_base, req_alignment);

            // Check if aligned_block_start is actually usable within current_free_extent
            if (aligned_block_start < current_extent_base) { // Should not happen with align_up if current_extent_base is reasonable
                // This case implies aligned_block_start wrapped around or req_alignment is excessively large.
                // Or current_extent_base is very high. For kernel addresses, this is unlikely.
                Pprev_next_ptr = &current_free_extent->next_free;
                current_free_extent = current_free_extent->next_free;
                continue;
            }

            size_t prefix_padding = aligned_block_start - current_extent_base;

            // Check if there's enough space *after* alignment padding
            if (current_extent_size >= req_size + prefix_padding) { // Found a suitable extent
                allocated_va_base = aligned_block_start;

                // This extent 'current_free_extent' will be used (and possibly split).
                // It needs to be removed from the free list.
                *Pprev_next_ptr = current_free_extent->next_free; // Unlink current_free_extent from list

                // The current_free_extent struct itself will be returned to the pool.
                // New VAExtent structs will be allocated for prefix and suffix if they exist.

                // Hold the pool lock while manipulating VAExtent structs
                cpu_status pool_lock_interrupt_state = disable_interrupts(); // Potentially redundant if outer lock_state suffices
                acquire_spinlock(&g_pal_va_extent_pool_lock);

                // Handle prefix (space before the aligned_block_start)
                if (prefix_padding > 0) {
                    VAExtent* prefix_extent = _allocate_va_extent_struct_locked();
                    if (prefix_extent) {
                        prefix_extent->base = current_extent_base;
                        prefix_extent->size = prefix_padding;
                        // Insert prefix back into the sorted list. *Pprev_next_ptr now points to
                        // current_free_extent's original next. The prefix comes before that.
                        prefix_extent->next_free = *Pprev_next_ptr;
                        *Pprev_next_ptr = prefix_extent;
                        Pprev_next_ptr = &prefix_extent->next_free; // Important: Pprev_next_ptr now points to prefix_extent->next_free
                                                                    // for correct suffix insertion.
                    } else {
                        dprintf("PALHaikuKernel: _allocate_va_range: No VAExtent struct for prefix! VA space [0x%lx, size %lu] lost.\n", current_extent_base, prefix_padding);
                        // This is bad, VA space is lost. Consider panic or more robust handling.
                    }
                }

                // Handle suffix (space after the allocated block)
                addr_t suffix_start = aligned_block_start + req_size;
                // Calculate suffix_len carefully to avoid underflow if suffix_start is past extent end
                size_t suffix_len = 0;
                if (suffix_start < current_extent_base + current_extent_size) {
                     suffix_len = (current_extent_base + current_extent_size) - suffix_start;
                }


                if (suffix_len > 0) {
                    VAExtent* suffix_extent = _allocate_va_extent_struct_locked();
                    if (suffix_extent) {
                        suffix_extent->base = suffix_start;
                        suffix_extent->size = suffix_len;
                        // Insert suffix. Pprev_next_ptr points to the link where suffix should be inserted.
                        // This link is either current_free_extent's original next (if no prefix)
                        // or prefix_extent->next_free (if prefix was inserted).
                        suffix_extent->next_free = *Pprev_next_ptr;
                        *Pprev_next_ptr = suffix_extent;
                    } else {
                         dprintf("PALHaikuKernel: _allocate_va_range: No VAExtent struct for suffix! VA space [0x%lx, size %lu] lost.\n", suffix_start, suffix_len);
                         // Bad, VA space lost.
                    }
                }

                // The original current_free_extent struct is now fully processed (split into prefix/suffix if needed).
                // Its constituent parts (if any) are back in the free list. So, the struct itself can be freed.
                _free_va_extent_struct_locked(current_free_extent);

                release_spinlock(&g_pal_va_extent_pool_lock);
                restore_interrupts(pool_lock_interrupt_state);
                goto found_va_range_exit; // Exit loop, VA list lock will be released at label
            }

            // If not suitable, move to the next extent in the free list
            Pprev_next_ptr = &current_free_extent->next_free;
            current_free_extent = current_free_extent->next_free;
        }

    found_va_range_exit: // Label to jump to for releasing the VA list lock and restoring interrupts
        release_spinlock(&g_snmalloc_va_list_lock);
        restore_interrupts(lock_state); // Restore original interrupt state

        if (allocated_va_base == 0) {
            dprintf("PALHaikuKernel: _allocate_va_range FAILED to find/allocate VA block for size %lu, align %lu\n", req_size, req_alignment);
            // TODO: Consider VMArena expansion logic here if appropriate for the PAL design.
        } else {
            // dprintf("PALHaikuKernel: _allocate_va_range allocated %p, size %lu, align %lu\n", (void*)allocated_va_base, req_size, req_alignment);
        }
        return allocated_va_base;
    }

    /**
     * @brief Frees a virtual address range back to the PAL's VA manager.
     *
     * Inserts the freed range [base, base + size) into the address-sorted
     * free list (g_snmalloc_va_free_list_head) and attempts to coalesce
     * with adjacent free blocks.
     *
     * @param base Base address of the VA range to free (page-aligned).
     * @param size Size of the VA range to free (page-aligned).
     */
    static void _free_va_range(addr_t base, size_t size)
    {
        if (base == 0 || size == 0) {
            dprintf("PALHaikuKernel: _free_va_range called with base 0 or size 0. Base: %p, Size: %lu\n", (void*)base, size);
            return;
        }
        SNMALLOC_ASSERT((base % page_size) == 0 && (size % page_size) == 0); // Should be page aligned by caller

        cpu_status lock_state = disable_interrupts(); // Overall interrupt disabling

        // 1. Allocate a VAExtent struct for the range being freed.
        cpu_status pool_lock_interrupt_state = disable_interrupts();
        acquire_spinlock(&g_pal_va_extent_pool_lock);
        VAExtent* new_free_extent = _allocate_va_extent_struct_locked();
        release_spinlock(&g_pal_va_extent_pool_lock);
        restore_interrupts(pool_lock_interrupt_state);

        if (!new_free_extent) {
            restore_interrupts(lock_state); // Restore outer interrupt state before panic
            panic("PALHaikuKernel: _free_va_range: No VAExtent struct available to track freed VA range! VA LEAK: base %p, size %zu", (void*)base, size);
            return; // Unreachable
        }
        new_free_extent->base = base;
        new_free_extent->size = size;
        new_free_extent->next_free = nullptr;

        // dprintf("PALHaikuKernel: _free_va_range attempting to free VA: base %p, size %lu\n", (void*)base, size);

        // 2. Insert into address-sorted free list and attempt coalescing.
        acquire_spinlock(&g_snmalloc_va_list_lock);

        VAExtent** Pprev_next_ptr = &g_snmalloc_va_free_list_head;
        VAExtent* current_list_iter = g_snmalloc_va_free_list_head;

        // Find insertion point (Pprev_next_ptr will point to the link that should point to new_free_extent)
        while (current_list_iter != nullptr && current_list_iter->base < new_free_extent->base) {
            Pprev_next_ptr = &current_list_iter->next_free;
            current_list_iter = current_list_iter->next_free;
        }

        // Insert new_free_extent into the list
        new_free_extent->next_free = current_list_iter; // current_list_iter is the block after new_free_extent, or nullptr
        *Pprev_next_ptr = new_free_extent;

        // Attempt to coalesce new_free_extent with next block (current_list_iter)
        if (new_free_extent->next_free != nullptr && // Is there a next block?
            (new_free_extent->base + new_free_extent->size == new_free_extent->next_free->base)) { // Are they contiguous?
            // dprintf("PALHaikuKernel: _free_va_range coalescing new_free_extent (%p, sz %lu) with NEXT (%p, sz %lu)\n",
            //     (void*)new_free_extent->base, new_free_extent->size,
            //     (void*)new_free_extent->next_free->base, new_free_extent->next_free->size);

            VAExtent* next_block_to_merge = new_free_extent->next_free;
            new_free_extent->size += next_block_to_merge->size;          // Absorb size
            new_free_extent->next_free = next_block_to_merge->next_free; // Bypass next_block_to_merge

            // Free the VAExtent struct of the merged next_block_to_merge
            cpu_status inner_pool_lock_state = disable_interrupts();
            acquire_spinlock(&g_pal_va_extent_pool_lock);
            _free_va_extent_struct_locked(next_block_to_merge);
            release_spinlock(&g_pal_va_extent_pool_lock);
            restore_interrupts(inner_pool_lock_state);
        }

        // Attempt to coalesce new_free_extent with previous block.
        // Pprev_next_ptr points to the link that now points to new_free_extent.
        // The previous block is `container_of(Pprev_next_ptr, VAExtent, next_free)` but that's tricky.
        // Easier: if Pprev_next_ptr is not &g_snmalloc_va_free_list_head, then there's a previous.
        // The block pointed to by Pprev_next_ptr is new_free_extent.
        // We need the block *before* new_free_extent. Let's find it.
        // Note: if new_free_extent was merged with next, its size and next_free are already updated.

        // To find the block *before* new_free_extent (which is now pointed to by *Pprev_next_ptr):
        // If *Pprev_next_ptr == new_free_extent, and Pprev_next_ptr != &g_snmalloc_va_free_list_head,
        // then the "previous block" is the one whose `next_free` field's address is `Pprev_next_ptr`.
        // This requires a slightly different iteration logic or passing the actual previous block.
        // Let's re-scan from head to find previous for simplicity here, though less efficient.

        if (Pprev_next_ptr != &g_snmalloc_va_free_list_head) { // Check if new_free_extent is not the head
            VAExtent* prev_block_iter = g_snmalloc_va_free_list_head;
            VAExtent* block_before_new_free = nullptr;
            while(prev_block_iter != nullptr && prev_block_iter->next_free != new_free_extent) {
                prev_block_iter = prev_block_iter->next_free;
            }
            block_before_new_free = prev_block_iter; // This is the block whose ->next_free is new_free_extent

            if (block_before_new_free != nullptr && // Found a block before new_free_extent
                (block_before_new_free->base + block_before_new_free->size == new_free_extent->base)) { // Are they contiguous?
                // dprintf("PALHaikuKernel: _free_va_range coalescing PREVIOUS (%p, sz %lu) with new_free_extent (%p, sz %lu)\n",
                //     (void*)block_before_new_free->base, block_before_new_free->size,
                //     (void*)new_free_extent->base, new_free_extent->size);

                block_before_new_free->size += new_free_extent->size;             // Absorb size into previous
                block_before_new_free->next_free = new_free_extent->next_free;    // Previous now points to new_free_extent's next

                // Free the VAExtent struct of new_free_extent (which is now merged into previous)
                cpu_status inner_pool_lock_state = disable_interrupts();
                acquire_spinlock(&g_pal_va_extent_pool_lock);
                _free_va_extent_struct_locked(new_free_extent);
                release_spinlock(&g_pal_va_extent_pool_lock);
                restore_interrupts(inner_pool_lock_state);
                // new_free_extent is now gone, prev_block_iter is the merged block.
            }
        }

        release_spinlock(&g_snmalloc_va_list_lock);
        restore_interrupts(lock_state); // Restore original interrupt state
        // dprintf("PALHaikuKernel: _free_va_range completed for VA: base %p, size %lu\n", (void*)base, size);
    }

  public: // Public PAL API methods follow
    /**
     * @brief Reports a fatal error and panics the kernel.
     * @param str Error message.
     */
    [[noreturn]] static void error(const char* const str) noexcept
    {
      panic("snmalloc PALHaikuKernel FATAL ERROR: %s", str);
    }

    /**
     * @brief Prints a message to the kernel debug log.
     * @param str Message to print.
     */
    static void message(const char* const str) noexcept
    {
      dprintf("snmalloc PALHaikuKernel: %s\n", str);
    }

    /**
     * @brief Initializes static resources for the PAL.
     *
     * This function should be called once during kernel startup (e.g., from `kmalloc_init`).
     * It creates the dedicated VMArea for snmalloc's heap.
     *
     * @return B_OK on success, or an error code if initialization fails.
     */
    static status_t StaticInit() {
        cpu_status state = disable_interrupts();
        acquire_spinlock(&g_snmalloc_pal_lock);

        if (g_snmalloc_kernel_area_id >= B_OK) {
            release_spinlock(&g_snmalloc_pal_lock);
            restore_interrupts(state);
            return B_OK; // Already initialized
        }

        void* arena_base = nullptr;
        // Create the VMArea. VM_AREA_FLAG_NULL_WIRED means it's initially just VA space.
        g_snmalloc_kernel_area_id = create_area_etc(
            VMAddressSpace::KernelID(),           // Owner: Kernel
            "snmalloc_kernel_heap_arena",         // Name
            SNMALLOC_KERNEL_ARENA_INITIAL_SIZE,   // Initial size
            B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, // Protections
            CREATE_AREA_DONT_WAIT | VM_AREA_FLAG_NULL_WIRED, // Flags: non-blocking, initially uncommitted
            0,                                    // No guard pages for the VMArea itself
            0,                                    // Locking: B_NO_LOCK on the area, PAL handles sync
            nullptr,                              // virtual_address_restrictions
            nullptr,                              // physical_address_restrictions
            &arena_base                           // Output for base address
        );

        if (g_snmalloc_kernel_area_id < B_OK) {
            release_spinlock(&g_snmalloc_pal_lock);
            restore_interrupts(state);
            panic("PALHaikuKernel: Failed to create snmalloc kernel VMArena! Error: %s", strerror(g_snmalloc_kernel_area_id));
            return g_snmalloc_kernel_area_id; // Should be unreachable
        }

        g_snmalloc_kernel_vm_area = VMAreas::Lookup(g_snmalloc_kernel_area_id);
        if (g_snmalloc_kernel_vm_area == nullptr) {
            // This should ideally not happen if create_area_etc succeeded.
            delete_area(g_snmalloc_kernel_area_id); // Attempt cleanup
            g_snmalloc_kernel_area_id = -1;
            release_spinlock(&g_snmalloc_pal_lock);
            restore_interrupts(state);
            panic("PALHaikuKernel: Could not look up created VMArena (id %" B_PRId32 ")!", g_snmalloc_kernel_area_id);
            return B_ERROR; // Should be unreachable
        }
        // Get the VMCache associated with the area. vm_area_get_locked_cache locks the cache.
        g_snmalloc_kernel_vm_cache = vm_area_get_locked_cache(g_snmalloc_kernel_vm_area);
        g_snmalloc_kernel_vm_area->cache->Unlock(); // Unlock it as we don't need it locked globally.

        // Initialize the internal pool for HaikuKernelSubMapping structures
        acquire_spinlock(&g_pal_mapping_pool_lock);
        g_pal_mapping_pool_free_list = nullptr;
        for (int i = SNMALLOC_PAL_MAPPING_POOL_SIZE - 1; i >= 0; --i) { // Add in reverse to get 0..N order
            g_pal_mapping_pool[i].next = g_pal_mapping_pool_free_list;
            g_pal_mapping_pool_free_list = &g_pal_mapping_pool[i];
        }
        release_spinlock(&g_pal_mapping_pool_lock);

        // Initialize the VA Extent structure pool using the helper function
        acquire_spinlock(&g_pal_va_extent_pool_lock);
        _initialize_va_extent_pool_locked(); // This sets up g_pal_va_extent_pool_free_list
        // At this point, g_pal_va_extent_pool_lock is still held.

        // Now, allocate the first VAExtent for the entire arena from this pool.
        VAExtent* initial_extent = _allocate_va_extent_struct_locked(); // Still under g_pal_va_extent_pool_lock
        release_spinlock(&g_pal_va_extent_pool_lock); // Release pool lock once struct is allocated or not

        if (!initial_extent) {
            // Failed to get a VAExtent struct. This is critical.
            // No need to acquire g_snmalloc_va_list_lock if we can't even get a struct.
            delete_area(g_snmalloc_kernel_area_id); // Attempt to clean up the created VMArena
            g_snmalloc_kernel_area_id = -1;
            // g_snmalloc_pal_lock is held, release it before panic
            release_spinlock(&g_snmalloc_pal_lock);
            restore_interrupts(state);
            panic("PALHaikuKernel: StaticInit failed to allocate initial VAExtent struct from pool!");
            return B_NO_MEMORY; // Should be unreachable due to panic
        }

        // Configure the initial extent to cover the whole arena
        initial_extent->base = (addr_t)arena_base;
        initial_extent->size = SNMALLOC_KERNEL_ARENA_INITIAL_SIZE;
        initial_extent->next_free = nullptr;

        // Add this initial extent to the main VA free list
        // g_snmalloc_va_list_lock is already initialized statically
        acquire_spinlock(&g_snmalloc_va_list_lock);
        g_snmalloc_va_free_list_head = initial_extent;
        release_spinlock(&g_snmalloc_va_list_lock);

        dprintf("PALHaikuKernel: StaticInit created VMArena %" B_PRId32 " at %p, size %lu\n",
            g_snmalloc_kernel_area_id, arena_base, (unsigned long)SNMALLOC_KERNEL_ARENA_INITIAL_SIZE);
        dprintf("PALHaikuKernel: Initialized internal pool for %d HaikuKernelSubMapping structs.\n",
            SNMALLOC_PAL_MAPPING_POOL_SIZE);
        dprintf("PALHaikuKernel: Initialized internal pool for %d VAExtent structs (using _initialize_va_extent_pool_locked).\n",
            SNMALLOC_PAL_VA_EXTENT_POOL_SIZE);
        dprintf("PALHaikuKernel: Initialized VA free list (g_snmalloc_va_free_list_head) with initial extent: base %p, size %lu\n",
            (void*)initial_extent->base, initial_extent->size);

        release_spinlock(&g_snmalloc_pal_lock);
        restore_interrupts(state);
        return B_OK;
    }

    /**
     * @brief Tears down static resources used by the PAL.
     *
     * This function might be called during kernel shutdown, if such a phase exists
     * and requires explicit cleanup of the allocator's resources.
     */
    static void StaticTeardown() {
        cpu_status state = disable_interrupts();
        acquire_spinlock(&g_snmalloc_pal_lock);

        if (g_snmalloc_kernel_area_id >= B_OK) {
            if (s_kernel_mapping_list != nullptr) {
                // This indicates a leak, as all memory should have been freed by snmalloc
                // before its PAL is torn down.
                dprintf("PALHaikuKernel: Warning: StaticTeardown called with outstanding sub-mappings. Memory leak likely.\n");
                // TODO: Optionally iterate s_kernel_mapping_list and attempt to free remaining blocks.
                // This is risky at shutdown and implies prior incorrect behavior.
            }
            delete_area(g_snmalloc_kernel_area_id);
            g_snmalloc_kernel_area_id = -1;
            g_snmalloc_kernel_vm_area = nullptr;
            g_snmalloc_kernel_vm_cache = nullptr;
            dprintf("PALHaikuKernel: StaticTeardown deleted VMArena.\n");
        }

        // Return all VAExtents from g_snmalloc_va_free_list_head to the g_pal_va_extent_pool.
        // This ensures all VAExtent structs are back in the pool before we potentially
        // re-initialize it or declare it fully "cleared".
        cpu_status va_list_lock_state = disable_interrupts(); // Use separate interrupt state for this section
        acquire_spinlock(&g_snmalloc_va_list_lock);
        VAExtent* current_va_block = g_snmalloc_va_free_list_head;
        g_snmalloc_va_free_list_head = nullptr; // Detach the list
        release_spinlock(&g_snmalloc_va_list_lock);
        restore_interrupts(va_list_lock_state);

        if (current_va_block != nullptr) {
            // dprintf("PALHaikuKernel: StaticTeardown returning VAExtents from g_snmalloc_va_free_list_head to pool.\n");
            cpu_status va_pool_lock_state = disable_interrupts();
            acquire_spinlock(&g_pal_va_extent_pool_lock);
            while (current_va_block != nullptr) {
                VAExtent* next_block = current_va_block->next_free; // Save next pointer
                _free_va_extent_struct_locked(current_va_block);    // Return current block to pool
                current_va_block = next_block;                      // Move to next
            }
            release_spinlock(&g_pal_va_extent_pool_lock);
            restore_interrupts(va_pool_lock_state);
        }

        // Clear mapping pool free list (pool itself is static, just reset the list pointer)
        acquire_spinlock(&g_pal_mapping_pool_lock);
        g_pal_mapping_pool_free_list = nullptr;
        // Note: Individual structs in g_pal_mapping_pool don't need explicit freeing here
        // as they are part of a static global array. Resetting the free list is enough.
        release_spinlock(&g_pal_mapping_pool_lock);

        // Clear VA Extent struct pool free list
        acquire_spinlock(&g_pal_va_extent_pool_lock);
        g_pal_va_extent_pool_free_list = nullptr; // Pool is static, just reset list
        release_spinlock(&g_pal_va_extent_pool_lock);


        release_spinlock(&g_snmalloc_pal_lock);
        restore_interrupts(state);
    }

    /**
     * @brief Informs the PAL that a previously reserved memory range is no longer needed.
     *
     * This function will unmap the virtual address range `p` of `size` bytes
     * from the dedicated snmalloc VMArea, and free the underlying physical pages
     * back to the system. It also updates memory accounting.
     *
     * @param p The base virtual address of the range to decommit.
     * @param size The size of the range in bytes. Must match the original reservation size.
     */
    static void notify_not_using(void* p, size_t size) noexcept
    {
      if (p == nullptr || size == 0) return;
      SNMALLOC_ASSERT(is_aligned_block<page_size>(p, size)); // Ensure page alignment.

      // Find and remove the tracking structure for this mapping.
      cpu_status lock_st = disable_interrupts();
      acquire_spinlock(&s_kernel_mapping_list_lock);
      HaikuKernelSubMapping** current_ptr = &s_kernel_mapping_list;
      HaikuKernelSubMapping* mapping_to_free = nullptr;
      while (*current_ptr != nullptr) {
        if ((*current_ptr)->virtual_address == p) {
          mapping_to_free = *current_ptr;
          if (mapping_to_free->size_in_bytes != size) {
            // This PAL currently expects full decommitment of originally reserved blocks.
            // Partial decommits would require more complex tracking and VA management.
            release_spinlock(&s_kernel_mapping_list_lock);
            restore_interrupts(lock_st);
            panic("PALHaikuKernel: notify_not_using size mismatch for %p. Expected %zu, got %zu. Partial decommit not supported by this simple PAL.",
                p, mapping_to_free->size_in_bytes, size);
            return;
          }
          *current_ptr = mapping_to_free->next; // Remove from list
          break;
        }
        current_ptr = &(*current_ptr)->next;
      }
      release_spinlock(&s_kernel_mapping_list_lock);
      restore_interrupts(lock_st);

      if (mapping_to_free == nullptr) {
        // This can happen if snmalloc tries to free a sub-region or an address not from this PAL.
        dprintf("PALHaikuKernel: notify_not_using called on unknown address %p or unaligned/sub-region request.\n", p);
        return;
      }

      SNMALLOC_ASSERT(g_snmalloc_kernel_vm_area != nullptr && g_snmalloc_kernel_vm_cache != nullptr);

      // 1. Unmap the virtual memory region from the kernel address space.
      VMTranslationMap* trans_map = VMAddressSpace::Kernel()->TranslationMap();
      trans_map->Lock();
      trans_map->Unmap((addr_t)p, (addr_t)p + size - 1);
      atomic_add(&gMappedPagesCount, -(ssize_t)mapping_to_free->num_pages); // Global Haiku counter
      trans_map->Unlock();

      // 2. Disassociate vm_pages from the VMArea's cache and free them.
      vm_page_reservation reservation; // Used by vm_page_free_etc
      vm_page_init_reservation(&reservation);

      vm_page* current_physical_page_tracker = mapping_to_free->first_page_struct;
      addr_t current_area_offset = (addr_t)p - g_snmalloc_kernel_vm_area->Base();

      for (page_num_t i = 0; i < mapping_to_free->num_pages; ++i) {
        // We must look up the vm_page again using its physical number because the
        // first_page_struct pointer might point to a vm_page structure that itself
        // could be paged out or moved if it wasn't wired (though for kernel heap it should be).
        // A safer way is to iterate based on physical page numbers if they are contiguous.
        vm_page* page_to_free = vm_lookup_page(current_physical_page_tracker->physical_page_number + i);
        if (!page_to_free) {
            // This would be a very serious error, meaning a page we thought we had is gone.
            panic("PALHaikuKernel: notify_not_using - vm_page lookup failed for phys page # %" B_PRIuPHYSADDR,
                  current_physical_page_tracker->physical_page_number + i);
            continue; // Or handle error more gracefully if possible
        }
        DEBUG_PAGE_ACCESS_START(page_to_free);
        // Remove the page from the VMArea's VMCache.
        g_snmalloc_kernel_vm_cache->RemovePage(page_to_free);
        // Free the vm_page structure back to the system.
        vm_page_free_etc(g_snmalloc_kernel_vm_cache, page_to_free, &reservation);
        DEBUG_PAGE_ACCESS_END(page_to_free);
        current_area_offset += page_size;
      }
      vm_page_unreserve_pages(&reservation); // Finalize reservation accounting for freed pages.

      // 3. Update kernel's global memory accounting.
      vm_unreserve_memory(mapping_to_free->size_in_bytes);

      // 4. Return the Virtual Address range to the PAL's VA manager.
      // TODO: Implement this. With the current bump_pointer VA allocator, this is a no-op,
      // leading to VA fragmentation within the snmalloc VMArena.
      // A proper VA manager (e.g., free list based) would take back this VA range:
      _free_va_range((addr_t)p, size);
      // dprintf("PALHaikuKernel: VA range %p - %p (%zu bytes) returned to VA manager.\n",
      //    p, (char*)p + size, size);


      // 5. Free the tracking structure itself.
      // This HaikuKernelSubMapping struct was allocated from our internal pool.
      cpu_status pool_lock_st = disable_interrupts();
      acquire_spinlock(&g_pal_mapping_pool_lock);
      mapping_to_free->next = g_pal_mapping_pool_free_list;
      g_pal_mapping_pool_free_list = mapping_to_free;
      release_spinlock(&g_pal_mapping_pool_lock);
      restore_interrupts(pool_lock_st);
      // dprintf("PALHaikuKernel: Returned HaikuKernelSubMapping %p to internal pool.\n", mapping_to_free);
    }

    /**
     * @brief Informs the PAL that a reserved memory range will now be used.
     * If `zero_mem` is `YesZero`, the memory must be zeroed by this call.
     *
     * For this PAL, `reserve_logic` already returns committed and (if requested via
     * `VM_PAGE_ALLOC_WIRED_CLEAR`) zeroed memory. So this is mainly for explicit zeroing.
     *
     * @tparam zero_mem Enum to indicate if memory should be zeroed.
     * @param p Base virtual address of the range.
     * @param size Size of the range in bytes.
     */
    template<ZeroMem zero_mem>
    static void notify_using(void* p, size_t size) noexcept
    {
      SNMALLOC_ASSERT(is_aligned_block<page_size>(p, size));
      if constexpr (zero_mem == YesZero) {
        PALHaikuKernel::zero<true>(p, size);
      }
      // No other action needed as reserve_logic provides ready-to-use memory.
    }

    /**
     * @brief Zeroes a region of memory.
     * @tparam page_aligned Hint if the memory region is page-aligned (not used by this impl).
     * @param p Pointer to the memory region.
     * @param size Size of the region in bytes.
     */
    template<bool page_aligned = false>
    static void zero(void* p, size_t size) noexcept
    {
      memset(p, 0, size);
    }

    /**
     * @brief Core logic for reserving and mapping memory from the kernel.
     *
     * Allocates physical pages, maps them into the dedicated snmalloc VMArena,
     * and returns a pointer to the mapped virtual address.
     *
     * @tparam state_using_unused If true, allocates pages as WIRED_CLEAR (committed and zeroed).
     *                            If false, allocates as RESERVED (placeholder, not yet backed).
     *                            Currently, always aims for WIRED_CLEAR.
     * @param size The minimum size of the memory region to reserve. Will be aligned up to page_size.
     * @param alignment_request Requested alignment for the virtual address. Must be power of 2.
     * @return Pointer to the reserved and mapped kernel virtual memory, or nullptr on failure.
     */
    template<bool state_using_unused = true>
    static void* reserve_logic(size_t size, size_t alignment_request) noexcept
    {
      if (g_snmalloc_kernel_area_id < B_OK) {
        // Attempt lazy initialization of the PAL's static resources if not already done.
        // This is a fallback; StaticInit should ideally be called explicitly during kernel boot.
        if (StaticInit() != B_OK) {
             error("PALHaikuKernel::reserve_logic called before StaticInit() and StaticInit failed!");
             return nullptr; // Should be unreachable due to panic in StaticInit or error
        }
      }
      if (size == 0) return nullptr;

      size_t aligned_size = Aal::align_up(size, page_size);
      if (aligned_size == 0 && size > 0) return nullptr; // Overflow

      page_num_t num_pages = aligned_size / page_size;

      // --- Virtual Address Allocation using _allocate_va_range ---
      addr_t va_to_map_at = _allocate_va_range(aligned_size, alignment_request);
      if (va_to_map_at == 0) {
          // _allocate_va_range already dprintf's on failure
          // dprintf("PALHaikuKernel: reserve_logic failed to get VA range of size %lu, align %lu.\n", aligned_size, alignment_request);
          return nullptr; // VA allocation failed
      }
      // dprintf("PALHaikuKernel: reserve_logic successfully allocated VA: %p, size %lu\n", (void*)va_to_map_at, aligned_size);
      // --- End VA Allocation ---

      // 1. Account for memory reservation
      status_t mem_reserve_status = vm_try_reserve_memory(aligned_size, VM_PRIORITY_SYSTEM, 0);
      if (mem_reserve_status != B_OK) {
        dprintf("PALHaikuKernel: vm_try_reserve_memory failed for %zu bytes. Error: %s\n", aligned_size, strerror(mem_reserve_status));
        _free_va_range(va_to_map_at, aligned_size); // <<< ROLLBACK VA allocation
        return nullptr;
      }

      // 2. Reserve vm_page structures
      vm_page_reservation phys_page_reservation;
      vm_page_init_reservation(&phys_page_reservation);
      vm_page_reserve_pages(&phys_page_reservation, num_pages, VM_PRIORITY_SYSTEM);

      // 3. Allocate a contiguous run of physical pages
      vm_page* first_page_struct = vm_page_allocate_page_run(
          (state_using_unused ? VM_PAGE_ALLOC_WIRED_CLEAR : VM_PAGE_ALLOC_RESERVED), // WIRED_CLEAR provides zeroed pages
          num_pages, &physical_address_restrictions::empty, VM_PRIORITY_SYSTEM);

      if (first_page_struct == nullptr) {
        vm_page_unreserve_pages(&phys_page_reservation); // Release vm_page structure reservation
        vm_unreserve_memory(aligned_size); // Release memory accounting
        dprintf("PALHaikuKernel: vm_page_allocate_page_run failed for %lu pages.\n", num_pages);
        _free_va_range(va_to_map_at, aligned_size); // <<< ROLLBACK VA allocation
        return nullptr;
      }

      // 4. Map these physical pages into the allocated Kernel Virtual Address
      vm_page_reservation map_struct_reservation; // For Map() internal needs
      vm_page_init_reservation(&map_struct_reservation);
      VMTranslationMap* trans_map = VMAddressSpace::Kernel()->TranslationMap();
      trans_map->Lock();

      status_t final_map_status = B_OK;
        page_num_t successfully_mapped_pages = 0;

      for (page_num_t i = 0; i < num_pages; ++i) {
        vm_page* current_vm_page = vm_lookup_page(first_page_struct->physical_page_number + i);
          if (!current_vm_page) {
              final_map_status = B_ERROR; // Should not happen for a contiguous run
              dprintf("PALHaikuKernel: vm_page lookup failed for page %" B_PRIuPHYSADDR " in allocated run during mapping.\n", first_page_struct->physical_page_number + i);
              break; // Exit mapping loop
        }
        DEBUG_PAGE_ACCESS_START(current_vm_page);
        g_snmalloc_kernel_vm_cache->InsertPage(current_vm_page,
            (va_to_map_at + i * page_size) - g_snmalloc_kernel_vm_area->Base());

        status_t map_status = trans_map->Map(
              va_to_map_at + i * page_size,
              current_vm_page->physical_page_number << PAGE_SHIFT,
              B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
              g_snmalloc_kernel_vm_area->memory_type,
              &map_struct_reservation);
        DEBUG_PAGE_ACCESS_END(current_vm_page);

        if (map_status != B_OK) {
            final_map_status = map_status;
              dprintf("PALHaikuKernel: VMTranslationMap::Map failed for page %lu at va %p. Error: %s\n",
                  i, (void*)(va_to_map_at + i * page_size), strerror(map_status));
              break; // Exit mapping loop
        }
          successfully_mapped_pages++;
      }
        // Unlock must happen regardless of loop break reason
      trans_map->Unlock();
        vm_page_unreserve_pages(&map_struct_reservation);

      if (final_map_status != B_OK) {
            dprintf("PALHaikuKernel: Mapping failed. Rolling back. Successfully mapped %lu pages before error.\n", successfully_mapped_pages);
            // Rollback for mapping failure:
            // 1. Unmap any pages that were successfully mapped in this attempt.
            if (successfully_mapped_pages > 0) {
                VMTranslationMap* trans_map_rb = VMAddressSpace::Kernel()->TranslationMap();
                trans_map_rb->Lock();
                // Unmap only the pages that were actually mapped in this call
                trans_map_rb->Unmap(va_to_map_at, va_to_map_at + (successfully_mapped_pages * page_size) - 1);
                atomic_add(&gMappedPagesCount, -(ssize_t)successfully_mapped_pages);
                trans_map_rb->Unlock();
            }

            // 2. Free all vm_page structures from the allocated run (first_page_struct)
            //    and remove them from cache.
            vm_page_reservation page_free_reservation;
            vm_page_init_reservation(&page_free_reservation);
            for (page_num_t i = 0; i < num_pages; ++i) { // Iterate all pages in the run
                vm_page* page_to_free = vm_lookup_page(first_page_struct->physical_page_number + i);
                if (page_to_free) {
                    DEBUG_PAGE_ACCESS_START(page_to_free);
                    if(g_snmalloc_kernel_vm_cache) g_snmalloc_kernel_vm_cache->RemovePage(page_to_free);
                    vm_page_free_etc(g_snmalloc_kernel_vm_cache, page_to_free, &page_free_reservation);
                    DEBUG_PAGE_ACCESS_END(page_to_free);
                }
            }
            vm_page_unreserve_pages(&page_free_reservation);

            // 3. Unreserve the vm_page structures originally reserved for the whole run
            vm_page_unreserve_pages(&phys_page_reservation);

            // 4. Unreserve memory accounting for the entire requested size
          vm_unreserve_memory(aligned_size);

            // 5. Free the VA range
            _free_va_range(va_to_map_at, aligned_size);

          return nullptr;
      }

      // 5. Track the successful mapping for later deallocation.
      cpu_status pool_lock_st = disable_interrupts();
      acquire_spinlock(&g_pal_mapping_pool_lock);
      HaikuKernelSubMapping* tracking_block = g_pal_mapping_pool_free_list;
      if (tracking_block != nullptr) {
          g_pal_mapping_pool_free_list = tracking_block->next;
      }
      release_spinlock(&g_pal_mapping_pool_lock);
      restore_interrupts(pool_lock_st);

      if (!tracking_block) {
          // Static pool exhausted. This is a critical error for this PAL design.
          // A more advanced PAL might try to dynamically allocate more tracking structs
          // or have a larger static pool.
          // panic("PALHaikuKernel: Ran out of internal tracking blocks (HaikuKernelSubMapping pool exhausted) for mapping at va %p.", (void*)va_to_map_at);
          // TODO: FULL ROLLBACK of mapped pages, physical pages (vm_page_free_etc for 'first_page_struct' run),
          // vm_page structure reservation (vm_page_unreserve_pages for 'phys_page_reservation'), and memory accounting.
          dprintf("PALHaikuKernel: CRITICAL - Ran out of HaikuKernelSubMapping pool for mapping at va %p.\n", (void*)va_to_map_at);

          // Attempt more complete rollback for this critical error:
          // This is complex because pages are already mapped.
          // A simplified rollback: unmap, free vm_pages, unreserve memory, free VA.
          // This doesn't use the 'mapping_to_free' path of notify_not_using as no tracking_block was successfully made.

          // 1. Unmap (very simplified, assumes no other users of this VA range yet)
          VMTranslationMap* trans_map_rb = VMAddressSpace::Kernel()->TranslationMap();
          trans_map_rb->Lock();
          trans_map_rb->Unmap(va_to_map_at, va_to_map_at + aligned_size - 1);
          atomic_add(&gMappedPagesCount, -(ssize_t)num_pages);
          trans_map_rb->Unlock();

          // 2. Free vm_page structures and remove from cache
          vm_page_reservation rollback_reservation;
          vm_page_init_reservation(&rollback_reservation);
          for (page_num_t i = 0; i < num_pages; ++i) {
            vm_page* page_to_free = vm_lookup_page(first_page_struct->physical_page_number + i);
            if (page_to_free) {
                DEBUG_PAGE_ACCESS_START(page_to_free);
                if(g_snmalloc_kernel_vm_cache) g_snmalloc_kernel_vm_cache->RemovePage(page_to_free);
                vm_page_free_etc(g_snmalloc_kernel_vm_cache, page_to_free, &rollback_reservation);
                DEBUG_PAGE_ACCESS_END(page_to_free);
            }
          }
          vm_page_unreserve_pages(&rollback_reservation); // Finalize for freed pages
          vm_page_unreserve_pages(&phys_page_reservation); // Unreserve original vm_page structures

          // 3. Unreserve memory accounting
          vm_unreserve_memory(aligned_size);

          // 4. Free VA range
          _free_va_range(va_to_map_at, aligned_size);

          return nullptr; // Allocation failed
      }
      tracking_block->virtual_address = (void*)va_to_map_at;
      tracking_block->size_in_bytes = aligned_size;
      tracking_block->num_pages = num_pages;
      tracking_block->first_page_struct = first_page_struct;

      cpu_status list_lock_st = disable_interrupts();
      acquire_spinlock(&s_kernel_mapping_list_lock);
      tracking_block->next = s_kernel_mapping_list;
      s_kernel_mapping_list = tracking_block;
      release_spinlock(&s_kernel_mapping_list_lock);
      restore_interrupts(list_lock_st);

      // Finalize the reservation for the allocated vm_page structures. They are now "in use".
      vm_page_unreserve_pages(&phys_page_reservation);

      return (void*)va_to_map_at;
    }

    /**
     * @brief Reserves page-aligned memory. Forwards to `reserve_logic`.
     * @param size Minimum size to reserve.
     * @return Pointer to reserved memory, or nullptr on failure.
     */
    template<bool state_using_unused = true>
    static void* reserve(size_t size) noexcept
    {
        return reserve_logic<state_using_unused>(size, page_size);
    }

    /**
     * @brief Reserves memory with a specified alignment.
     * @param size Minimum size to reserve.
     * @param alignment Requested alignment (must be power of 2, >= page_size).
     * @return Pointer to reserved memory, or nullptr on failure.
     */
    template<bool state_using = true>
    static void* reserve_aligned(size_t size, size_t alignment) noexcept
    {
        if (alignment == 0) alignment = 1;
        if (!bits::is_pow2(alignment)) {
            error("PALHaikuKernel: reserve_aligned called with non-power-of-2 alignment.");
            return nullptr;
        }
        // Ensure alignment is at least page_size for kernel page-based operations.
        if (alignment < page_size) alignment = page_size;

        return reserve_logic<state_using>(size, alignment);
    }

    /**
     * @brief Gets 64 bits of entropy.
     * @return A 64-bit random number.
     *
     * TODO: Replace placeholder with a call to a proper Haiku kernel entropy source.
     */
    static uint64_t get_entropy64() noexcept
    {
      uint64_t random_value;
      // This is a placeholder. A real kernel would use its internal RNG.
      // Example: if (kernel_get_random_bytes(&random_value, sizeof(random_value)) != B_OK) {
      //    error("PALHaikuKernel: Failed to get kernel entropy");
      // }
      // Using a weak, predictable fallback for now:
      random_value = ((uint64_t)system_time() << 32) | ((uint32_t)(uintptr_t)&random_value);
      random_value ^= thread_get_current_thread_id(); // XOR with thread ID for minor variation
      return random_value;
    }
  };

} // namespace snmalloc

#endif // __HAIKU__ && _KERNEL_MODE
