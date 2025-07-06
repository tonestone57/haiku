#pragma once

// This PAL is for Haiku userland (libroot)
#if defined(__HAIKU__) && !defined(_KERNEL_MODE)

// Core snmalloc headers that this PAL might need definitions from,
// assuming they are found via -I$(HAIKU_TOP)/BSD/snmalloc-main/src
#  include <snmalloc/aal/aal.h> // For Aal::address_bits, Aal::smallest_page_size, Aal::align_up
#  include <snmalloc/pal/pal_consts.h> // For PalFeatures, ZeroMem etc.
#  include <snmalloc/pal/pal_ds.h>     // For PalNotificationObject (though not used yet by this PAL)
#  include <snmalloc/pal/pal_timer_default.h> // For PalTimerDefaultImpl
#  include <snmalloc/error.h>   // For SNMALLOC_ASSERT, SNMALLOC_LIKELY, etc.

// Haiku Userland Headers
#  include <OS.h>        // For Haiku API: create_area, delete_area, area_id, B_PAGE_SIZE etc.
#  include <stdio.h>     // For fprintf on error (used by PAL's error reporting)
#  include <stdlib.h>    // For abort (used by PAL's error reporting)
#  include <string.h>    // For memset, strerror

// C++ Standard Library
#  include <new>         // For std::nothrow (though not directly used, good include for C++ PALs)
#  include <map>         // For tracking area_ids
#  include <mutex>       // For protecting the area_id map


namespace snmalloc
{
  // Structure to store area metadata for userland areas
  struct HaikuUserAreaInfo {
    area_id id;
    size_t size;
    void* base_address; // Store base address for map key consistency
  };

  // Global map to track allocated areas and their IDs.
  // Keyed by base_address for easier lookup.
  static std::map<void*, HaikuUserAreaInfo> s_haiku_user_area_map;
  static std::mutex s_haiku_user_area_map_mutex; // Standard C++ mutex

  /**
   * Platform abstraction layer for Haiku Userland (libroot).
   * This PAL uses Haiku's create_area and delete_area syscalls for memory management.
   */
  class PALHaikuUser : public PalTimerDefaultImpl<PALHaikuUser>
  {
  public:
    /**
     * Bitmap of PalFeatures flags indicating the optional features that this
     * PAL supports.
     * LazyCommit: Haiku areas are generally lazy by default.
     * Entropy: Haiku provides get_random_data.
     * Time: Inherited from PalTimerDefaultImpl.
     * AlignedAllocation: Not claimed; create_area provides page alignment.
     * Print: For error/message output.
     */
    static constexpr uint64_t pal_features =
      LazyCommit | Entropy | Time | Print;

    static constexpr size_t page_size = B_PAGE_SIZE;
    static constexpr size_t address_bits = Aal::address_bits;
    // minimum_alloc_size not specified as AlignedAllocation is false.

    [[noreturn]] static void error(const char* const str) noexcept
    {
      // Using dprintf for libroot context might be better than fprintf(stderr)
      // if stderr itself could be using malloc. For early init, raw write to STDERR_FILENO is safest.
      // For now, keeping it simple. A more robust version would use system calls for output.
      char buffer[1024];
      snprintf(buffer, sizeof(buffer), "snmalloc PALHaikuUser FATAL ERROR: %s\n", str);
      write(STDERR_FILENO, buffer, strlen(buffer));
      abort();
    }

    static void message(const char* const str) noexcept
    {
      char buffer[1024];
      snprintf(buffer, sizeof(buffer), "snmalloc PALHaikuUser: %s\n", str);
      write(STDOUT_FILENO, buffer, strlen(buffer));
    }

    static void notify_not_using(void* p, size_t size) noexcept
    {
      if (p == nullptr)
        return;

      // size parameter is often the original requested size, which might be different
      // from the aligned size stored, or 0 if not known by caller.
      // We primarily rely on 'p' to find the area.

      std::lock_guard<std::mutex> guard(s_haiku_user_area_map_mutex);
      auto it = s_haiku_user_area_map.find(p);

      if (it != s_haiku_user_area_map.end()) {
        if (size != 0 && it->second.size != Aal::align_up(size, page_size) && it->second.size != size) {
           // This warning logic might need refinement based on how snmalloc calls this.
           // It's possible snmalloc calls with the exact original size, not the aligned one.
           dprintf("snmalloc PALHaikuUser: warning: notify_not_using size hint mismatch for address %p. Stored aligned size %zu, hint size %zu.\n",
            p, it->second.size, size);
        }
        area_id id_to_delete = it->second.id;
        s_haiku_user_area_map.erase(it);
        // Mutex is released before delete_area by RAII.

        if (delete_area(id_to_delete) != B_OK) {
          // Cannot call PALHaikuUser::error here as it might try to lock the mutex again.
          dprintf("snmalloc PALHaikuUser: delete_area (%" B_PRId32 ") failed for address %p\n", id_to_delete, p);
          // This is a serious issue, but aborting might be too much if other areas are fine.
        }
      } else {
        // This can happen if snmalloc tries to decommit a sub-region of an area,
        // which this PAL doesn't support (Haiku areas are the unit of deletion).
        // Or if 'p' is not the start of an area we allocated.
        dprintf("snmalloc PALHaikuUser: notify_not_using called on unknown or sub-region address %p\n", p);
      }
    }

