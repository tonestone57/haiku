// src/system/kernel/alloc/snmalloc/pal_haiku_kernel.h
#pragma once

// This guard ensures this PAL is only included when building for Haiku Kernel
#if defined(__HAIKU__) && defined(_KERNEL_MODE)

// snmalloc includes - assuming <snmalloc/...> path is set up by build system
// to point to src/third_party/snmalloc_lib/src/
#include <snmalloc/aal/aal.h>
#include <snmalloc/pal/pal_consts.h>
#include <snmalloc/pal/pal_ds.h>
#include <snmalloc/pal/pal_timer_default.h>
#include <snmalloc/ds/bits.h> // Used by _allocate_va_range for is_pow2
#include <snmalloc/error.h>   // For SNMALLOC_ASSERT

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
#include <debug.h>  // For ASSERT, panic, dprintf (used by SNMALLOC_ASSERT indirectly)
#include <kernel.h> // For create_area_etc, delete_area, area_id, system_time, thread_get_current_thread_id
#include <slab/Slab.h> // For object_cache_alloc (used by old PAL version, now only for HaikuKernelSubMapping if not using internal pool)
#include <kernel/thread.h> // For spinlock, disable_interrupts, etc.


// Forward declaration
struct PageRunInfo; // If used by HaikuKernelSubMapping or other PAL structs

// Structure to track free extents of virtual address space within the main VMArena.
struct VAExtent {
    addr_t base;        // Base address of the free VA extent
    size_t size;        // Size of the free VA extent
    VAExtent* next_free; // Pointer to the next VAExtent in a singly linked list (e.g. free list or pool list)
};

#define SNMALLOC_PAL_VA_EXTENT_POOL_SIZE 256
static VAExtent g_pal_va_extent_pool[SNMALLOC_PAL_VA_EXTENT_POOL_SIZE];
static VAExtent* g_pal_va_extent_pool_free_list = nullptr;
static spinlock g_pal_va_extent_pool_lock = B_SPINLOCK_INITIALIZER;

static VAExtent* g_snmalloc_va_free_list_head = nullptr;
// static spinlock g_snmalloc_va_list_lock = B_SPINLOCK_INITIALIZER; // This is in snmalloc_kernel_api.cpp or PAL itself.
// It is indeed here, as part of the PAL's globals.
static spinlock g_snmalloc_va_list_lock = B_SPINLOCK_INITIALIZER;


static VMArea* g_snmalloc_kernel_vm_area = nullptr;
static VMCache* g_snmalloc_kernel_vm_cache = nullptr;
static area_id g_snmalloc_kernel_area_id = -1;
static spinlock g_snmalloc_pal_lock = B_SPINLOCK_INITIALIZER;

#define SNMALLOC_KERNEL_ARENA_INITIAL_SIZE (64 * 1024 * 1024)

// g_snmalloc_kernel_va_next_fit was for the bump pointer, replaced by VAExtent list.
// static addr_t g_snmalloc_kernel_va_next_fit = 0;
// static spinlock g_snmalloc_va_lock = B_SPINLOCK_INITIALIZER; // Replaced by g_snmalloc_va_list_lock

struct HaikuKernelSubMapping {
  void* virtual_address;
  size_t size_in_bytes;
  page_num_t num_pages;
  vm_page* first_page_struct;
  HaikuKernelSubMapping* next;
};
static HaikuKernelSubMapping* s_kernel_mapping_list = nullptr;
static spinlock s_kernel_mapping_list_lock = B_SPINLOCK_INITIALIZER;

#define SNMALLOC_PAL_MAPPING_POOL_SIZE 256
static HaikuKernelSubMapping g_pal_mapping_pool[SNMALLOC_PAL_MAPPING_POOL_SIZE];
static HaikuKernelSubMapping* g_pal_mapping_pool_free_list = nullptr;
static spinlock g_pal_mapping_pool_lock = B_SPINLOCK_INITIALIZER;


namespace snmalloc
{
  class PALHaikuKernel : public PalTimerDefaultImpl<PALHaikuKernel>
  {
  public:
    static constexpr uint64_t pal_features =
      AlignedAllocation | Entropy | Time | Print; // Add other features like LazyCommit if truly supported

