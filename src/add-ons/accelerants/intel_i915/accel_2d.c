/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "accelerant.h"       // For gInfo, accelerant_info, IOCTL codes and args
#include <unistd.h>           // For ioctl
#include <syslog.h>           // For syslog
#include <string.h>           // For memcpy, memset
#include <stdlib.h>           // For malloc, free
#include <GraphicsDefs.h>     // For color_space enum

#undef TRACE
#define TRACE_ACCEL
#ifdef TRACE_ACCEL
#	define TRACE(x...) syslog(LOG_INFO, "intel_i915_accelerant_2d: " x)
#else
#	define TRACE(x...)
#endif

// --- GEM Helper Function Prototypes & Stubs (Conceptual) ---
static status_t
get_gtt_offset_for_gem_handle(uint32_t gem_handle, uint64_t* out_gtt_offset, size_t* out_size)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || out_gtt_offset == NULL || out_size == NULL)
		return B_BAD_VALUE;

	intel_i915_gem_info_args args;
	args.handle = gem_handle;

	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_GET_INFO, &args, sizeof(args)) != B_OK) {
		TRACE("get_gtt_offset_for_gem_handle: GEM_GET_INFO failed for handle %u\n", gem_handle);
		return B_ERROR;
	}

	if (!args.gtt_mapped) {
		TRACE("get_gtt_offset_for_gem_handle: Handle %u is not GTT mapped by kernel.\n", gem_handle);
		// This is problematic. The kernel needs to ensure BOs used by GPU are GTT mapped.
		// Forcing a map here from accelerant might be possible with another IOCTL if kernel supports it,
		// or this indicates an issue with how the BO was prepared/passed.
		return B_BAD_VALUE; // Or some other error indicating it's not ready for GPU
	}

	*out_gtt_offset = (uint64_t)args.gtt_offset_pages * B_PAGE_SIZE;
	*out_size = args.size; // This is allocated_size
	return B_OK;
}

static status_t
create_and_upload_gem_bo(const void* data, size_t size, uint32_t gem_create_flags,
                         uint32_t* out_gem_handle, area_id* out_kernel_area_id,
                         void** out_cpu_addr, uint64_t* out_gtt_offset)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || data == NULL || size == 0 ||
		out_gem_handle == NULL || out_kernel_area_id == NULL ||
		out_cpu_addr == NULL || out_gtt_offset == NULL)
		return B_BAD_VALUE;

	status_t status;
	intel_i915_gem_create_args create_args = { .size = size, .flags = gem_create_flags };
	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CREATE, &create_args, sizeof(create_args)) != 0) {
		TRACE("create_and_upload_gem_bo: GEM_CREATE failed\n");
		return B_ERROR;
	}
	*out_gem_handle = create_args.handle;

	intel_i915_gem_mmap_area_args mmap_args = { .handle = *out_gem_handle };
	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_MMAP_AREA, &mmap_args, sizeof(mmap_args)) != 0) {
		TRACE("create_and_upload_gem_bo: GEM_MMAP_AREA failed for handle %u\n", *out_gem_handle);
		status = B_ERROR;
		goto err_close_bo;
	}
	*out_kernel_area_id = mmap_args.map_area_id; // This is the kernel area

	// Clone and map for CPU write
	area_id cloned_area = clone_area("gem_upload_clone", out_cpu_addr, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, *out_kernel_area_id);
	if (cloned_area < B_OK) {
		TRACE("create_and_upload_gem_bo: clone_area failed for area %" B_PRId32 "\n", *out_kernel_area_id);
		status = cloned_area;
		goto err_close_bo;
	}

	memcpy(*out_cpu_addr, data, size);
	delete_area(cloned_area); // Unmap CPU virtual address after copy
	*out_cpu_addr = NULL; // No longer valid

	// Get GTT offset
	// The BO must be GTT mapped by the kernel for GPU access.
	// This might happen implicitly on creation for certain types, or via execbuffer domains,
	// or an explicit GTT bind IOCTL (which this Haiku driver doesn't seem to have).
	// We rely on INTEL_I915_IOCTL_GEM_GET_INFO.
	size_t bo_alloc_size;
	status = get_gtt_offset_for_gem_handle(*out_gem_handle, out_gtt_offset, &bo_alloc_size);
	if (status != B_OK) {
		TRACE("create_and_upload_gem_bo: get_gtt_offset failed for handle %u\n", *out_gem_handle);
		goto err_close_bo;
	}

	return B_OK;

err_close_bo:
	if (*out_gem_handle != 0) {
		intel_i915_gem_close_args close_args = { *out_gem_handle };
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args, sizeof(close_args));
		*out_gem_handle = 0;
	}
	return status;
}


// --- Polygon Filling Functions (Conceptual Stubs) ---

