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
  HaikuKernelSubMapping* next; ///< Pointer for linked list of active mappings.
};
/** Head of the linked list for active sub-mappings. */
static HaikuKernelSubMapping* s_kernel_mapping_list = nullptr;
/** Spinlock protecting s_kernel_mapping_list. */
static spinlock s_kernel_mapping_list_lock = B_SPINLOCK_INITIALIZER;


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

        // Initialize the (currently simplistic) VA bump pointer.
        // A proper VA manager would initialize its free list here with [arena_base, arena_base + size).
        acquire_spinlock(&g_snmalloc_va_lock);
        g_snmalloc_kernel_va_next_fit = (addr_t)arena_base;
        release_spinlock(&g_snmalloc_va_lock);

        dprintf("PALHaikuKernel: StaticInit created VMArena %" B_PRId32 " at %p, size %lu\n",
            g_snmalloc_kernel_area_id, arena_base, (unsigned long)SNMALLOC_KERNEL_ARENA_INITIAL_SIZE);

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
        // Reset VA bump pointer (a real VA manager would clear its state).
        acquire_spinlock(&g_snmalloc_va_lock);
        g_snmalloc_kernel_va_next_fit = 0;
        release_spinlock(&g_snmalloc_va_lock);


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
      // acquire_spinlock(&g_snmalloc_va_lock);
      // g_va_space_manager->free_range(p, size);
      // release_spinlock(&g_snmalloc_va_lock);
      dprintf("PALHaikuKernel: VA range %p - %p (%zu bytes) is now free (TODO: implement actual VA reuse).\n",
          p, (char*)p + size, size);


      // 5. Free the tracking structure itself.
      // TODO: Replace gGenericObjectCache with a PAL-internal pool for these structs.
      object_cache_free(gGenericObjectCache, mapping_to_free, CACHE_DONT_LOCK_KERNEL_SPACE);
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

      // --- Virtual Address Allocation (Simplified Bump Pointer) ---
      // TODO: Replace with a robust VA Space Manager.
      cpu_status va_lock_state = disable_interrupts();
      acquire_spinlock(&g_snmalloc_va_lock);
      addr_t va_to_map_at = Aal::align_up(g_snmalloc_kernel_va_next_fit, alignment_request);
      if (va_to_map_at < g_snmalloc_kernel_vm_area->Base() || // Check if va_to_map_at is before arena base
          va_to_map_at + aligned_size > g_snmalloc_kernel_vm_area->Base() + g_snmalloc_kernel_vm_area->Size()) {
          // Ran out of VA space in the arena with the current bump pointer, or alignment pushed it out.
          release_spinlock(&g_snmalloc_va_lock);
          restore_interrupts(va_lock_state);
          dprintf("PALHaikuKernel: Cannot satisfy VA request of size %zu (aligned %zu) at alignment %zu in dedicated snmalloc arena. VA Next Fit: %p, Arena End: %p\n",
              size, aligned_size, alignment_request, (void*)g_snmalloc_kernel_va_next_fit, (void*)(g_snmalloc_kernel_vm_area->Base() + g_snmalloc_kernel_vm_area->Size()));
          // TODO: Implement VMArena expansion or a proper VA manager that can find fragmented space.
          return nullptr;
      }
      g_snmalloc_kernel_va_next_fit = va_to_map_at + aligned_size;
      release_spinlock(&g_snmalloc_va_lock);
      restore_interrupts(va_lock_state);
      // --- End VA Allocation ---

      // 1. Account for memory reservation
      status_t mem_reserve_status = vm_try_reserve_memory(aligned_size, VM_PRIORITY_SYSTEM, 0);
      if (mem_reserve_status != B_OK) {
        dprintf("PALHaikuKernel: vm_try_reserve_memory failed for %zu bytes.\n", aligned_size);
        // TODO: Return VA space to VA manager if it was more sophisticated than bump pointer.
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
        return nullptr;
      }

      // 4. Map these physical pages into the allocated Kernel Virtual Address
      vm_page_reservation map_struct_reservation; // For Map() internal needs
      vm_page_init_reservation(&map_struct_reservation);
      VMTranslationMap* trans_map = VMAddressSpace::Kernel()->TranslationMap();
      trans_map->Lock();

      status_t final_map_status = B_OK;
      for (page_num_t i = 0; i < num_pages; ++i) {
        vm_page* current_vm_page = vm_lookup_page(first_page_struct->physical_page_number + i);
        if (!current_vm_page) { // Should not happen for a contiguous run from vm_page_allocate_page_run
            final_map_status = B_ERROR;
            panic("PALHaikuKernel: vm_page lookup failed for page %" B_PRIuPHYSADDR " in allocated run.", first_page_struct->physical_page_number + i);
            break;
        }
        DEBUG_PAGE_ACCESS_START(current_vm_page);
        // Associate page with the VMArea's cache and specify its offset within the area
        g_snmalloc_kernel_vm_cache->InsertPage(current_vm_page,
            (va_to_map_at + i * page_size) - g_snmalloc_kernel_vm_area->Base());

        // Map the page
        status_t map_status = trans_map->Map(
            va_to_map_at + i * page_size,                           // Virtual address for this page
            current_vm_page->physical_page_number << PAGE_SHIFT, // Physical address
            B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,            // Protections
            g_snmalloc_kernel_vm_area->memory_type,                // Memory type from our VMArea
            &map_struct_reservation                              // Reservation for map structures
        );
        DEBUG_PAGE_ACCESS_END(current_vm_page);

        if (map_status != B_OK) {
            final_map_status = map_status;
            // This is a critical failure. A full rollback of already mapped pages,
            // allocated vm_pages, and reservations is needed here. This is complex.
            // For this draft, panicking is a simpler way to halt on error.
            panic("PALHaikuKernel: VMTranslationMap::Map failed for page at va %p! Error: %s",
                (void*)(va_to_map_at + i * page_size), strerror(map_status));
            break;
        }
      }
      trans_map->Unlock();
      vm_page_unreserve_pages(&map_struct_reservation); // Release pages used by Map() for its own structures.

      if (final_map_status != B_OK) {
          // TODO: Implement FULL ROLLBACK of:
          // 1. Already mapped pages in the loop above.
          // 2. All vm_page structures allocated by vm_page_allocate_page_run (needs careful freeing).
          // 3. vm_page_unreserve_pages(&phys_page_reservation).
          // 4. vm_unreserve_memory(aligned_size).
          // 5. Return VA range to VA manager.
          // This is critical for stability. For now, we might have leaked resources if only some pages mapped.
          dprintf("PALHaikuKernel: Mapping failed mid-way. Resources might be leaked. Critical error.\n");
          // Attempt to free what we can easily:
          vm_page_unreserve_pages(&phys_page_reservation);
          vm_unreserve_memory(aligned_size);
          return nullptr;
      }

      // 5. Track the successful mapping for later deallocation.
      // TODO: Replace gGenericObjectCache with a PAL-internal pool.
      HaikuKernelSubMapping* tracking_block = (HaikuKernelSubMapping*)
          object_cache_alloc(gGenericObjectCache, sizeof(HaikuKernelSubMapping),
                             CACHE_DONT_WAIT_FOR_MEMORY | CACHE_DONT_LOCK_KERNEL_SPACE);
      if (!tracking_block) {
          // TODO: FULL ROLLBACK as above. This is a failure to allocate tracking metadata.
          panic("PALHaikuKernel: Failed to allocate tracking block for mapping at va %p.", (void*)va_to_map_at);
          // Attempt cleanup (highly complex at this stage)
          // For now, this would lead to a leak of the mapped region and physical pages.
          vm_unreserve_memory(aligned_size); // At least try to fix accounting
          return nullptr;
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