    static constexpr size_t page_size = B_PAGE_SIZE;
    static constexpr size_t address_bits = Aal::address_bits;
    static constexpr size_t minimum_alloc_size = page_size;

  private:
    static void _initialize_va_extent_pool_locked()
    {
        g_pal_va_extent_pool_free_list = nullptr;
        for (int i = SNMALLOC_PAL_VA_EXTENT_POOL_SIZE - 1; i >= 0; --i) {
            g_pal_va_extent_pool[i].next_free = g_pal_va_extent_pool_free_list;
            g_pal_va_extent_pool_free_list = &g_pal_va_extent_pool[i];
        }
    }

    static VAExtent* _allocate_va_extent_struct_locked()
    {
        VAExtent* extent = g_pal_va_extent_pool_free_list;
        if (extent) {
            g_pal_va_extent_pool_free_list = extent->next_free;
            extent->next_free = nullptr;
            extent->base = 0;
            extent->size = 0;
        } else {
            dprintf("PALHaikuKernel: CRITICAL - VAExtent static pool exhausted!\n");
        }
        return extent;
    }

    static void _free_va_extent_struct_locked(VAExtent* extent)
    {
        if (extent == nullptr) {
             dprintf("PALHaikuKernel: WARNING - _free_va_extent_struct_locked called with nullptr.\n");
             return;
        }
        extent->next_free = g_pal_va_extent_pool_free_list;
        g_pal_va_extent_pool_free_list = extent;
    }

    static addr_t _allocate_va_range(size_t req_size, size_t req_alignment)
    {
        SNMALLOC_ASSERT(req_size > 0 && (req_size % page_size) == 0);
        SNMALLOC_ASSERT(req_alignment >= page_size && bits::is_pow2(req_alignment));

        cpu_status lock_state = disable_interrupts();
        acquire_spinlock(&g_snmalloc_va_list_lock);

        VAExtent** Pprev_next_ptr = &g_snmalloc_va_free_list_head;
        VAExtent* current_free_extent = g_snmalloc_va_free_list_head;
        addr_t allocated_va_base = 0;

        while (current_free_extent != nullptr) {
            addr_t current_extent_base = current_free_extent->base;
            size_t current_extent_size = current_free_extent->size;
            addr_t aligned_block_start = Aal::align_up(current_extent_base, req_alignment);

            if (aligned_block_start < current_extent_base) {
                Pprev_next_ptr = &current_free_extent->next_free;
                current_free_extent = current_free_extent->next_free;
                continue;
            }
            size_t prefix_padding = aligned_block_start - current_extent_base;

            if (current_extent_size >= req_size + prefix_padding) {
                allocated_va_base = aligned_block_start;
                *Pprev_next_ptr = current_free_extent->next_free;

                cpu_status pool_lock_interrupt_state = disable_interrupts();
                acquire_spinlock(&g_pal_va_extent_pool_lock);

                if (prefix_padding > 0) {
                    VAExtent* prefix_extent = _allocate_va_extent_struct_locked();
                    if (prefix_extent) {
                        prefix_extent->base = current_extent_base;
                        prefix_extent->size = prefix_padding;
                        prefix_extent->next_free = *Pprev_next_ptr;
                        *Pprev_next_ptr = prefix_extent;
                        Pprev_next_ptr = &prefix_extent->next_free;
                    } else {
                        dprintf("PALHaikuKernel: _allocate_va_range: No VAExtent struct for prefix! VA space [0x%lx, size %lu] lost.\n", current_extent_base, prefix_padding);
                    }
                }

                addr_t suffix_start = aligned_block_start + req_size;
                size_t suffix_len = 0;
                if (suffix_start < current_extent_base + current_extent_size) {
                     suffix_len = (current_extent_base + current_extent_size) - suffix_start;
                }

                if (suffix_len > 0) {
                    VAExtent* suffix_extent = _allocate_va_extent_struct_locked();
                    if (suffix_extent) {
                        suffix_extent->base = suffix_start;
                        suffix_extent->size = suffix_len;
                        suffix_extent->next_free = *Pprev_next_ptr;
                        *Pprev_next_ptr = suffix_extent;
                    } else {
                         dprintf("PALHaikuKernel: _allocate_va_range: No VAExtent struct for suffix! VA space [0x%lx, size %lu] lost.\n", suffix_start, suffix_len);
                    }
                }
                _free_va_extent_struct_locked(current_free_extent);

                release_spinlock(&g_pal_va_extent_pool_lock);
                restore_interrupts(pool_lock_interrupt_state);
                goto found_va_range_exit;
            }
            Pprev_next_ptr = &current_free_extent->next_free;
            current_free_extent = current_free_extent->next_free;
        }

    found_va_range_exit:
        release_spinlock(&g_snmalloc_va_list_lock);
        restore_interrupts(lock_state);

        if (allocated_va_base == 0) {
            dprintf("PALHaikuKernel: _allocate_va_range FAILED to find/allocate VA block for size %lu, align %lu\n", req_size, req_alignment);
        }
        return allocated_va_base;
    }