void
intel_i915_fill_triangle_list(engine_token *et,
    const fill_triangle_params triangle_list[], uint32 num_triangles,
    uint32 color, const general_rect* clip_rects, uint32 num_clip_rects)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || num_triangles == 0 || triangle_list == NULL) {
		TRACE("fill_triangle_list: Invalid params or not initialized.\n");
		return;
	}

	TRACE("fill_triangle_list: %lu triangles, color 0x%lx. 3D Pipe (STUBBED).\n",
		num_triangles, color);

	// TODO: Full implementation using 3D pipeline (similar to conceptual arbitrary line drawing)
	// - Acquire Engine, Get Batch Buffer
	// - For each triangle (or batch):
	//   - Setup Vertex Buffer: 3 vertices (x1,y1), (x2,y2), (x3,y3)
	// - Setup 3D Pipeline States:
	//   - Dest Surface State (framebuffer)
	//   - Solid Color VS & PS (reuse from line drawing)
	//   - VS/PS Constants (for color)
	//   - Vertex Elements
	//   - Viewport, Scissor (from clip_rects), Blend (opaque), Depth/Stencil (disabled)
	//   - Rasterization state (CULLMODE_NONE or CULLMODE_BACK depending on winding)
	// - Emit 3DPRIMITIVE (_3DPRIM_TRIANGLELIST)
	// - Emit PIPE_CONTROL for sync
	// - Submit Batch Buffer
	// - Release Engine

	uint8_t gen = gInfo->shared_info->graphics_generation;
	if (gen < 7) { // Basic 3D pipeline for this is generally targeted at Gen7+
		TRACE("fill_triangle_list: Polygon fill via 3D pipe not supported/implemented for Gen < 7. No-op.\n");
		return;
	}

	// TODO: Acquire engine (ACQUIRE_ENGINE(et);)
	// For now, this stub will process one triangle at a time to show the flow.
	// A real implementation should batch multiple triangles into a single command buffer
	// and a single vertex buffer if possible for efficiency.

	for (uint32 i = 0; i < num_triangles; i++) {
		const fill_triangle_params* current_tri = &triangle_list[i];

		// 1. Prepare Vertex Data for this triangle
		// Using scaled_blit_vertex for {x,y,z,w, u,v} struct. u,v can be zero for solid fill.
		// Z=0, W=1 for 2D screen-space rendering.
		// Vertices should be in screen coordinates. VS will handle transformation to clip space.
		scaled_blit_vertex vb_data[3];
		vb_data[0] = (scaled_blit_vertex){(float)current_tri->x1, (float)current_tri->y1, 0.0f, 1.0f, 0.0f, 0.0f};
		vb_data[1] = (scaled_blit_vertex){(float)current_tri->x2, (float)current_tri->y2, 0.0f, 1.0f, 0.0f, 0.0f};
		vb_data[2] = (scaled_blit_vertex){(float)current_tri->x3, (float)current_tri->y3, 0.0f, 1.0f, 0.0f, 0.0f};

		uint32_t vb_handle = 0;
		area_id k_area_vb = -1; // Kernel area_id for the mmap of VB
		void* cpu_addr_vb_clone = NULL; // CPU address of cloned VB area (not used after upload)
		uint64_t vb_gtt_offset = 0;
		status_t vb_status = create_and_upload_gem_bo(vb_data, sizeof(vb_data),
			I915_BO_ALLOC_CACHING_WC, // Suitable for CPU write, GPU read
			&vb_handle, &k_area_vb, &cpu_addr_vb_clone, &vb_gtt_offset);

		if (vb_status != B_OK) {
			TRACE("fill_triangle_list: Failed to create/upload vertex buffer for tri %u: %s\n", i, strerror(vb_status));
			continue; // Skip this triangle
		}
		// cpu_addr_vb_clone is already unmapped by create_and_upload_gem_bo

		// 2. Create and populate command buffer
		size_t cmd_dwords_estimate = 150; // Rough estimate for states + 1 triangle
		size_t cmd_buffer_size = cmd_dwords_estimate * sizeof(uint32);
		uint32 cmd_handle_batch = 0; // Different from vb_handle
		area_id k_area_cmd = -1, c_area_cmd = -1;
		uint32* cs_start = NULL;
		uint32* cs = NULL;

		if (create_cmd_buffer(cmd_buffer_size, &cmd_handle_batch, &k_area_cmd, (void**)&cs_start) != B_OK) {
			TRACE("fill_triangle_list: Failed to create command buffer for tri %u.\n", i);
			if (vb_handle != 0) { // Clean up VB if cmd buffer fails
				intel_i915_gem_close_args close_args = { vb_handle };
				ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args, sizeof(close_args));
			}
			continue; // Skip this triangle
		}
		c_area_cmd = area_for(cs_start);
		cs = cs_start;

		// --- Conceptual Command Stream Construction for one triangle ---
		// Note: Many commands (STATE_BASE_ADDRESS, SURFACE_STATE, BINDING_TABLE, SHADERS, VF/VE, VIEWPORT, BLEND, DEPTH, RASTER)
		// would ideally be set up once if multiple triangles are batched, or if a graphics context is maintained.
		// For this per-triangle stub, they are conceptually repeated.

		TRACE("    TODO (Conceptual): Emit PIPELINE_SELECT (3D).\n");
		TRACE("    TODO (Conceptual): Emit STATE_BASE_ADDRESS (Surface State Base, Dynamic State Base, etc.).\n");
		TRACE("    TODO (Conceptual): Emit RENDER_SURFACE_STATE for destination (framebuffer as Render Target) into SSB.\n");
		//      - Base Address: gInfo->shared_info->framebuffer_physical
		//      - Width/Height/Pitch/Tiling/Format from gInfo->shared_info
		TRACE("    TODO (Conceptual): Emit BINDING_TABLE_STATE (for Render Target) and 3DSTATE_BINDING_TABLE_POINTERS.\n");
		TRACE("    TODO (Conceptual): Emit shader states (3DSTATE_VS, 3DSTATE_PS, MEDIA_VFE_STATE).\n");
		TRACE("    TODO (Conceptual): Emit 3DSTATE_CONSTANT_PS to load 'color' (converted to float RGBA).\n");
		TRACE("    TODO (Conceptual): Emit 3DSTATE_VERTEX_BUFFERS (using vb_gtt_offset, stride sizeof(scaled_blit_vertex)).\n");
		TRACE("    TODO (Conceptual): Emit 3DSTATE_VERTEX_ELEMENTS (for position R32G32_FLOAT or R32G32B32A32_FLOAT from VB).\n");
		TRACE("    TODO (Conceptual): Emit Viewport state (to screen dimensions).\n");
		TRACE("    TODO (Conceptual): Emit Scissor state (from clip_rects if num_clip_rects > 0, else to screen/dest bounds).\n");
		//      if (num_clip_rects > 0) { /* Emit 3DSTATE_SCISSOR_RECTANGLE using clip_rects[0] */ }
		TRACE("    TODO (Conceptual): Emit Blend state (opaque), Depth/Stencil state (disabled), Raster state (CULL_NONE).\n");

		TRACE("    TODO (Conceptual): Emit 3DPRIMITIVE (_3DPRIM_TRIANGLELIST, vertex_count=3, start_vertex=0).\n");
		//      Example: *cs++ = (CMD_3DPRIMITIVE | _3DPRIM_TRIANGLELIST | (num_dwords_for_prim_cmd - 2));
		//               *cs++ = 3; // Vertex Count per Instance
		//               *cs++ = 0; // Start Vertex Location
		//               *cs++ = 1; // Instance Count
		//               *cs++ = 0; // Start Instance Location
		//               *cs++ = 0; // Vertex Buffer Index

		cs = emit_pipe_control(cs, PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH | PIPE_CONTROL_CS_STALL, 0,0,0);
		*cs++ = MI_BATCH_BUFFER_END;
		// --- End Conceptual Command Stream ---

		uint32_t current_batch_len_dwords = cs - cs_start;
		intel_i915_gem_execbuffer_args exec_args = { cmd_handle_batch, current_batch_len_dwords * sizeof(uint32), RCS0 };
		// TODO: Add relocations for vb_gtt_offset and any GTT offsets for shaders, state heaps.

		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args)) != 0) {
			TRACE("fill_triangle_list: EXECBUFFER failed for tri %u.\n", i);
		}

		destroy_cmd_buffer(cmd_handle_batch, c_area_cmd, cs_start);
		if (vb_handle != 0) { // Destroy the temporary Vertex Buffer GEM object for this triangle
			intel_i915_gem_close_args close_args = { vb_handle };
			ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args, sizeof(close_args));
		}
	}
	// TODO: Release engine (RELEASE_ENGINE(et, NULL);)
	return;
}

void
intel_i915_fill_convex_polygon(engine_token *et,
    const int16 coords[], uint32 num_vertices, // coords is [x0,y0, x1,y1, ...]
    uint32 color, const general_rect* clip_rects, uint32 num_clip_rects)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || num_vertices < 3 || coords == NULL) {
		TRACE("fill_convex_polygon: Invalid params (num_vertices %lu) or not initialized.\n", num_vertices);
		return;
	}

	TRACE("fill_convex_polygon: %lu vertices, color 0x%lx. 3D Pipe (STUBBED).\n",
		num_vertices, color);

	if (num_vertices > 200) { // Arbitrary limit for simple stack-based triangulation
		TRACE("fill_convex_polygon: Too many vertices (%lu) for simple VLA-based triangulation stub.\n", num_vertices);
		return;
	}

	// Simple Triangulation: Create a triangle fan from V0.
	// (V0,V1,V2), (V0,V2,V3), ..., (V0,V(n-2),V(n-1))
	// This requires (num_vertices - 2) triangles.
	if (num_vertices >= 3) {
		uint32 num_triangles = num_vertices - 2;
		// Using VLA for simplicity in stub; real code might use malloc or batching.
		uint32 num_triangles = num_vertices - 2;
		fill_triangle_params current_triangle;

		TRACE("  fill_convex_polygon: Triangulating into %lu triangles (fan from V0).\n", num_triangles);

		for (uint32 i = 0; i < num_triangles; i++) {
			// Triangle V0, V(i+1), V(i+2)
			current_triangle.x1 = coords[0];                    // V0.x
			current_triangle.y1 = coords[1];                    // V0.y
			current_triangle.x2 = coords[(i + 1) * 2 + 0];      // V(i+1).x
			current_triangle.y2 = coords[(i + 1) * 2 + 1];      // V(i+1).y
			current_triangle.x3 = coords[(i + 2) * 2 + 0];      // V(i+2).x
			current_triangle.y3 = coords[(i + 2) * 2 + 1];      // V(i+2).y

			TRACE("    Triangle %lu: (%d,%d)-(%d,%d)-(%d,%d)\n", i,
				current_triangle.x1, current_triangle.y1,
				current_triangle.x2, current_triangle.y2,
				current_triangle.x3, current_triangle.y3);

			// Call the (currently stubbed) triangle list filler for this single triangle.
			// A more optimized version would collect all 'num_triangles' into an array
			// and make a single call to intel_i915_fill_triangle_list.
			intel_i915_fill_triangle_list(et, &current_triangle, 1, color, clip_rects, num_clip_rects);
		}
	}
	return;
}


