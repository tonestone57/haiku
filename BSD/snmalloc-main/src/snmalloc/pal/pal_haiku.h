#pragma once

#if defined(__HAIKU__)

// Remove PALPOSIX dependency as we are providing the core allocation routines.
// #  include "pal_posix.h"

#  include "../aal/aal.h" // For Aal::address_bits, Aal::smallest_page_size
#  include "pal_consts.h" // For PalFeatures, ZeroMem etc.
#  include "pal_ds.h"     // For PalNotificationObject (though not used yet)
#  include "pal_timer_default.h" // For PalTimerDefaultImpl

#  include <OS.h>        // For Haiku API: create_area, delete_area, area_id, B_PAGE_SIZE etc.
#  include <stdio.h>     // For fprintf on error
#  include <stdlib.h>    // For abort
#  include <string.h>    // For memset, strerror
#  include <new>         // For std::nothrow
#  include <map>         // For tracking area_ids
#  include <mutex>       // For protecting the area_id map


namespace snmalloc
{
  // Structure to store area metadata
  struct HaikuAreaInfo {
    area_id id;
    size_t size;
  };

  // Global map to track allocated areas and their IDs.
  static std::map<void*, HaikuAreaInfo> s_haiku_area_map;
  static std::mutex s_haiku_area_map_mutex;

  /**
   * Platform abstraction layer for Haiku.
   * This PAL uses Haiku's create_area and delete_area syscalls for memory management.
   */
  class PALHaiku : public PalTimerDefaultImpl<PALHaiku>
  {
  public:
    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.
     * LazyCommit: Haiku areas are generally lazy by default.
     * Entropy: Haiku provides get_random_data.
     * Time: Inherited from PalTimerDefaultImpl.
     * AlignedAllocation: Not claimed as we can only guarantee page alignment from userland create_area.
     */
    static constexpr uint64_t pal_features =
      LazyCommit | Entropy | Time;

    /**
     * Page size for Haiku.
     */
    static constexpr size_t page_size = B_PAGE_SIZE;

    /**
     * Address bits for the architecture Haiku is running on.
     */
    static constexpr size_t address_bits = Aal::address_bits;

    // minimum_alloc_size is not needed as AlignedAllocation is not specified.

    /**
     * Report a fatal error and exit.
     */
    [[noreturn]] static void error(const char* const str) noexcept
    {
      fprintf(stderr, "snmalloc PALHaiku FATAL ERROR: %s\n", str);
      // Consider writev to STDERR_FILENO for more robustness like PALPOSIX.
      abort();
    }

    /**
     * Notify platform that we will not be needing these pages.
     * For PALHaiku, this means deleting the Haiku area.
     */
    static void notify_not_using(void* p, size_t size) noexcept
    {
      if (p == nullptr)
        return;

      std::lock_guard<std::mutex> guard(s_haiku_area_map_mutex);
      auto it = s_haiku_area_map.find(p);
      if (it != s_haiku_area_map.end()) {
        // If size is non-zero, it should ideally match the original allocation size.
        // snmalloc without pal_enforce_access calls this for the whole original block.
        if (size != 0 && it->second.size != size) {
           // This could indicate a problem or a PAL usage pattern not fully handled yet (e.g. pal_enforce_access).
           fprintf(stderr, "snmalloc PALHaiku: warning: notify_not_using size mismatch for address %p. Expected %zu, got %zu.\n",
            p, it->second.size, size);
           // For now, we proceed to delete the area based on 'p' if found,
           // assuming 'p' is the base of a previously reserved block.
        }
        area_id id_to_delete = it->second.id;
        s_haiku_area_map.erase(it);

        // guard is released before delete_area by scope exit

        if (delete_area(id_to_delete) != B_OK) {
          fprintf(stderr, "snmalloc PALHaiku: delete_area (%" B_PRId32 ") failed for address %p\n", id_to_delete, p);
        }
      } else {
        fprintf(stderr, "snmalloc PALHaiku: notify_not_using called on unknown address %p\n", p);
      }
    }

    /**
     * Notify platform that we will be using these pages.
     * If zero_mem is YesZero, the memory must be zeroed.
     */
    template<ZeroMem zero_mem>
    static void notify_using(void* p, size_t size) noexcept
    {
      SNMALLOC_ASSERT(is_aligned_block<page_size>(p, size));
      if constexpr (zero_mem == YesZero)
      {
        PALHaiku::zero<true>(p, size);
      }
    }

    /**
     * Notify platform that we will be using these pages for reading.
     */
    static void notify_using_readonly(void* p, size_t size) noexcept
    {
      SNMALLOC_ASSERT(is_aligned_block<page_size>(p, size));
      // No specific action needed for Haiku areas beyond initial create_area.
    }

    /**
     * Zero a range of memory.
     */
    template<bool page_aligned = false>
    static void zero(void* p, size_t size) noexcept
    {
      memset(p, 0, size);
    }

    /**
     * Reserve memory. This is the primary allocation function.
     * For Haiku, this creates a new area. B_CONTIGUOUS flag is used to hint
     * for physically contiguous memory, which aids snmalloc's large allocations.
     */
    template<bool state_using_unused = true>
    static void* reserve(size_t size) noexcept
    {
      if (size == 0)
        return nullptr;

      size_t aligned_size = Aal::align_up(size, page_size);
      if (aligned_size == 0 && size > 0) // Overflow check
        return nullptr;

      void* start_address = nullptr;

      // Use B_CONTIGUOUS to request physically contiguous memory if possible.
      // This helps snmalloc with its slab strategy for larger allocations.
      // CREATE_AREA_DONT_CLEAR could be a flag if snmalloc guarantees zeroing.
      // For now, let create_area zero the pages (default behavior).
      area_id id = create_area("snmalloc_arena", &start_address, B_ANY_ADDRESS,
                               aligned_size, B_CONTIGUOUS,
                               B_READ_AREA | B_WRITE_AREA);

      if (id < B_OK) {
        fprintf(stderr, "snmalloc PALHaiku: create_area failed with error %s (%" B_PRId32 ")\n", strerror(id), id);
        return nullptr;
      }

      std::lock_guard<std::mutex> guard(s_haiku_area_map_mutex);
      s_haiku_area_map[start_address] = {id, aligned_size};

      return start_address;
    }

    // reserve_aligned is not strictly needed if AlignedAllocation is not claimed.
    // snmalloc will use reserve() and perform manual alignment if necessary.
    // If reserve_aligned is still part of the concept snmalloc uses,
    // it can simply forward to reserve(), as create_area already provides page_size alignment.
    template<bool state_using = true>
	static void* reserve_aligned(size_t size, size_t alignment) noexcept
	{
        // Haiku's create_area returns page-aligned virtual addresses.
        // B_CONTIGUOUS hints for physical contiguity.
        // snmalloc will handle further virtual alignment if needed from the page-aligned block.
        // We don't pass specific physical alignment to userland create_area.
        UNUSED(alignment); // alignment is effectively page_size or handled by snmalloc
		return reserve<state_using>(size);
	}

    static uint64_t get_entropy64() noexcept
    {
      uint64_t result;
      status_t status = get_random_data(&result, sizeof(result));
      if (status != B_OK) {
        // In a real scenario, a fallback or more robust error handling might be needed.
        // For now, calling PALHaiku::error is consistent.
        error("PALHaiku: get_random_data failed");
      }
      return result;
    }

    // PalTimerDefaultImpl provides internal_time_in_ms.
  };
} // namespace snmalloc
#endif