    static void _free_va_range(addr_t base, size_t size)
    {
        if (base == 0 || size == 0) {
            dprintf("PALHaikuKernel: _free_va_range called with base 0 or size 0. Base: %p, Size: %lu\n", (void*)base, size);
            return;
        }
        SNMALLOC_ASSERT((base % page_size) == 0 && (size % page_size) == 0);

        cpu_status lock_state = disable_interrupts();

        cpu_status pool_lock_interrupt_state = disable_interrupts();
        acquire_spinlock(&g_pal_va_extent_pool_lock);
        VAExtent* new_free_extent = _allocate_va_extent_struct_locked();
        release_spinlock(&g_pal_va_extent_pool_lock);
        restore_interrupts(pool_lock_interrupt_state);

        if (!new_free_extent) {
            restore_interrupts(lock_state);
            panic("PALHaikuKernel: _free_va_range: No VAExtent struct available to track freed VA range! VA LEAK: base %p, size %zu", (void*)base, size);
            return;
        }
        new_free_extent->base = base;
        new_free_extent->size = size;
        new_free_extent->next_free = nullptr;

        acquire_spinlock(&g_snmalloc_va_list_lock);

        VAExtent** Pprev_next_ptr = &g_snmalloc_va_free_list_head;
        VAExtent* current_list_iter = g_snmalloc_va_free_list_head;

        while (current_list_iter != nullptr && current_list_iter->base < new_free_extent->base) {
            Pprev_next_ptr = &current_list_iter->next_free;
            current_list_iter = current_list_iter->next_free;
        }
        new_free_extent->next_free = current_list_iter;
        *Pprev_next_ptr = new_free_extent;

        if (new_free_extent->next_free != nullptr &&
            (new_free_extent->base + new_free_extent->size == new_free_extent->next_free->base)) {
            VAExtent* next_block_to_merge = new_free_extent->next_free;
            new_free_extent->size += next_block_to_merge->size;
            new_free_extent->next_free = next_block_to_merge->next_free;

            cpu_status inner_pool_lock_state = disable_interrupts();
            acquire_spinlock(&g_pal_va_extent_pool_lock);
            _free_va_extent_struct_locked(next_block_to_merge);
            release_spinlock(&g_pal_va_extent_pool_lock);
            restore_interrupts(inner_pool_lock_state);
        }

        if (Pprev_next_ptr != &g_snmalloc_va_free_list_head) {
            VAExtent* prev_block_iter = g_snmalloc_va_free_list_head;
            VAExtent* block_before_new_free = nullptr;
            while(prev_block_iter != nullptr && prev_block_iter->next_free != new_free_extent) {
                prev_block_iter = prev_block_iter->next_free;
            }
            block_before_new_free = prev_block_iter;

            if (block_before_new_free != nullptr &&
                (block_before_new_free->base + block_before_new_free->size == new_free_extent->base)) {
                block_before_new_free->size += new_free_extent->size;
                block_before_new_free->next_free = new_free_extent->next_free;

                cpu_status inner_pool_lock_state = disable_interrupts();
                acquire_spinlock(&g_pal_va_extent_pool_lock);
                _free_va_extent_struct_locked(new_free_extent);
                release_spinlock(&g_pal_va_extent_pool_lock);
                restore_interrupts(inner_pool_lock_state);
            }
        }
        release_spinlock(&g_snmalloc_va_list_lock);
        restore_interrupts(lock_state);
    }