// New function for drawing arbitrary lines using the 3D pipeline (conceptual stub)
void
intel_i915_draw_line_arbitrary(engine_token *et,
    const line_params *line, uint32 color,
    const general_rect* clip_rects, uint32 num_clip_rects)
{
    if (gInfo == NULL || gInfo->device_fd < 0 || line == NULL) {
        TRACE("draw_line_arbitrary: Invalid params or not initialized.\n");
        return;
    }

    // Check for zero-length line (draw as a point)
    if (line->x1 == line->x2 && line->y1 == line->y2) {
        fill_rect_params point_rect = {line->x1, line->y1, line->x1, line->y1};
        // Determine if clipping should be enabled based on num_clip_rects
        // The intel_i915_fill_rectangle function expects a boolean for enable_hw_clip.
        // If num_clip_rects > 0, we'd typically set up scissor/clip state.
        // For this fallback, we just pass a hint.
        intel_i915_fill_rectangle(et, color, &point_rect, 1, (num_clip_rects > 0));
        return;
    }

    // Fallback to existing H/V line drawer if applicable
    if (line->y1 == line->y2) { // Horizontal line
        uint16 hv_line_coords[4] = {(uint16)line->x1, (uint16)line->y1, (uint16)line->x2, (uint16)line->y2};
        // The intel_i915_draw_hv_lines function also expects a boolean for enable_hw_clip.
        // It internally converts lines to fill_rect_params.
        // We need to pass clipping information if we want the H/V lines to be clipped.
        // This might require intel_i915_draw_hv_lines to also take clip_rects or for
        // intel_i915_fill_rectangle to handle a list of clip_rects if that's how app_server works.
        // For now, indicating clipping might be active.
        intel_i915_draw_hv_lines(et, color, hv_line_coords, 1, (num_clip_rects > 0));
        return;
    }
    if (line->x1 == line->x2) { // Vertical line
        uint16 hv_line_coords[4] = {(uint16)line->x1, (uint16)line->y1, (uint16)line->x2, (uint16)line->y2};
        intel_i915_draw_hv_lines(et, color, hv_line_coords, 1, (num_clip_rects > 0));
        return;
    }

    // Angled line: requires 3D pipeline
    TRACE("draw_line_arbitrary: Angled line (%d,%d)-(%d,%d) color 0x%lx. 3D Pipe (STUBBED).\n",
        line->x1, line->y1, line->x2, line->y2, color);

    // --- Conceptual Geometric Calculation for a ~1px thick quad ---
    // These would be screen coordinates. The VS would transform them to clip space.
    float x1 = (float)line->x1;
    float y1 = (float)line->y1;
    float x2 = (float)line->x2;
    float y2 = (float)line->y2;

    float dx = x2 - x1;
    float dy = y2 - y1;

    // For a 1-pixel thick line, half_thickness is 0.5.
    // The extrusion direction depends on whether the line is more horizontal or vertical.
    float v_x0, v_y0, v_x1, v_y1, v_x2, v_y2, v_x3, v_y3;

    if (fabsf(dx) >= fabsf(dy)) { // More horizontal or equal (prefer horizontal for diagonal)
        // Extrude vertically by 0.5
        v_x0 = x1; v_y0 = y1 - 0.5f;
        v_x1 = x1; v_y1 = y1 + 0.5f;
        v_x2 = x2; v_y2 = y2 + 0.5f;
        v_x3 = x2; v_y3 = y2 - 0.5f;
    } else { // More vertical
        // Extrude horizontally by 0.5
        v_x0 = x1 - 0.5f; v_y0 = y1;
        v_x1 = x1 + 0.5f; v_y1 = y1;
        v_x2 = x2 + 0.5f; v_y2 = y2;
        v_x3 = x2 - 0.5f; v_y3 = y2;
    }

    TRACE("  Quad Vertices (conceptual screen coords):\n"
          "  V0: (%.2f, %.2f)\n  V1: (%.2f, %.2f)\n"
          "  V2: (%.2f, %.2f)\n  V3: (%.2f, %.2f)\n",
          v_x0, v_y0, v_x1, v_y1, v_x2, v_y2, v_x3, v_y3);

    // These vertices (v_x0,v_y0 ... v_x3,v_y3) would form the input
    // for the vertex buffer. They would typically be ordered to form two triangles, e.g.:
    // Triangle 1: V0, V1, V2
    // Triangle 2: V0, V2, V3
    // (Or using a triangle strip: V0, V1, V3, V2 if winding is correct)

    // --- Actual 3D Pipeline Implementation for Angled Lines (Conceptual) ---
	uint8_t gen = gInfo->shared_info->graphics_generation;

	// 1. Acquire Engine & Batch Buffer
	// ACQUIRE_ENGINE(et); // Assuming et is passed in and valid by the hook
	size_t cmd_dwords_estimate = 200; // Rough estimate
	size_t cmd_buffer_size = cmd_dwords_estimate * sizeof(uint32);
	uint32 cmd_handle = 0;
	area_id k_area_cmd = -1, c_area_cmd = -1;
	uint32* cs_start = NULL;
	uint32* cs = NULL;

	if (create_cmd_buffer(cmd_buffer_size, &cmd_handle, &k_area_cmd, (void**)&cs_start) != B_OK) {
		TRACE("draw_line_arbitrary: Failed to create command buffer.\n");
		// RELEASE_ENGINE(et, NULL);
		return;
	}
	c_area_cmd = area_for(cs_start);
	cs = cs_start;

	// --- Framebuffer Constants ---
	const uint32_t fb_gtt_offset = gInfo->shared_info->framebuffer_physical;
	const uint32_t fb_stride = gInfo->shared_info->bytes_per_row;
	const uint16_t fb_total_width = gInfo->shared_info->current_mode.virtual_width;
	const uint16_t fb_total_height = gInfo->shared_info->current_mode.virtual_height;
	const uint32_t fb_format_hw = get_surface_format_hw_value(gInfo->shared_info->current_mode.space);
	const bool fb_is_tiled = (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE);
	const uint32_t fb_tile_mode_hw = fb_is_tiled ? (gInfo->shared_info->fb_tiling_mode == I915_TILING_X ? 1 : 2) : 0;

	// --- Shader Kernels (Conceptual - these would be GEM BOs) ---
	// uint32_t vs_kernel_gtt_offset = get_solid_color_vs_gtt_offset(gen);
	// uint32_t ps_kernel_gtt_offset = get_solid_color_ps_gtt_offset(gen);
	TRACE("    Conceptual: Would get GTT offsets for solid color VS & PS kernels.\n");


	// --- Vertex Buffer for the Quad ---
	scaled_blit_vertex vb_data[4]; // Using scaled_blit_vertex for {x,y,z,w, u,v}
	                               // For solid lines, u,v are not strictly needed but VS might expect them.
	                               // A simpler vertex {x,y} might be used if VS is adapted.
	vb_data[0] = (scaled_blit_vertex){v_x0, v_y0, 0.0f, 1.0f, 0.0f, 0.0f};
	vb_data[1] = (scaled_blit_vertex){v_x1, v_y1, 0.0f, 1.0f, 0.0f, 0.0f};
	vb_data[2] = (scaled_blit_vertex){v_x2, v_y2, 0.0f, 1.0f, 0.0f, 0.0f}; // For TRIANGLE_STRIP V3,V2 order
	vb_data[3] = (scaled_blit_vertex){v_x3, v_y3, 0.0f, 1.0f, 0.0f, 0.0f}; // For TRIANGLE_STRIP V0,V1,V3,V2

	uint32_t vb_handle = 0;
	area_id k_area_vb = -1;
	void* cpu_addr_vb = NULL;
	uint64_t vb_gtt_offset = 0;
	status_t vb_status = create_and_upload_gem_bo(vb_data, sizeof(vb_data),
		I915_BO_ALLOC_CACHING_WC, /* suitable flags for VB */
		&vb_handle, &k_area_vb, &cpu_addr_vb, &vb_gtt_offset);

	if (vb_status != B_OK) {
		TRACE("draw_line_arbitrary: Failed to create/upload vertex buffer: %s\n", strerror(vb_status));
		destroy_cmd_buffer(cmd_handle, c_area_cmd, cs_start);
		// RELEASE_ENGINE(et, NULL);
		return;
	}
	TRACE("    Conceptual: Vertex Buffer GTT offset 0x%llx\n", vb_gtt_offset);


	// --- Conceptual Command Stream Construction ---
	// This is a highly simplified sequence. Many DWords and specific values are omitted.
	// Refer to Intel PRMs (Gen7.5+ Vol 2a, 2d, Vol 7) for actual command details.

	// Example: Minimal state setup (many commands omitted for brevity)
	// *cs++ = CMD_PIPELINE_SELECT | PIPELINE_SELECT_3D; (If not default)
	// *cs++ = CMD_STATE_BASE_ADDRESS ... (setup surface state base, dynamic state base, etc.)
	// *cs++ = CMD_3DSTATE_BINDING_TABLE_POINTERS ... (point to a simple binding table for RT)
	// *cs++ = CMD_RENDER_SURFACE_STATE ... (setup for framebuffer as RT, written to SSB)
	// *cs++ = CMD_3DSTATE_VIEWPORT_STATE_POINTERS ...
	// *cs++ = CMD_3DSTATE_SCISSOR_STATE_POINTERS ... (set to clip_rects or dest line bounds)
	// if (num_clip_rects > 0) {
	//    *cs++ = GEN7_3DSTATE_SCISSOR_RECTANGLE_ENABLE | (3-2);
	//    *cs++ = (clip_rects[0].left & 0xFFFF) | ((clip_rects[0].top & 0xFFFF) << 16);
	//    *cs++ = (clip_rects[0].right & 0xFFFF) | ((clip_rects[0].bottom & 0xFFFF) << 16);
	// } else { /* Set to full framebuffer */ }
	// *cs++ = CMD_3DSTATE_VS ... (point to VS kernel)
	// *cs++ = CMD_3DSTATE_PS ... (point to PS kernel, setup constants for 'color')
	// *cs++ = CMD_3DSTATE_CONSTANT_PS ... (load 'color' into PS constants)
	//    uint32_t r = (color >> 16) & 0xff; uint32_t g = (color >> 8) & 0xff; uint32_t b = color & 0xff; uint32_t a = (color >> 24) & 0xff;
	//    float fc[4] = { r/255.f, g/255.f, b/255.f, a/255.f };
	//    memcpy(cs, fc, sizeof(fc)); cs += 4; // Example for loading 1 vec4 constant
	// *cs++ = CMD_3DSTATE_VERTEX_BUFFERS ... (point to vb_gtt_offset, stride=sizeof(scaled_blit_vertex))
	// *cs++ = CMD_3DSTATE_VERTEX_ELEMENTS ... (describe position element R32G32_FLOAT or R32G32B32A32_FLOAT)
	// *cs++ = CMD_3DSTATE_BLEND_STATE_POINTERS ... (opaque blend)
	// *cs++ = CMD_3DSTATE_DEPTH_BUFFER_TYPE_NULL ... (disable depth)

	// *cs++ = CMD_3DPRIMITIVE | _3DPRIM_TRIANGLESTRIP | (num_dwords_for_prim_cmd - 2);
	// *cs++ = 4; // Vertex count for strip (V0,V1,V3,V2)
	// *cs++ = 0; // Start vertex
	// *cs++ = 1; // Instance count
	// *cs++ = 0; // Start instance

	TRACE("    Conceptual: Emitted 3D pipeline state and 3DPRIMITIVE for line quad.\n");
	// Fallback to ensure batch ends for stub
	cs = emit_pipe_control(cs, PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH | PIPE_CONTROL_CS_STALL, 0,0,0);
	*cs++ = MI_BATCH_BUFFER_END;
	// --- End Conceptual Command Stream ---

	uint32_t current_batch_len_dwords = cs - cs_start;
	intel_i915_gem_execbuffer_args exec_args = { cmd_handle, current_batch_len_dwords * sizeof(uint32), RCS0 };
	// TODO: Add relocations for vb_gtt_offset, shader_gtt_offsets, state_buffer_gtt_offsets

	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args)) != 0) {
		TRACE("draw_line_arbitrary: EXECBUFFER failed.\n");
	}

	destroy_cmd_buffer(cmd_handle, c_area_cmd, cs_start);
	if (vb_handle != 0) { // Destroy the temporary Vertex Buffer GEM object
		intel_i915_gem_close_args close_args = { vb_handle };
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args, sizeof(close_args));
	}
	// RELEASE_ENGINE(et, NULL);
    return;
}


