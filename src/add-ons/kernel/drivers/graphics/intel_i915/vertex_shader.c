/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "vertex_shader.h"
#include "intel_i915_priv.h"

#include "gem_object.h"

#include "string.h"

status_t
intel_vertex_shader_init(intel_i915_device_info* devInfo)
{
	const char* shader_code =
		"vs.1.1\n"
		"dcl_position v0\n"
		"dcl_color v1\n"
		"mov oPos, v0\n"
		"mov oD0, v1\n";

	struct intel_i915_gem_object* obj;
	status_t status = intel_i915_gem_object_create(devInfo, strlen(shader_code), 0, 0, 0, 0, &obj);
	if (status != B_OK) {
		return status;
	}

	void* obj_buffer;
	status = intel_i915_gem_object_map_cpu(obj, &obj_buffer);
	if (status != B_OK) {
		intel_i915_gem_object_put(obj);
		return status;
	}

	memcpy(obj_buffer, shader_code, strlen(shader_code));

	intel_i915_gem_object_unmap_cpu(obj);
	intel_i915_gem_object_put(obj);

	return B_OK;
}

void
intel_vertex_shader_uninit(intel_i915_device_info* devInfo)
{
	// TODO: Implement vertex shader uninitialization.
}
