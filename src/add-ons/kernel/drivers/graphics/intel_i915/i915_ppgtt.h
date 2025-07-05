#ifndef I915_PPGTT_H
#define I915_PPGTT_H

#include "intel_i915_priv.h" // For gen7_ppgtt_pde_t, device_info, GEM object struct (indirectly)
#include <kernel/locks/mutex.h>
#include <kernel/util/list.h> // Potentially for managing PT BOs if not a fixed array

// Forward declare GEM object structure if not fully visible via intel_i915_priv.h
// struct intel_i915_gem_object; // Already implicitly available via intel_i915_priv.h -> gem_object.h (conceptually)

// Structure to manage a Per-Process GTT instance
struct i915_ppgtt {
	struct intel_i915_device_info* dev_priv;
	struct mutex lock; // Lock for this ppgtt instance's operations
	int refcount;      // Refcount for the PPGTT struct itself

	// Page Directory (PD) - for Gen7, one PD covers up to 4GB (1024 PDEs * 1024 PTEs * 4KB)
	// We use one PD, pointed to by what would be PDP0 in the context image.
	struct intel_i915_gem_object* pd_bo; // GEM object for the single Page Directory
	gen7_ppgtt_pde_t* pd_cpu_map;        // CPU virtual address of the mapped PD BO

	// Page Tables (PTs) are more dynamic.
	// For Gen7, a single PD has 1024 PDEs, each potentially pointing to a PT.
	// We store the GEM objects backing these PTs. NULL if no PT for that PDE.
	struct intel_i915_gem_object* pt_bos[GEN7_PPGTT_PD_ENTRIES];
	// We might also cache CPU mappings of these PTs, or map/unmap them on demand.
	// For simplicity initially, we can map them when created and keep them mapped.
	// gen7_ppgtt_pte_t* pt_cpu_maps[GEN7_PPGTT_PD_ENTRIES]; // Optional cached CPU maps for PTs

	// GPU Virtual Address Space Allocator for this PPGTT
	// This is a placeholder for a more sophisticated VMA manager.
	// For now, it might not be used if userspace provides GPU VAs.
	uint64_t vma_next_free_offset; // Simple bump allocator for GPU VAs
	uint64_t vma_size;             // e.g., 4GB for a 32-bit PPGTT

	// Other PPGTT-specific info, e.g., generation, flags
};

// PPGTT lifecycle functions
status_t i915_ppgtt_create(struct intel_i915_device_info* devInfo, struct i915_ppgtt** ppgtt_out);
void i915_ppgtt_destroy(struct i915_ppgtt* ppgtt); // Takes a ref, destroys when refcount is 0
void i915_ppgtt_get(struct i915_ppgtt* ppgtt);
void i915_ppgtt_put(struct i915_ppgtt* ppgtt);


// PPGTT binding functions
status_t i915_ppgtt_bind_object(struct i915_ppgtt* ppgtt, struct intel_i915_gem_object* obj,
                                uint64_t ppgtt_addr, bool map_writable, enum gtt_caching_type gpu_cache_type);
status_t i915_ppgtt_unbind_object(struct i915_ppgtt* ppgtt, uint64_t ppgtt_addr, size_t size);

#endif /* I915_PPGTT_H */