void intel_i915_fill_rectangle(engine_token *et, uint32 color, fill_rect_params *list, uint32 count,
    //    - Scissor should be set based on clip_rects if num_clip_rects > 0.
    //      If multiple clip_rects, this implies multiple draw calls or complex stencil.
    //      For simplicity, assume for now only the first clip_rect is used if num_clip_rects > 0.
    // 4. Emit 3DPRIMITIVE to draw the quad.
    // 5. Emit PIPE_CONTROL for sync.
    // 6. Submit Batch Buffer.
    // 7. Release Engine.

    // No software fallback implemented here for angled lines.
    return;
}


// Intel Blitter Command Definitions
#define BLT_DEPTH_8			(0 << 24)
#define BLT_DEPTH_16_565	(1 << 24)
#define BLT_DEPTH_16_1555	(2 << 24)
#define BLT_DEPTH_32		(3 << 24)
#define BLT_ROP_PATCOPY		(0xF0 << 16)
#define BLT_ROP_SRCCOPY		(0xCC << 16)
#define BLT_ROP_DSTINVERT	(0x55 << 16)
// For XY_COLOR_BLT & XY_SRC_COPY_BLT on Gen7+, DW0 bits:
#define BLT_WRITE_RGB		(1 << 20) // Bit 20: RGB Write Enable
#define BLT_WRITE_ALPHA		(1 << 21) // Bit 21: Alpha Write Enable


#define MI_BATCH_BUFFER_END	(0x0A000000)

#define GFX_OP_PIPE_CONTROL_CMD	(0x3 << 29 | 0x3 << 27 | 0x2 << 24)
#define PIPE_CONTROL_LEN(len)	((len) - 2)
#define PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH	(1 << 12)
#define PIPE_CONTROL_CS_STALL                   (1 << 20)

#define XY_COLOR_BLT_CMD_OPCODE		(0x50 << 22)
#define XY_COLOR_BLT_LENGTH		(5 - 2)

#define XY_SRC_COPY_BLT_CMD_OPCODE	(0x53 << 22)
#define XY_SRC_COPY_BLT_LENGTH		(6 - 2)
// For XY_SRC_COPY_BLT_CMD (Gen4-Gen7 documented), DW0 bit 19 for Chroma Key Enable.
// Gen8+ needs PRM verification if this bit/mechanism changed for XY_SRC_COPY_BLT.
#define XY_SRC_COPY_BLT_CHROMA_KEY_ENABLE (1 << 19)


// Tiling bits for XY_COLOR_BLT (Destination)
#define XY_COLOR_BLT_DST_TILED_GEN7		(1 << 11)
// Tiling bits for XY_SRC_COPY_BLT (Destination & Source)
#define XY_SRC_COPY_BLT_DST_TILED_GEN7	(1 << 11)
#define XY_SRC_COPY_BLT_SRC_TILED_GEN7	(1 << 15)

// Note: For Gen8/Gen9, PRM checks are needed to confirm if these bits are identical.
// Initial assumption is they are similar for XY blits.

static void
_log_tiling_generalization_status()
{
	static bool status_logged = false;
	if (!status_logged && gInfo && gInfo->shared_info) {
		uint8_t gen = gInfo->shared_info->graphics_generation;
		if (gen == 7) {
			syslog(LOG_INFO, "intel_i915_accelerant_2d: Using Gen7 specific tiling logic for XY blits.");
		} else if (gen == 8 || gen == 9) {
			syslog(LOG_INFO, "intel_i915_accelerant_2d: Using Gen7-like tiling logic for Gen %u XY blits. PRM verification strongly recommended.", gen);
		} else if (gen > 9) {
			syslog(LOG_WARNING, "intel_i915_accelerant_2d: WARNING! Tiling command flags for Gen %u are UNKNOWN and thus DISABLED for XY blits. Surface tiling properties are still set by kernel.", gen);
		} else if (gen != 0 && gen < 7) {
			syslog(LOG_INFO, "intel_i915_accelerant_2d: Tiling command flags for Gen %u (pre-Gen7) are not explicitly set by this accelerant for XY blits.", gen);
		}
		status_logged = true;
	}
}

// New function to handle drawing horizontal/vertical lines as thin rectangles
void
intel_i915_draw_hv_lines(engine_token *et, uint32 color,
	uint16 *line_coords, uint32 num_lines, bool enable_hw_clip) // Added enable_hw_clip
{
	if (gInfo == NULL || gInfo->device_fd < 0 || num_lines == 0 || line_coords == NULL)
		return;

	fill_rect_params* rect_list = (fill_rect_params*)malloc(num_lines * sizeof(fill_rect_params));
	if (rect_list == NULL) {
		TRACE("draw_hv_lines: Failed to allocate memory for rect_list\n");
		return;
	}

	uint32 num_rects = 0;
	for (uint32 i = 0; i < num_lines; i++) {
		uint16 x1 = line_coords[i * 4 + 0];
		uint16 y1 = line_coords[i * 4 + 1];
		uint16 x2 = line_coords[i * 4 + 2];
		uint16 y2 = line_coords[i * 4 + 3];

		if (y1 == y2) { // Horizontal line
			rect_list[num_rects].left = min_c(x1, x2);
			rect_list[num_rects].top = y1;
			rect_list[num_rects].right = max_c(x1, x2);
			rect_list[num_rects].bottom = y1;
			num_rects++;
		} else if (x1 == x2) { // Vertical line
			rect_list[num_rects].left = x1;
			rect_list[num_rects].top = min_c(y1, y2);
			rect_list[num_rects].right = x1;
			rect_list[num_rects].bottom = max_c(y1, y2);
			num_rects++;
		} // Diagonal lines skipped by this function
	}

	if (num_rects > 0) {
		// Pass enable_hw_clip to the underlying fill_rectangle
		intel_i915_fill_rectangle(et, color, rect_list, num_rects, enable_hw_clip);
	}

	free(rect_list);
}