  public:
    [[noreturn]] static void error(const char* const str) noexcept
    {
      panic("snmalloc PALHaikuKernel FATAL ERROR: %s", str);
    }

    static void message(const char* const str) noexcept
    {
      dprintf("snmalloc PALHaikuKernel: %s\n", str);
    }

    static status_t StaticInit() {
        cpu_status state = disable_interrupts();
        acquire_spinlock(&g_snmalloc_pal_lock);

        if (g_snmalloc_kernel_area_id >= B_OK) {
            release_spinlock(&g_snmalloc_pal_lock);
            restore_interrupts(state);
            return B_OK;
        }

        void* arena_base = nullptr;
        g_snmalloc_kernel_area_id = create_area_etc(
            VMAddressSpace::KernelID(), "snmalloc_kernel_heap_arena",
            SNMALLOC_KERNEL_ARENA_INITIAL_SIZE, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
            CREATE_AREA_DONT_WAIT | VM_AREA_FLAG_NULL_WIRED, 0, 0, nullptr, nullptr, &arena_base);

        if (g_snmalloc_kernel_area_id < B_OK) {
            release_spinlock(&g_snmalloc_pal_lock);
            restore_interrupts(state);
            panic("PALHaikuKernel: Failed to create snmalloc kernel VMArena! Error: %s", strerror(g_snmalloc_kernel_area_id));
            return g_snmalloc_kernel_area_id;
        }

        g_snmalloc_kernel_vm_area = VMAreas::Lookup(g_snmalloc_kernel_area_id);
        if (g_snmalloc_kernel_vm_area == nullptr) {
            delete_area(g_snmalloc_kernel_area_id);
            g_snmalloc_kernel_area_id = -1;
            release_spinlock(&g_snmalloc_pal_lock);
            restore_interrupts(state);
            panic("PALHaikuKernel: Could not look up created VMArena (id %" B_PRId32 ")!", g_snmalloc_kernel_area_id);
            return B_ERROR;
        }
        g_snmalloc_kernel_vm_cache = vm_area_get_locked_cache(g_snmalloc_kernel_vm_area);
        g_snmalloc_kernel_vm_area->cache->Unlock();

        acquire_spinlock(&g_pal_mapping_pool_lock);
        g_pal_mapping_pool_free_list = nullptr;
        for (int i = SNMALLOC_PAL_MAPPING_POOL_SIZE - 1; i >= 0; --i) {
            g_pal_mapping_pool[i].next = g_pal_mapping_pool_free_list;
            g_pal_mapping_pool_free_list = &g_pal_mapping_pool[i];
        }
        release_spinlock(&g_pal_mapping_pool_lock);

        acquire_spinlock(&g_pal_va_extent_pool_lock);
        _initialize_va_extent_pool_locked();
        VAExtent* initial_extent = _allocate_va_extent_struct_locked();
        release_spinlock(&g_pal_va_extent_pool_lock);

        if (!initial_extent) {
            delete_area(g_snmalloc_kernel_area_id);
            g_snmalloc_kernel_area_id = -1;
            release_spinlock(&g_snmalloc_pal_lock);
            restore_interrupts(state);
            panic("PALHaikuKernel: StaticInit failed to allocate initial VAExtent struct from pool!");
            return B_NO_MEMORY;
        }

        initial_extent->base = (addr_t)arena_base;
        initial_extent->size = SNMALLOC_KERNEL_ARENA_INITIAL_SIZE;
        initial_extent->next_free = nullptr;

        acquire_spinlock(&g_snmalloc_va_list_lock);
        g_snmalloc_va_free_list_head = initial_extent;
        release_spinlock(&g_snmalloc_va_list_lock);

        dprintf("PALHaikuKernel: StaticInit created VMArena %" B_PRId32 " at %p, size %lu\n",
            g_snmalloc_kernel_area_id, arena_base, (unsigned long)SNMALLOC_KERNEL_ARENA_INITIAL_SIZE);
        // ... other dprintfs ...

        release_spinlock(&g_snmalloc_pal_lock);
        restore_interrupts(state);
        return B_OK;
    }

