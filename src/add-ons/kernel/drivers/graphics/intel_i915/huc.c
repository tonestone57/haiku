/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "huc.h"
#include "intel_i915_priv.h"
#include "huc_hevc.h"
#include "gem_object.h"

#include <FindDirectory.h>
#include <image.h>
#include <stdio.h>
#include "i915_platform_data.h"

status_t
intel_huc_init(intel_i915_device_info* devInfo)
{
	char path[256];
	snprintf(path, sizeof(path), "/lib/firmware/intel/%s_huc_ver%d_%d.bin",
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

	return intel_huc_hevc_init(devInfo);
}

void
intel_huc_uninit(intel_i915_device_info* devInfo)
{
	// TODO: Implement HuC uninitialization.
}

void
intel_huc_handle_response(intel_i915_device_info* devInfo)
{
	uint32_t response;
	while (intel_huc_get_response(devInfo, &response) == B_OK) {
		// TODO: Handle the response.
	}
}

status_t
intel_huc_get_response(intel_i915_device_info* devInfo, uint32_t* response)
{
	uint32_t* cmd_queue = (uint32_t*)devInfo->huc_log_cpu_addr;
	if (cmd_queue == NULL) {
		return B_NO_INIT;
	}
	uint32_t head = cmd_queue[GUC_CMD_QUEUE_HEAD_OFFSET / 4];
	uint32_t tail = cmd_queue[GUC_CMD_QUEUE_TAIL_OFFSET / 4];
	uint32_t size = cmd_queue[GUC_CMD_QUEUE_SIZE_OFFSET / 4];

	if (head == tail) {
		return B_NO_MEMORY;
	}

	*response = cmd_queue[head];
	head = (head + 1) % size;
	cmd_queue[GUC_CMD_QUEUE_HEAD_OFFSET / 4] = head;

	return B_OK;
}