static uint32
get_blit_colordepth_flags(uint16 bits_per_pixel, color_space format)
{
	switch (format) {
		case B_CMAP8: return BLT_DEPTH_8;
		case B_RGB15: case B_RGBA15: case B_RGB15_BIG: case B_RGBA15_BIG: return BLT_DEPTH_16_1555;
		case B_RGB16: case B_RGB16_BIG: return BLT_DEPTH_16_565;
		case B_RGB24_BIG: case B_RGB32: case B_RGBA32: case B_RGB32_BIG: case B_RGBA32_BIG: return BLT_DEPTH_32;
		default:
			TRACE("get_blit_colordepth_flags: Unknown color space %d, bpp %d. Defaulting to 32bpp flags.\n", format, bits_per_pixel);
			return BLT_DEPTH_32;
	}
}

static uint32*
emit_pipe_control_render_stall(uint32* cs)
{
	cs[0] = GFX_OP_PIPE_CONTROL_CMD | PIPE_CONTROL_LEN(4);
	cs[1] = PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH | PIPE_CONTROL_CS_STALL;
	cs[2] = 0; cs[3] = 0;
	return cs + 4;
}

static status_t
create_cmd_buffer(size_t size, uint32* handle_out, area_id* area_out, void** cpu_addr_out)
{
	if (gInfo == NULL || gInfo->device_fd < 0) return B_NO_INIT;
	intel_i915_gem_create_args create_args = { .size = size, .flags = I915_BO_ALLOC_CPU_CLEAR };
	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CREATE, &create_args, sizeof(create_args)) != 0) {
		TRACE("create_cmd_buffer: GEM_CREATE failed\n"); return B_ERROR;
	}
	*handle_out = create_args.handle;
	intel_i915_gem_mmap_area_args mmap_args = { .handle = *handle_out };
	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_MMAP_AREA, &mmap_args, sizeof(mmap_args)) != 0) {
		TRACE("create_cmd_buffer: GEM_MMAP_AREA failed for handle %lu\n", *handle_out);
		intel_i915_gem_close_args close_args = { *handle_out };
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args, sizeof(close_args));
		return B_ERROR;
	}
	*area_out = mmap_args.map_area_id;
	void* addr_temp;
	area_id cloned_area = clone_area("cmd_buffer_clone", &addr_temp, B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, *area_out);
	if (cloned_area < B_OK) {
		TRACE("create_cmd_buffer: failed to clone area %" B_PRId32 "\n", *area_out);
		intel_i915_gem_close_args close_args = { *handle_out };
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args, sizeof(close_args));
		return cloned_area;
	}
	*cpu_addr_out = addr_temp;
	// TRACE("create_cmd_buffer: handle %lu, area %" B_PRId32 ", cpu_addr %p, size %llu\n",
	//	*handle_out, *area_out, *cpu_addr_out, mmap_args.size); // Too verbose for general use
	return B_OK;
}

static void
destroy_cmd_buffer(uint32 handle, area_id cloned_cmd_area, void* cpu_addr)
{
	if (gInfo == NULL || gInfo->device_fd < 0) return;
	if (cloned_cmd_area >= B_OK) delete_area(cloned_cmd_area);
	if (handle != 0) {
		intel_i915_gem_close_args close_args = { handle };
		ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_CLOSE, &close_args, sizeof(close_args));
	}
}

void
intel_i915_fill_span(engine_token *et, uint32 color, uint16 *list, uint32 count, bool enable_hw_clip)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0) return;
	_log_tiling_generalization_status(); // Logs generation-specific tiling info once
	uint8_t gen = gInfo->shared_info->graphics_generation;

	const size_t max_ops_per_batch = 160;
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_ops_per_batch), max_ops_per_batch);
		size_t cmd_dwords_per_span = 5;
		size_t pipe_control_dwords = 4;
		size_t cmd_dwords = (current_batch_count * cmd_dwords_per_span) + pipe_control_dwords + 1;
		size_t cmd_buffer_size = cmd_dwords * sizeof(uint32);

		uint32 cmd_handle; area_id k_area, c_area = -1; uint32* cpu_buf;
		if (create_cmd_buffer(cmd_buffer_size, &cmd_handle, &k_area, (void**)&cpu_buf) != B_OK) return;
		c_area = area_for(cpu_buf);

		uint32 cur_dw_idx = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			uint16 y = list[(batch * max_ops_per_batch + i) * 3 + 0];
			uint16 x1 = list[(batch * max_ops_per_batch + i) * 3 + 1];
			uint16 x2 = list[(batch * max_ops_per_batch + i) * 3 + 2];
			if (x1 >= x2) continue;

			uint32 cmd_dw0 = XY_COLOR_BLT_CMD_OPCODE | XY_COLOR_BLT_LENGTH | BLT_ROP_PATCOPY;
			uint32 depth_flags = get_blit_colordepth_flags(gInfo->shared_info->current_mode.bits_per_pixel, gInfo->shared_info->current_mode.space);
			cmd_dw0 |= depth_flags;
			if (depth_flags == BLT_DEPTH_32) cmd_dw0 |= BLT_WRITE_RGB;
			if (enable_hw_clip) {
				cmd_dw0 |= BLT_CLIPPING_ENABLE;
			}

			if (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE) {
				// Apply tiling flags for known/assumed compatible generations
				if (gen == 7 || gen == 8 || gen == 9) {
					cmd_dw0 |= XY_COLOR_BLT_DST_TILED_GEN7;
				}
				// For gen > 9, specific tiling flags are unknown, so not set.
				// For gen < 7, XY_COLOR_BLT_DST_TILED_GEN7 is not applicable.
			}
			cpu_buf[cur_dw_idx++] = cmd_dw0;
			cpu_buf[cur_dw_idx++] = gInfo->shared_info->bytes_per_row;
			cpu_buf[cur_dw_idx++] = (x1 & 0xFFFF) | ((y & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = (x2 & 0xFFFF) | (((y + 1) & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = color;
		}
		if (cur_dw_idx == 0) { destroy_cmd_buffer(cmd_handle, c_area, cpu_buf); continue; }
		uint32* p = emit_pipe_control_render_stall(cpu_buf + cur_dw_idx); *p = MI_BATCH_BUFFER_END; cur_dw_idx = (p - cpu_buf) + 1;

		intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), RCS0 };
		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args)) != 0) TRACE("fill_span: EXECBUFFER failed.\n");
		destroy_cmd_buffer(cmd_handle, c_area, cpu_buf);
	}
}

typedef struct { uint16 src_left, src_top, src_width, src_height, dest_left, dest_top, dest_width, dest_height; } scaled_blit_params;