    static void StaticTeardown() {
        cpu_status state = disable_interrupts();
        acquire_spinlock(&g_snmalloc_pal_lock);

        if (g_snmalloc_kernel_area_id >= B_OK) {
            // ... (leak check for s_kernel_mapping_list) ...
            delete_area(g_snmalloc_kernel_area_id);
            g_snmalloc_kernel_area_id = -1;
            g_snmalloc_kernel_vm_area = nullptr;
            g_snmalloc_kernel_vm_cache = nullptr;
            dprintf("PALHaikuKernel: StaticTeardown deleted VMArena.\n");
        }

        cpu_status va_list_lock_state = disable_interrupts();
        acquire_spinlock(&g_snmalloc_va_list_lock);
        VAExtent* current_va_block = g_snmalloc_va_free_list_head;
        g_snmalloc_va_free_list_head = nullptr;
        release_spinlock(&g_snmalloc_va_list_lock);
        restore_interrupts(va_list_lock_state);

        if (current_va_block != nullptr) {
            cpu_status va_pool_lock_state = disable_interrupts();
            acquire_spinlock(&g_pal_va_extent_pool_lock);
            while (current_va_block != nullptr) {
                VAExtent* next_block = current_va_block->next_free;
                _free_va_extent_struct_locked(current_va_block);
                current_va_block = next_block;
            }
            release_spinlock(&g_pal_va_extent_pool_lock);
            restore_interrupts(va_pool_lock_state);
        }

        acquire_spinlock(&g_pal_mapping_pool_lock);
        g_pal_mapping_pool_free_list = nullptr;
        release_spinlock(&g_pal_mapping_pool_lock);

        acquire_spinlock(&g_pal_va_extent_pool_lock);
        g_pal_va_extent_pool_free_list = nullptr;
        release_spinlock(&g_pal_va_extent_pool_lock);

        release_spinlock(&g_snmalloc_pal_lock);
        restore_interrupts(state);
    }

    static void notify_not_using(void* p, size_t size) noexcept
    {
      if (p == nullptr || size == 0) return;
      SNMALLOC_ASSERT(is_aligned_block<page_size>(p, size));

      cpu_status lock_st = disable_interrupts();
      acquire_spinlock(&s_kernel_mapping_list_lock);
      HaikuKernelSubMapping** current_ptr = &s_kernel_mapping_list;
      HaikuKernelSubMapping* mapping_to_free = nullptr;
      while (*current_ptr != nullptr) {
        if ((*current_ptr)->virtual_address == p) {
          mapping_to_free = *current_ptr;
          if (mapping_to_free->size_in_bytes != size) {
            release_spinlock(&s_kernel_mapping_list_lock);
            restore_interrupts(lock_st);
            panic("PALHaikuKernel: notify_not_using size mismatch for %p. Expected %zu, got %zu.",
                p, mapping_to_free->size_in_bytes, size);
            return;
          }
          *current_ptr = mapping_to_free->next;
          break;
        }
        current_ptr = &(*current_ptr)->next;
      }
      release_spinlock(&s_kernel_mapping_list_lock);
      restore_interrupts(lock_st);

      if (mapping_to_free == nullptr) {
        dprintf("PALHaikuKernel: notify_not_using called on unknown address %p or unaligned/sub-region request.\n", p);
        return;
      }

      SNMALLOC_ASSERT(g_snmalloc_kernel_vm_area != nullptr && g_snmalloc_kernel_vm_cache != nullptr);

      VMTranslationMap* trans_map = VMAddressSpace::Kernel()->TranslationMap();
      trans_map->Lock();
      trans_map->Unmap((addr_t)p, (addr_t)p + size - 1);
      atomic_add(&gMappedPagesCount, -(ssize_t)mapping_to_free->num_pages);
      trans_map->Unlock();

      vm_page_reservation reservation;
      vm_page_init_reservation(&reservation);
      vm_page* current_physical_page_tracker = mapping_to_free->first_page_struct;
      for (page_num_t i = 0; i < mapping_to_free->num_pages; ++i) {
        vm_page* page_to_free = vm_lookup_page(current_physical_page_tracker->physical_page_number + i);
        if (!page_to_free) {
            panic("PALHaikuKernel: notify_not_using - vm_page lookup failed for phys page # %" B_PRIuPHYSADDR,
                  current_physical_page_tracker->physical_page_number + i);
            continue;
        }
        DEBUG_PAGE_ACCESS_START(page_to_free);
        g_snmalloc_kernel_vm_cache->RemovePage(page_to_free);
        vm_page_free_etc(g_snmalloc_kernel_vm_cache, page_to_free, &reservation);
        DEBUG_PAGE_ACCESS_END(page_to_free);
      }
      vm_page_unreserve_pages(&reservation);
      vm_unreserve_memory(mapping_to_free->size_in_bytes);
      _free_va_range((addr_t)p, size);

      cpu_status pool_lock_st = disable_interrupts();
      acquire_spinlock(&g_pal_mapping_pool_lock);
      mapping_to_free->next = g_pal_mapping_pool_free_list;
      g_pal_mapping_pool_free_list = mapping_to_free;
      release_spinlock(&g_pal_mapping_pool_lock);
      restore_interrupts(pool_lock_st);
    }