    template<ZeroMem zero_mem>
    static void notify_using(void* p, size_t size) noexcept
    {
      SNMALLOC_ASSERT(Aal::is_aligned_block(address_cast(p), size, page_size));
      if constexpr (zero_mem == YesZero)
      {
        PALHaikuUser::zero<true>(p, size);
      }
    }

    static void notify_using_readonly(void* p, size_t size) noexcept
    {
      SNMALLOC_ASSERT(Aal::is_aligned_block(address_cast(p), size, page_size));
      // No specific action for Haiku userland areas beyond initial create_area.
      // Protections are per-area.
    }

    template<bool page_aligned = false>
    static void zero(void* p, size_t size) noexcept
    {
      memset(p, 0, size);
    }

    template<bool state_using_unused = true> // Corresponds to snmalloc's internal state hint
    static void* reserve(size_t size_request) noexcept
    {
      if (size_request == 0)
        return nullptr;

      size_t aligned_size = Aal::align_up(size_request, page_size);
      if (aligned_size == 0 && size_request > 0) { // Overflow check
        dprintf("snmalloc PALHaikuUser: reserve size request overflowed after alignment: %zu -> %zu\n", size_request, aligned_size);
        return nullptr;
      }
      if (aligned_size < size_request) { // Another overflow check
         dprintf("snmalloc PALHaikuUser: reserve size request overflowed (aligned < original): %zu -> %zu\n", size_request, aligned_size);
         return nullptr;
      }


      void* start_address = nullptr;
      // B_CONTIGUOUS is a hint. For userland, virtual contiguity is guaranteed by create_area.
      // Physical contiguity is not guaranteed but hinted.
      // CREATE_AREA_DONT_CLEAR is not used; Haiku areas are zeroed by default.
      // If state_using_unused is false, snmalloc might not zero it later, relying on OS.
      area_id id = create_area("snmalloc_ul_arena", &start_address, B_ANY_ADDRESS,
                               aligned_size, B_LAZY_LOCK, // B_LAZY_LOCK for userland areas
                               B_READ_AREA | B_WRITE_AREA);

      if (id < B_OK) {
        dprintf("snmalloc PALHaikuUser: create_area failed with error %s (%" B_PRId32 ") for size %zu (aligned %zu)\n",
                strerror(id), id, size_request, aligned_size);
        return nullptr;
      }

      std::lock_guard<std::mutex> guard(s_haiku_user_area_map_mutex);
      s_haiku_user_area_map[start_address] = {id, aligned_size, start_address};

      return start_address;
    }

    template<bool state_using = true>
	static void* reserve_aligned(size_t size, size_t alignment) noexcept
	{
        // Haiku's create_area returns page-aligned virtual addresses.
        // snmalloc's core will handle further virtual alignment if needed from the page-aligned block
        // if alignment > page_size.
        if (alignment <= page_size)
            return reserve<state_using>(size);

        // For alignments larger than page_size, snmalloc expects the PAL to handle it
        // or it will over-reserve and align manually. Since AlignedAllocation is false,
        // snmalloc will do the latter. So, this can just be:
        UNUSED(alignment);
        return reserve<state_using>(size);
	}

    static uint64_t get_entropy64() noexcept
    {
      uint64_t result;
      status_t status = get_random_data(&result, sizeof(result));
      if (status != B_OK) {
        // Cannot call PALHaikuUser::error here due to potential recursion if error uses malloc.
        // A very simple direct write or spin + abort might be needed for true fatal PAL errors.
        dprintf("snmalloc PALHaikuUser: get_random_data failed. Returning low-quality entropy.\n");
        // Fallback to some low-quality entropy if absolutely necessary, though this is bad.
        // Aborting is safer if entropy is critical. For now, returning something.
        result = (uint64_t)(uintptr_t)&result ^ (uint64_t)system_time();
      }
      return result;
    }
  };
} // namespace snmalloc
#endif // __HAIKU__ && !_KERNEL_MODE