static void
intel_i915_screen_to_screen_transparent_blit(engine_token *et, uint32 transparent_color,
	blit_params *list, uint32 count, bool enable_hw_clip)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0) return;
	_log_tiling_generalization_status();
	uint8_t gen = gInfo->shared_info->graphics_generation;

	intel_i915_set_blitter_chroma_key_args ck_args;
	ck_args.low_color = transparent_color;
	ck_args.high_color = transparent_color;
	// This mask (0x7 for RGB) needs PRM verification for specific color formats (e.g. BGR vs RGB)
	// and what the hardware expects. Assuming RGB for now.
	ck_args.mask = 0x00FFFFFF; // Enable check for R, G, B. Alpha ignored.
	ck_args.enable = true;

	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_CHROMA_KEY, &ck_args, sizeof(ck_args)) != 0) {
		TRACE("s2s_transparent_blit: Failed to set chroma key via IOCTL. Falling back to normal blit.\n");
		intel_i915_screen_to_screen_blit(et, list, count);
		return;
	}

	const size_t max_ops_per_batch = 160;
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_ops_per_batch), max_ops_per_batch);
		size_t cmd_dwords_per_blit = 6;
		size_t pipe_control_dwords = 4;
		size_t cmd_dwords = (current_batch_count * cmd_dwords_per_blit) + pipe_control_dwords + 1;
		size_t cmd_buffer_size = cmd_dwords * sizeof(uint32);

		uint32 cmd_handle; area_id k_area, c_area = -1; uint32* cpu_buf;
		if (create_cmd_buffer(cmd_buffer_size, &cmd_handle, &k_area, (void**)&cpu_buf) != B_OK) {
			TRACE("s2s_transparent_blit: Failed to create command buffer for batch %zu.\n", batch);
			goto cleanup_chroma_key;
		}
		c_area = area_for(cpu_buf);

		uint32 cur_dw_idx = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			blit_params *blit = &list[batch * max_ops_per_batch + i];

			uint32 cmd_dw0 = XY_SRC_COPY_BLT_CMD_OPCODE | XY_SRC_COPY_BLT_LENGTH | BLT_ROP_SRCCOPY;
			if (gen >= 4) { // Chroma Key Enable bit (19) is documented for Gen4+ XY_SRC_COPY_BLT
				cmd_dw0 |= XY_SRC_COPY_BLT_CHROMA_KEY_ENABLE;
			}
			if (enable_hw_clip) {
				cmd_dw0 |= BLT_CLIPPING_ENABLE;
			}

			uint32 depth_flags = get_blit_colordepth_flags(gInfo->shared_info->current_mode.bits_per_pixel, gInfo->shared_info->current_mode.space);
			cmd_dw0 |= depth_flags;
			if (depth_flags == BLT_DEPTH_32) cmd_dw0 |= BLT_WRITE_RGB;

			if (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE) {
				// Apply tiling flags for known/assumed compatible generations
				if (gen == 7 || gen == 8 || gen == 9) {
					cmd_dw0 |= XY_SRC_COPY_BLT_DST_TILED_GEN7;
					cmd_dw0 |= XY_SRC_COPY_BLT_SRC_TILED_GEN7;
				}
				// For gen > 9, specific tiling flags are unknown, so not set.
				// For gen < 7, these specific Gen7+ flags are not applicable.
			}
			cpu_buf[cur_dw_idx++] = cmd_dw0;
			cpu_buf[cur_dw_idx++] = gInfo->shared_info->bytes_per_row;
			cpu_buf[cur_dw_idx++] = (blit->dest_left & 0xFFFF) | ((blit->dest_top & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = ((blit->dest_left + blit->width) & 0xFFFF) | (((blit->dest_top + blit->height) & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = gInfo->shared_info->framebuffer_physical;
			cpu_buf[cur_dw_idx++] = (blit->src_left & 0xFFFF) | ((blit->src_top & 0xFFFF) << 16);
		}

		if (cur_dw_idx == 0) { destroy_cmd_buffer(cmd_handle, c_area, cpu_buf); continue; }
		uint32* p = emit_pipe_control_render_stall(cpu_buf + cur_dw_idx); *p = MI_BATCH_BUFFER_END; cur_dw_idx = (p - cpu_buf) + 1;

		intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), RCS0 };
		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args)) != 0) {
			TRACE("s2s_transparent_blit: EXECBUFFER failed for batch %zu.\n", batch);
		}
		destroy_cmd_buffer(cmd_handle, c_area, cpu_buf);
	}

cleanup_chroma_key:
	ck_args.enable = false;
	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_SET_BLITTER_CHROMA_KEY, &ck_args, sizeof(ck_args)) != 0) {
		TRACE("s2s_transparent_blit: Failed to disable chroma key via IOCTL.\n");
	}
}