    template<ZeroMem zero_mem>
    static void notify_using(void* p, size_t size) noexcept
    {
      SNMALLOC_ASSERT(is_aligned_block<page_size>(p, size));
      if constexpr (zero_mem == YesZero) {
        PALHaikuKernel::zero<true>(p, size);
      }
    }

    template<bool page_aligned = false>
    static void zero(void* p, size_t size) noexcept
    {
      memset(p, 0, size);
    }

    template<bool state_using_unused = true>
    static void* reserve_logic(size_t size, size_t alignment_request) noexcept
    {
      if (g_snmalloc_kernel_area_id < B_OK) {
        if (StaticInit() != B_OK) {
             error("PALHaikuKernel::reserve_logic called before StaticInit() and StaticInit failed!");
             return nullptr;
        }
      }
      if (size == 0) return nullptr;

      size_t aligned_size = Aal::align_up(size, page_size);
      if (aligned_size == 0 && size > 0) return nullptr;

      page_num_t num_pages = aligned_size / page_size;
      addr_t va_to_map_at = _allocate_va_range(aligned_size, alignment_request);
      if (va_to_map_at == 0) {
          return nullptr;
      }

      status_t mem_reserve_status = vm_try_reserve_memory(aligned_size, VM_PRIORITY_SYSTEM, 0);
      if (mem_reserve_status != B_OK) {
        dprintf("PALHaikuKernel: vm_try_reserve_memory failed for %zu bytes. Error: %s\n", aligned_size, strerror(mem_reserve_status));
        _free_va_range(va_to_map_at, aligned_size);
        return nullptr;
      }

      vm_page_reservation phys_page_reservation;
      vm_page_init_reservation(&phys_page_reservation);
      vm_page_reserve_pages(&phys_page_reservation, num_pages, VM_PRIORITY_SYSTEM);

      vm_page* first_page_struct = vm_page_allocate_page_run(
          (state_using_unused ? VM_PAGE_ALLOC_WIRED_CLEAR : VM_PAGE_ALLOC_RESERVED),
          num_pages, &physical_address_restrictions::empty, VM_PRIORITY_SYSTEM);

      if (first_page_struct == nullptr) {
        vm_page_unreserve_pages(&phys_page_reservation);
        vm_unreserve_memory(aligned_size);
        dprintf("PALHaikuKernel: vm_page_allocate_page_run failed for %lu pages.\n", num_pages);
        _free_va_range(va_to_map_at, aligned_size);
        return nullptr;
      }

      vm_page_reservation map_struct_reservation;
      vm_page_init_reservation(&map_struct_reservation);
      VMTranslationMap* trans_map = VMAddressSpace::Kernel()->TranslationMap();
      trans_map->Lock();

      status_t final_map_status = B_OK;
        page_num_t successfully_mapped_pages = 0;

      for (page_num_t i = 0; i < num_pages; ++i) {
        vm_page* current_vm_page = vm_lookup_page(first_page_struct->physical_page_number + i);
          if (!current_vm_page) {
              final_map_status = B_ERROR;
              dprintf("PALHaikuKernel: vm_page lookup failed for page %" B_PRIuPHYSADDR " in allocated run during mapping.\n", first_page_struct->physical_page_number + i);
              break;
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
              break;
        }
          successfully_mapped_pages++;
      }
      trans_map->Unlock();
        vm_page_unreserve_pages(&map_struct_reservation);

      if (final_map_status != B_OK) {
            dprintf("PALHaikuKernel: Mapping failed. Rolling back. Successfully mapped %lu pages before error.\n", successfully_mapped_pages);
            if (successfully_mapped_pages > 0) {
                VMTranslationMap* trans_map_rb = VMAddressSpace::Kernel()->TranslationMap();
                trans_map_rb->Lock();
                trans_map_rb->Unmap(va_to_map_at, va_to_map_at + (successfully_mapped_pages * page_size) - 1);
                atomic_add(&gMappedPagesCount, -(ssize_t)successfully_mapped_pages);
                trans_map_rb->Unlock();
            }
            vm_page_reservation page_free_reservation;
            vm_page_init_reservation(&page_free_reservation);
            for (page_num_t i = 0; i < num_pages; ++i) {
                vm_page* page_to_free = vm_lookup_page(first_page_struct->physical_page_number + i);
                if (page_to_free) {
                    DEBUG_PAGE_ACCESS_START(page_to_free);
                    if(g_snmalloc_kernel_vm_cache) g_snmalloc_kernel_vm_cache->RemovePage(page_to_free);
                    vm_page_free_etc(g_snmalloc_kernel_vm_cache, page_to_free, &page_free_reservation);
                    DEBUG_PAGE_ACCESS_END(page_to_free);
                }
            }
            vm_page_unreserve_pages(&page_free_reservation);
            vm_page_unreserve_pages(&phys_page_reservation);
            vm_unreserve_memory(aligned_size);
            _free_va_range(va_to_map_at, aligned_size);
          return nullptr;
      }

      cpu_status pool_lock_st = disable_interrupts();
      acquire_spinlock(&g_pal_mapping_pool_lock);
      HaikuKernelSubMapping* tracking_block = g_pal_mapping_pool_free_list;
      if (tracking_block != nullptr) {
          g_pal_mapping_pool_free_list = tracking_block->next;
      }
      release_spinlock(&g_pal_mapping_pool_lock);
      restore_interrupts(pool_lock_st);

      if (!tracking_block) {
          dprintf("PALHaikuKernel: CRITICAL - Ran out of HaikuKernelSubMapping pool for mapping at va %p.\n", (void*)va_to_map_at);
          VMTranslationMap* trans_map_rb = VMAddressSpace::Kernel()->TranslationMap();
          trans_map_rb->Lock();
          trans_map_rb->Unmap(va_to_map_at, va_to_map_at + aligned_size - 1);
          atomic_add(&gMappedPagesCount, -(ssize_t)num_pages);
          trans_map_rb->Unlock();
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
          vm_page_unreserve_pages(&rollback_reservation);
          vm_page_unreserve_pages(&phys_page_reservation);
          vm_unreserve_memory(aligned_size);
          _free_va_range(va_to_map_at, aligned_size);
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

      vm_page_unreserve_pages(&phys_page_reservation);
      return (void*)va_to_map_at;
    }

    template<bool state_using_unused = true>
    static void* reserve(size_t size) noexcept
    {
        return reserve_logic<state_using_unused>(size, page_size);
    }

    template<bool state_using = true>
    static void* reserve_aligned(size_t size, size_t alignment) noexcept
    {
        if (alignment == 0) alignment = 1;
        if (!bits::is_pow2(alignment)) {
            error("PALHaikuKernel: reserve_aligned called with non-power-of-2 alignment.");
            return nullptr;
        }
        if (alignment < page_size) alignment = page_size;
        return reserve_logic<state_using>(size, alignment);
    }

    static uint64_t get_entropy64() noexcept
    {
      uint64_t random_value;
      random_value = ((uint64_t)system_time() << 32) | ((uint32_t)(uintptr_t)&random_value);
      random_value ^= thread_get_current_thread_id();
      return random_value;
    }
  };

} // namespace snmalloc

#endif // __HAIKU__ && _KERNEL_MODE
