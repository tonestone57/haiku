/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "guc.h"
#include "intel_i915_priv.h"
#include "gem_object.h"

#include <FindDirectory.h>
#include <image.h>
#include <stdio.h>

#include "i915_platform_data.h"

status_t
intel_guc_init(intel_i915_device_info* devInfo)
{
	char path[256];
	snprintf(path, sizeof(path), "/lib/firmware/intel/%s_guc_ver%d_%d.bin",
		intel_platform_name(devInfo->platform),
		INTEL_GRAPHICS_GEN(devInfo->runtime_caps.device_id),
		devInfo->runtime_caps.revision_id);

	FILE* fp = fopen(path, "rb");
	if (fp == NULL) {
		return ENOENT;
	}

	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	void* buffer = malloc(size);
	if (buffer == NULL) {
		fclose(fp);
		return B_NO_MEMORY;
	}

	if (fread(buffer, 1, size, fp) != size) {
		fclose(fp);
		free(buffer);
		return B_IO_ERROR;
	}

	fclose(fp);

	struct intel_i915_gem_object* obj;
	status_t status = intel_i915_gem_object_create(devInfo, size, 0, 0, 0, 0, &obj);
	if (status != B_OK) {
		free(buffer);
		return status;
	}

	void* obj_buffer;
	status = intel_i915_gem_object_map_cpu(obj, &obj_buffer);
	if (status != B_OK) {
		intel_i915_gem_object_put(obj);
		free(buffer);
		return status;
	}

	memcpy(obj_buffer, buffer, size);

	intel_i915_gem_object_unmap_cpu(obj);
	intel_i915_gem_object_put(obj);

	free(buffer);

	return B_OK;
}

void
intel_guc_uninit(intel_i915_device_info* devInfo)
{
	// TODO: Implement GuC uninitialization.
}

void
intel_guc_handle_response(intel_i915_device_info* devInfo)
{
	// TODO: Implement GuC response handling.
}