static void
intel_i915_screen_to_screen_scaled_filtered_blit(engine_token* et,
    scaled_blit_params *list, uint32 count, bool enable_hw_clip)
{
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0) {
		TRACE("s2s_scaled_blit: No gInfo or no ops.\n");
		return;
	}
	_log_tiling_generalization_status();
	// uint8_t gen = gInfo->shared_info->graphics_generation; // For future use

	// IMPORTANT: This function is a conceptual outline.
	// Actual implementation requires deep PRM knowledge for setting up:
	// 1. Surface states (for source and destination)
	// 2. Sampler states (for bilinear filtering from source)
	// 3. Vertex formats and vertex buffers (defining the destination quad)
	// 4. Shader programs (vertex and fragment shaders for texture sampling)
	// 5. Viewport, Scissor, Blend states, etc.
	// 6. Binding table entries to link shaders to surfaces.
	// This is far more complex than XY_SRC_COPY_BLT and typically uses the Render Command Streamer (RCS).

	TRACE("s2s_scaled_filtered_blit: %lu ops. HW Accel for this is COMPLEX and NOT fully implemented - conceptual outline only.\n", count);

	// Fallback: Perform an UN SCALED blit for the first item as a placeholder.
	// This makes the function somewhat testable without full RCS programming.
	// A real driver might fall back to software scaling if HW is too complex or unavailable.
	if (count > 0) {
		blit_params unscaled_op;
		unscaled_op.src_left = list[0].src_left;
		unscaled_op.src_top = list[0].src_top;
		unscaled_op.dest_left = list[0].dest_left;
		unscaled_op.dest_top = list[0].dest_top;
		// Use the SMALLER of src/dest width/height for this unscaled example to ensure it fits
		unscaled_op.width = min_c(list[0].src_width, list[0].dest_width);
		unscaled_op.height = min_c(list[0].src_height, list[0].dest_height);

		if (unscaled_op.width > 0 && unscaled_op.height > 0) {
			TRACE("s2s_scaled_filtered_blit: Performing an UN SCALED blit for the first item (size %dx%d) as a placeholder.\n",
				unscaled_op.width, unscaled_op.height);
			// Use the existing screen_to_screen_blit with enable_hw_clip flag
			intel_i915_screen_to_screen_blit(et, &unscaled_op, 1, enable_hw_clip);
		} else {
			TRACE("s2s_scaled_filtered_blit: Placeholder unscaled blit for first item resulted in zero dimension.\n");
		}
		// For a full implementation, one would loop through all 'count' items.
	}
	// Due to complexity and need for PRM, full RCS-based implementation is deferred.
	// A real implementation would loop through 'count' items, potentially batching them.
	// For this conceptual version, we'll outline for one item.
	if (count == 0) return;
	scaled_blit_params* item = &list[0]; // Process first item for concept

	// --- Acquire Engine & Batch Buffer ---
	// This would typically be done once if batching multiple blits, or per blit if not.
	// ACQUIRE_ENGINE(et); // Assuming et is passed in and valid

	size_t cmd_dwords_estimate = 256; // Increased estimate for 3D states
	size_t cmd_buffer_size = cmd_dwords_estimate * sizeof(uint32);
	uint32 cmd_handle;
	area_id k_area_cmd, c_area_cmd = -1;
	uint32* cs_start;

	if (create_cmd_buffer(cmd_buffer_size, &cmd_handle, &k_area_cmd, (void**)&cs_start) != B_OK) {
		TRACE("s2s_scaled_blit: Failed to create command buffer for item.\n");
		// RELEASE_ENGINE(et, NULL);
		return;
	}
	c_area_cmd = area_for(cs_start);
	uint32* cs = cs_start;

	// --- Shader Binaries (Conceptual Placeholders) ---
	// These would be actual pre-compiled GPU machine code byte arrays.
	// For simplicity, assume functions exist to get their GTT offset and size.
	// uint32_t vs_kernel_gtt_offset = get_vs_kernel_gtt_offset(gen);
	// uint32_t ps_kernel_gtt_offset = get_ps_kernel_gtt_offset(gen);
	// uint32_t vs_kernel_size = get_vs_kernel_size(gen);
	// uint32_t ps_kernel_size = get_ps_kernel_size(gen);
	TRACE("s2s_scaled_blit: Placeholder: Would load VS & PS kernels here.\n");


	// --- Framebuffer Constants ---
	const uint32_t fb_gtt_offset = gInfo->shared_info->framebuffer_physical;
	const uint32_t fb_stride = gInfo->shared_info->bytes_per_row;
	const uint16_t fb_total_width = gInfo->shared_info->current_mode.virtual_width;
	const uint16_t fb_total_height = gInfo->shared_info->current_mode.virtual_height;
	const uint32_t fb_format_hw = get_surface_format_hw_value(gInfo->shared_info->current_mode.space);
	const bool fb_is_tiled = (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE);
	const uint32_t fb_tile_mode_hw = fb_is_tiled ? (gInfo->shared_info->fb_tiling_mode == I915_TILING_X ? 1 : 2) : 0;

	// --- Conceptual Command Stream Construction (Highly Simplified) ---
	// This sequence is illustrative and GEN-dependent. Many details omitted.

	// 1. Pipeline Select (if needed, often part of context state)
	// *cs++ = CMD_PIPELINE_SELECT | PIPELINE_SELECT_3D;

	// 2. STATE_BASE_ADDRESS
	//    Program GTT offsets for various state heaps (general, surface, dynamic, instruction)
	//    Example: *cs++ = CMD_STATE_BASE_ADDRESS | (num_dwords - 2); *cs++ = surface_state_heap_gtt_offset; ...
	TRACE("s2s_scaled_blit: Placeholder: Emit STATE_BASE_ADDRESS.\n");


	// 3. Surface States (RENDER_SURFACE_STATE)
	//    These structs would be written to a GEM BO (Surface State Buffer - SSB),
	//    and the binding table would contain pointers (offsets within SSB) to them.
	//    For this example, we imagine directly embedding simplified state or using macros.
	//    Binding Table Index 0: Source (Framebuffer as Texture)
	//    Binding Table Index 1: Destination (Framebuffer as Render Target)
	TRACE("s2s_scaled_blit: Placeholder: Setup RENDER_SURFACE_STATE for src & dst in SSB.\n");
	//    Example conceptual fields for source surface state (texture):
	//    - Surface Type: 2D
	//    - Surface Format: fb_format_hw
	//    - Base Address: fb_gtt_offset
	//    - Width: fb_total_width - 1
	//    - Height: fb_total_height - 1
	//    - Pitch: fb_stride - 1
	//    - Tiling: fb_tile_mode_hw
	//    - Shader Channel Select: R, G, B, A from surface
	//    Example conceptual fields for destination surface state (render target):
	//    - Similar to source, but with Render Target flag set.

	// 4. Sampler State (SAMPLER_STATE)
	//    Written to a dynamic state area or SSB. Defines filtering and addressing.
	TRACE("s2s_scaled_blit: Placeholder: Setup SAMPLER_STATE for bilinear filtering & CLAMP_TO_EDGE.\n");
	//    Example fields: Min/Mag Filter = LINEAR, Mip Filter = NONE, AddrU/V/W = CLAMP_TO_EDGE.

	// 5. Binding Table Setup (3DSTATE_BINDING_TABLE_POINTERS)
	//    Points to an array of BINDING_TABLE_STATE entries in memory.
	//    Entry 0 -> Source Surface State offset in SSB.
	//    Entry (for sampler, if separate) -> Sampler State offset.
	TRACE("s2s_scaled_blit: Placeholder: Setup BINDING_TABLE_POINTERS.\n");

	// 6. Shader Program Pointers (3DSTATE_VS, 3DSTATE_PS, etc.)
	//    Points to shader kernels (already in GEM objects).
	//    Also 3DSTATE_CONSTANT_PS/VS if using constants.
	//    And MEDIA_VFE_STATE for thread dispatch config, URB setup.
	TRACE("s2s_scaled_blit: Placeholder: Setup VS, PS, MEDIA_VFE_STATE, etc.\n");

	// 7. Vertex Data & Primitives
	//    Define a quad covering the destination rectangle. Texture coordinates map the source rectangle.
	//    scaled_blit_vertex vb_data[4]; // For a quad (e.g. two triangles)
	//    vb_data[0].x = item->dest_left; vb_data[0].y = item->dest_top;
	//    vb_data[0].u = (float)item->src_left / fb_total_width;
	//    vb_data[0].v = (float)item->src_top / fb_total_height;
	//    ... (define other 3 vertices similarly for a rectangle)
	//    Upload vb_data to a GEM object (Vertex Buffer - VB).
	//    uint32_t vb_gtt_offset = upload_vertices_to_gem_and_get_offset(vb_data, ...);
	//    3DSTATE_VERTEX_BUFFERS: points to VB, specifies stride.
	//    3DSTATE_VERTEX_ELEMENTS: defines vertex layout (position XYZW, texcoord UV).
	TRACE("s2s_scaled_blit: Placeholder: Setup Vertex Buffers and Vertex Elements.\n");

	// 8. Viewport, Scissor, Blend, Depth States
	//    3DSTATE_VIEWPORT_STATE_POINTERS / SF_CLIP_VIEWPORT: Set to destination dimensions.
	//    3DSTATE_SCISSOR_STATE_POINTERS / 3DSTATE_SCISSOR_RECTANGLE: Set to destination rect.
	//      If enable_hw_clip is true, this scissor is further ANDed with the global clip rect
	//      (though that global clip rect is for 2D blitter, 3D pipeline uses its own scissor).
	//    Blend state: Typically disabled for opaque blit (all channels write enabled, func_add, src_one, dst_zero).
	//    Depth/Stencil state: Disabled.
	TRACE("s2s_scaled_blit: Placeholder: Setup Viewport, Scissor, Blend, Depth states.\n");

	// 9. 3DPRIMITIVE Command
	//    Draws the quad (e.g., _3DPRIM_RECTLIST or _3DPRIM_TRIANGLESTRIP).
	//    *cs++ = CMD_3DPRIMITIVE | _3DPRIM_RECTLIST | (num_vtx_elements_dwords - 2);
	//    *cs++ = 4; // Vertex count for RECTLIST
	//    *cs++ = 0; // Start vertex
	//    *cs++ = 1; // Instance count
	//    *cs++ = 0; // Start instance
	TRACE("s2s_scaled_blit: Placeholder: Emit 3DPRIMITIVE command.\n");


	// 10. Synchronization and Batch End
	cs = emit_pipe_control(cs, PIPE_CONTROL_RENDER_TARGET_CACHE_FLUSH | PIPE_CONTROL_CS_STALL, 0, 0, 0);
	*cs++ = MI_BATCH_BUFFER_END;

	uint32_t current_batch_len_dwords = cs - cs_start;
	if (current_batch_len_dwords > cmd_dwords_estimate) {
		TRACE("s2s_scaled_blit: WARNING - Exceeded estimated DWord count! Est: %zu, Actual: %u. Batch may be invalid.\n",
			cmd_dwords_estimate, current_batch_len_dwords);
	}

	intel_i915_gem_execbuffer_args exec_args = { cmd_handle, current_batch_len_dwords * sizeof(uint32), RCS0 };
	// TODO: Populate relocation list if GTT offsets of GEM BOs (SSB, shaders, VB) are patched into the batch.
	// exec_args.relocations_ptr = ...; exec_args.relocation_count = ...;

	if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args)) != 0) {
		TRACE("s2s_scaled_blit: EXECBUFFER failed for item %u.\n", op_idx);
	}
	destroy_cmd_buffer(cmd_handle, c_area_cmd, cs_start);

	// RELEASE_ENGINE(et, NULL); // Release engine after processing all items or if batching
	}
	// If not batching all items, ACQUIRE/RELEASE_ENGINE would be inside the loop.
}


void intel_i915_fill_rectangle(engine_token *et, uint32 color, fill_rect_params *list, uint32 count,
	bool enable_hw_clip) { // Added enable_hw_clip
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0) return;
	_log_tiling_generalization_status();
	uint8_t gen = gInfo->shared_info->graphics_generation;

	const size_t max_ops_per_batch = 160;
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_ops_per_batch), max_ops_per_batch);
		size_t cmd_dwords_per_rect = 5;
		size_t pipe_control_dwords = 4;
		size_t cmd_dwords = (current_batch_count * cmd_dwords_per_rect) + pipe_control_dwords + 1;
		size_t cmd_buffer_size = cmd_dwords * sizeof(uint32);

		uint32 cmd_handle; area_id k_area, c_area = -1; uint32* cpu_buf;
		if (create_cmd_buffer(cmd_buffer_size, &cmd_handle, &k_area, (void**)&cpu_buf) != B_OK) return;
		c_area = area_for(cpu_buf);

		uint32 cur_dw_idx = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			fill_rect_params *rect = &list[batch * max_ops_per_batch + i];
			if (rect->right < rect->left || rect->bottom < rect->top) continue;

			uint32 cmd_dw0 = XY_COLOR_BLT_CMD_OPCODE | XY_COLOR_BLT_LENGTH | BLT_ROP_PATCOPY;
			uint32 depth_flags = get_blit_colordepth_flags(gInfo->shared_info->current_mode.bits_per_pixel, gInfo->shared_info->current_mode.space);
			cmd_dw0 |= depth_flags;
			if (depth_flags == BLT_DEPTH_32) cmd_dw0 |= BLT_WRITE_RGB;
			if (enable_hw_clip) {
				cmd_dw0 |= BLT_CLIPPING_ENABLE;
			}

			if (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE) {
				// Apply tiling flags for known/assumed compatible generations
				if (gen == 7 || gen == 8 || gen == 9) {
					cmd_dw0 |= XY_COLOR_BLT_DST_TILED_GEN7;
				}
				// For gen > 9, specific tiling flags are unknown, so not set.
				// For gen < 7, XY_COLOR_BLT_DST_TILED_GEN7 is not applicable.
			}
			cpu_buf[cur_dw_idx++] = cmd_dw0;
			cpu_buf[cur_dw_idx++] = gInfo->shared_info->bytes_per_row;
			cpu_buf[cur_dw_idx++] = (rect->left & 0xFFFF) | ((rect->top & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = ((rect->right + 1) & 0xFFFF) | (((rect->bottom + 1) & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = color;
		}
		if (cur_dw_idx == 0) { destroy_cmd_buffer(cmd_handle, c_area, cpu_buf); continue; }
		uint32* p = emit_pipe_control_render_stall(cpu_buf + cur_dw_idx); *p = MI_BATCH_BUFFER_END; cur_dw_idx = (p - cpu_buf) + 1;

		intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), RCS0 };
		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args)) != 0) TRACE("fill_rectangle: EXECBUFFER failed.\n");
		destroy_cmd_buffer(cmd_handle, c_area, cpu_buf);
	}
}

void intel_i915_invert_rectangle(engine_token *et, fill_rect_params *list, uint32 count, bool enable_hw_clip) {
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0) return;
	_log_tiling_generalization_status();
	uint8_t gen = gInfo->shared_info->graphics_generation;

	const size_t max_ops_per_batch = 160;
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_ops_per_batch), max_ops_per_batch);
		size_t cmd_dwords_per_rect = 5;
		size_t pipe_control_dwords = 4;
		size_t cmd_dwords = (current_batch_count * cmd_dwords_per_rect) + pipe_control_dwords + 1;
		size_t cmd_buffer_size = cmd_dwords * sizeof(uint32);

		uint32 cmd_handle; area_id k_area, c_area = -1; uint32* cpu_buf;
		if (create_cmd_buffer(cmd_buffer_size, &cmd_handle, &k_area, (void**)&cpu_buf) != B_OK) return;
		c_area = area_for(cpu_buf);

		uint32 cur_dw_idx = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			fill_rect_params *rect = &list[batch * max_ops_per_batch + i];
			if (rect->right < rect->left || rect->bottom < rect->top) continue;

			uint32 cmd_dw0 = XY_COLOR_BLT_CMD_OPCODE | XY_COLOR_BLT_LENGTH | BLT_ROP_DSTINVERT;
			uint32 depth_flags = get_blit_colordepth_flags(gInfo->shared_info->current_mode.bits_per_pixel, gInfo->shared_info->current_mode.space);
			cmd_dw0 |= depth_flags;
			if (depth_flags == BLT_DEPTH_32) cmd_dw0 |= BLT_WRITE_RGB;
			if (enable_hw_clip) {
				cmd_dw0 |= BLT_CLIPPING_ENABLE;
			}

			if (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE) {
				// Apply tiling flags for known/assumed compatible generations
				if (gen == 7 || gen == 8 || gen == 9) {
					cmd_dw0 |= XY_COLOR_BLT_DST_TILED_GEN7;
				}
				// For gen > 9, specific tiling flags are unknown, so not set.
				// For gen < 7, XY_COLOR_BLT_DST_TILED_GEN7 is not applicable.
			}
			cpu_buf[cur_dw_idx++] = cmd_dw0;
			cpu_buf[cur_dw_idx++] = gInfo->shared_info->bytes_per_row;
			cpu_buf[cur_dw_idx++] = (rect->left & 0xFFFF) | ((rect->top & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = ((rect->right + 1) & 0xFFFF) | (((rect->bottom + 1) & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = 0; // Dummy color for DSTINVERT
		}
		if (cur_dw_idx == 0) { destroy_cmd_buffer(cmd_handle, c_area, cpu_buf); continue; }
		uint32* p = emit_pipe_control_render_stall(cpu_buf + cur_dw_idx); *p = MI_BATCH_BUFFER_END; cur_dw_idx = (p - cpu_buf) + 1;

		intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), RCS0 };
		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args)) != 0) TRACE("invert_rectangle: EXECBUFFER failed.\n");
		destroy_cmd_buffer(cmd_handle, c_area, cpu_buf);
	}
}

void intel_i915_screen_to_screen_blit(engine_token *et, blit_params *list, uint32 count, bool enable_hw_clip) {
	if (gInfo == NULL || gInfo->device_fd < 0 || count == 0) return;
	_log_tiling_generalization_status();
	uint8_t gen = gInfo->shared_info->graphics_generation;

	const size_t max_ops_per_batch = 160;
	size_t num_batches = (count + max_ops_per_batch - 1) / max_ops_per_batch;

	for (size_t batch = 0; batch < num_batches; batch++) {
		size_t current_batch_count = min_c(count - (batch * max_ops_per_batch), max_ops_per_batch);
		size_t cmd_dwords_per_blit = 6;
		size_t pipe_control_dwords = 4;
		size_t cmd_dwords = (current_batch_count * cmd_dwords_per_blit) + pipe_control_dwords + 1;
		size_t cmd_buffer_size = cmd_dwords * sizeof(uint32);

		uint32 cmd_handle; area_id k_area, c_area = -1; uint32* cpu_buf;
		if (create_cmd_buffer(cmd_buffer_size, &cmd_handle, &k_area, (void**)&cpu_buf) != B_OK) return;
		c_area = area_for(cpu_buf);

		uint32 cur_dw_idx = 0;
		for (size_t i = 0; i < current_batch_count; i++) {
			blit_params *blit = &list[batch * max_ops_per_batch + i];

			uint32 cmd_dw0 = XY_SRC_COPY_BLT_CMD_OPCODE | XY_SRC_COPY_BLT_LENGTH | BLT_ROP_SRCCOPY;
			uint32 depth_flags = get_blit_colordepth_flags(gInfo->shared_info->current_mode.bits_per_pixel, gInfo->shared_info->current_mode.space);
			cmd_dw0 |= depth_flags;
			if (depth_flags == BLT_DEPTH_32) cmd_dw0 |= BLT_WRITE_RGB;
			if (enable_hw_clip) {
				cmd_dw0 |= BLT_CLIPPING_ENABLE;
			}

			if (gInfo->shared_info->fb_tiling_mode != I915_TILING_NONE) {
				// Apply tiling flags for known/assumed compatible generations
				if (gen == 7 || gen == 8 || gen == 9) {
					cmd_dw0 |= XY_SRC_COPY_BLT_DST_TILED_GEN7;
					cmd_dw0 |= XY_SRC_COPY_BLT_SRC_TILED_GEN7;
				}
				// For gen > 9, specific tiling flags are unknown, so not set.
				// For gen < 7, these specific Gen7+ flags are not applicable.
			}
			cpu_buf[cur_dw_idx++] = cmd_dw0;
			cpu_buf[cur_dw_idx++] = gInfo->shared_info->bytes_per_row; // Dest pitch
			cpu_buf[cur_dw_idx++] = (blit->dest_left & 0xFFFF) | ((blit->dest_top & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = ((blit->dest_left + blit->width) & 0xFFFF) | (((blit->dest_top + blit->height) & 0xFFFF) << 16);
			cpu_buf[cur_dw_idx++] = gInfo->shared_info->framebuffer_physical; // Source GTT offset (assuming same FB)
			cpu_buf[cur_dw_idx++] = (blit->src_left & 0xFFFF) | ((blit->src_top & 0xFFFF) << 16);
		}
		if (cur_dw_idx == 0) { destroy_cmd_buffer(cmd_handle, c_area, cpu_buf); continue; }
		uint32* p = emit_pipe_control_render_stall(cpu_buf + cur_dw_idx); *p = MI_BATCH_BUFFER_END; cur_dw_idx = (p - cpu_buf) + 1;

		intel_i915_gem_execbuffer_args exec_args = { cmd_handle, cur_dw_idx * sizeof(uint32), RCS0 };
		if (ioctl(gInfo->device_fd, INTEL_I915_IOCTL_GEM_EXECBUFFER, &exec_args, sizeof(exec_args)) != 0) TRACE("s2s_blit: EXECBUFFER failed.\n");
		destroy_cmd_buffer(cmd_handle, c_area, cpu_buf);
	}
}
