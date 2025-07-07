/*
 * Copyright 2006-2018, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, axeld@pinc-software.de
 */


#include "driver.h"
#include "device.h"
#include "intel_extreme.h"
#include "utility.h"

#include <OS.h>
#include <KernelExport.h>
#include <Drivers.h>
#include <PCI.h>
#include <SupportDefs.h>
#include <graphic_driver.h>
#include <image.h>

#include <stdlib.h>
#include <string.h>


#define DEBUG_COMMANDS

#define TRACE_DEVICE
#ifdef TRACE_DEVICE
#	define TRACE(x...) dprintf("intel_extreme: " x)
#else
#	define TRACE(x) ;
#endif

#define ERROR(x...) dprintf("intel_extreme: " x)
#define CALLED(x...) TRACE("CALLED %s\n", __PRETTY_FUNCTION__)


/* device hooks prototypes */

static status_t device_open(const char* name, uint32 flags, void** _cookie);
static status_t device_close(void* data);
static status_t device_free(void* data);
static status_t device_ioctl(void* data, uint32 opcode, void* buffer,
	size_t length);
static status_t device_read(void* data, off_t offset, void* buffer,
	size_t* length);
static status_t device_write(void* data, off_t offset, const void* buffer,
	size_t* length);


device_hooks gDeviceHooks = {
	device_open,
	device_close,
	device_free,
	device_ioctl,
	device_read,
	device_write,
	NULL,
	NULL,
	NULL,
	NULL
};


#ifdef DEBUG_COMMANDS
static int
getset_register(int argc, char** argv)
{
	if (argc < 2 || argc > 3) {
		kprintf("usage: %s <register> [set-to-value]\n", argv[0]);
		return 0;
	}

	uint32 reg = parse_expression(argv[1]);
	uint32 value = 0;
	bool set = argc == 3;
	if (set)
		value = parse_expression(argv[2]);

	kprintf("intel_extreme register %#" B_PRIx32 "\n", reg);

	intel_info &info = *gDeviceInfo[0];
	uint32 oldValue = read32(info, reg);

	kprintf("  %svalue: %#" B_PRIx32 " (%" B_PRIu32 ")\n", set ? "old " : "",
		oldValue, oldValue);

	if (set) {
		write32(info, reg, value);

		value = read32(info, reg);
		kprintf("  new value: %#" B_PRIx32 " (%" B_PRIu32 ")\n", value, value);
	}

	return 0;
}


static int
dump_pipe_info(int argc, char** argv)
{
	int pipeOffset = 0;

	if (argc > 2) {
		kprintf("usage: %s [pipe index]\n", argv[0]);
		return 0;
	}

	if (argc > 1) {
		uint32 pipe = parse_expression(argv[1]);
		if (pipe != 0)
			pipeOffset = INTEL_DISPLAY_OFFSET; // Use pipe B if requested
	}

	intel_info &info = *gDeviceInfo[0];
	uint32 value;

	kprintf("intel_extreme pipe configuration:\n");

	value = read32(info, INTEL_DISPLAY_A_HTOTAL + pipeOffset);
	kprintf("  HTOTAL start %" B_PRIu32 " end %" B_PRIu32 "\n",
		(value & 0xFFFF) + 1, (value >> 16) + 1);
	value = read32(info, INTEL_DISPLAY_A_HBLANK + pipeOffset);
	kprintf("  HBLANK start %" B_PRIu32 " end %" B_PRIu32 "\n",
		(value & 0xFFFF) + 1, (value >> 16) + 1);
	value = read32(info, INTEL_DISPLAY_A_HSYNC + pipeOffset);
	kprintf("  HSYNC start %" B_PRIu32 " end %" B_PRIu32 "\n",
		(value & 0xFFFF) + 1, (value >> 16) + 1);
	value = read32(info, INTEL_DISPLAY_A_VTOTAL + pipeOffset);
	kprintf("  VTOTAL start %" B_PRIu32 " end %" B_PRIu32 "\n",
		(value & 0xFFFF) + 1, (value >> 16) + 1);
	value = read32(info, INTEL_DISPLAY_A_VBLANK + pipeOffset);
	kprintf("  VBLANK start %" B_PRIu32 " end %" B_PRIu32 "\n",
		(value & 0xFFFF) + 1, (value >> 16) + 1);
	value = read32(info, INTEL_DISPLAY_A_VSYNC + pipeOffset);
	kprintf("  VSYNC start %" B_PRIu32 " end %" B_PRIu32 "\n",
		(value & 0xFFFF) + 1, (value >> 16) + 1);
	value = read32(info, INTEL_DISPLAY_A_PIPE_SIZE + pipeOffset);
	kprintf("  SIZE %" B_PRIu32 "x%" B_PRIu32 "\n",
		(value & 0xFFFF) + 1, (value >> 16) + 1);

	if (info.pch_info != INTEL_PCH_NONE) {
		kprintf("intel_extreme transcoder configuration:\n");

		value = read32(info, INTEL_TRANSCODER_A_HTOTAL + pipeOffset);
		kprintf("  HTOTAL start %" B_PRIu32 " end %" B_PRIu32 "\n",
			(value & 0xFFFF) + 1, (value >> 16) + 1);
		value = read32(info, INTEL_TRANSCODER_A_HBLANK + pipeOffset);
		kprintf("  HBLANK start %" B_PRIu32 " end %" B_PRIu32 "\n",
			(value & 0xFFFF) + 1, (value >> 16) + 1);
		value = read32(info, INTEL_TRANSCODER_A_HSYNC + pipeOffset);
		kprintf("  HSYNC start %" B_PRIu32 " end %" B_PRIu32 "\n",
			(value & 0xFFFF) + 1, (value >> 16) + 1);
		value = read32(info, INTEL_TRANSCODER_A_VTOTAL + pipeOffset);
		kprintf("  VTOTAL start %" B_PRIu32 " end %" B_PRIu32 "\n",
			(value & 0xFFFF) + 1, (value >> 16) + 1);
		value = read32(info, INTEL_TRANSCODER_A_VBLANK + pipeOffset);
		kprintf("  VBLANK start %" B_PRIu32 " end %" B_PRIu32 "\n",
			(value & 0xFFFF) + 1, (value >> 16) + 1);
		value = read32(info, INTEL_TRANSCODER_A_VSYNC + pipeOffset);
		kprintf("  VSYNC start %" B_PRIu32 " end %" B_PRIu32 "\n",
			(value & 0xFFFF) + 1, (value >> 16) + 1);
		value = read32(info, INTEL_TRANSCODER_A_IMAGE_SIZE + pipeOffset);
		kprintf("  SIZE %" B_PRIu32 "x%" B_PRIu32 "\n",
			(value & 0xFFFF) + 1, (value >> 16) + 1);
	}

	kprintf("intel_extreme display plane configuration:\n");

	value = read32(info, INTEL_DISPLAY_A_CONTROL + pipeOffset);
	kprintf("  CONTROL: %" B_PRIx32 "\n", value);
	value = read32(info, INTEL_DISPLAY_A_BASE + pipeOffset);
	kprintf("  BASE: %" B_PRIx32 "\n", value);
	value = read32(info, INTEL_DISPLAY_A_BYTES_PER_ROW + pipeOffset);
	kprintf("  BYTES_PER_ROW: %" B_PRIx32 "\n", value);
	value = read32(info, INTEL_DISPLAY_A_SURFACE + pipeOffset);
	kprintf("  SURFACE: %" B_PRIx32 "\n", value);

	return 0;
}

#endif	// DEBUG_COMMANDS


//	#pragma mark - Device Hooks


static status_t
device_open(const char* name, uint32 /*flags*/, void** _cookie)
{
	CALLED();
	int32 id;

	// find accessed device
	{
		char* thisName;

		// search for device name
		for (id = 0; (thisName = gDeviceNames[id]) != NULL; id++) {
			if (!strcmp(name, thisName))
				break;
		}
		if (!thisName)
			return B_BAD_VALUE;
	}

	intel_info* info = gDeviceInfo[id];

	mutex_lock(&gLock);

	if (info->open_count == 0) {
		// This device hasn't been initialized yet, so we
		// allocate needed resources and initialize the structure
		info->init_status = intel_extreme_init(*info);
		if (info->init_status == B_OK) {
#ifdef DEBUG_COMMANDS
			add_debugger_command("ie_reg", getset_register,
				"dumps or sets the specified intel_extreme register");
			add_debugger_command("ie_pipe", dump_pipe_info,
				"show pipe configuration information");
#endif
		}
	}

	if (info->init_status == B_OK) {
		info->open_count++;
		*_cookie = info;
	} else
		ERROR("%s: initialization failed!\n", __func__);

	mutex_unlock(&gLock);

	return info->init_status;
}


static status_t
device_close(void* /*data*/)
{
	CALLED();
	return B_OK;
}


static status_t
device_free(void* data)
{
	struct intel_info* info = (intel_info*)data;

	mutex_lock(&gLock);

	if (info->open_count-- == 1) {
		// release info structure
		info->init_status = B_NO_INIT;
		intel_extreme_uninit(*info);

#ifdef DEBUG_COMMANDS
		remove_debugger_command("ie_reg", getset_register);
		remove_debugger_command("ie_pipe", dump_pipe_info);
#endif
	}

	mutex_unlock(&gLock);
	return B_OK;
}


static status_t
device_ioctl(void* data, uint32 op, void* buffer, size_t bufferLength)
{
	struct intel_info* info = (intel_info*)data;

	switch (op) {
		case B_GET_ACCELERANT_SIGNATURE:
			TRACE("accelerant: %s\n", INTEL_ACCELERANT_NAME);
			if (user_strlcpy((char*)buffer, INTEL_ACCELERANT_NAME,
					bufferLength) < B_OK)
				return B_BAD_ADDRESS;
			return B_OK;

		// needed to share data between kernel and accelerant
		case INTEL_GET_PRIVATE_DATA:
		{
			intel_get_private_data data;
			if (user_memcpy(&data, buffer, sizeof(intel_get_private_data)) < B_OK)
				return B_BAD_ADDRESS;

			if (data.magic == INTEL_PRIVATE_DATA_MAGIC) {
				data.shared_info_area = info->shared_area;
				return user_memcpy(buffer, &data,
					sizeof(intel_get_private_data));
			}
			break;
		}

		// needed for cloning
		case INTEL_GET_DEVICE_NAME:
			if (user_strlcpy((char* )buffer, gDeviceNames[info->id],
					bufferLength) < B_OK)
				return B_BAD_ADDRESS;
			return B_OK;

		// graphics mem manager
		case INTEL_ALLOCATE_GRAPHICS_MEMORY:
		{
			intel_allocate_graphics_memory allocMemory;
			if (user_memcpy(&allocMemory, buffer,
					sizeof(intel_allocate_graphics_memory)) < B_OK)
				return B_BAD_ADDRESS;

			if (allocMemory.magic != INTEL_PRIVATE_DATA_MAGIC)
				return B_BAD_VALUE;

			status_t status = intel_allocate_memory(*info, allocMemory.size,
				allocMemory.alignment, allocMemory.flags,
				&allocMemory.buffer_base);
			if (status == B_OK) {
				// copy result
				if (user_memcpy(buffer, &allocMemory,
						sizeof(intel_allocate_graphics_memory)) < B_OK)
					return B_BAD_ADDRESS;
			}
			return status;
		}

		case INTEL_FREE_GRAPHICS_MEMORY:
		{
			intel_free_graphics_memory freeMemory;
			if (user_memcpy(&freeMemory, buffer,
					sizeof(intel_free_graphics_memory)) < B_OK)
				return B_BAD_ADDRESS;

			if (freeMemory.magic == INTEL_PRIVATE_DATA_MAGIC)
				return intel_free_memory(*info, freeMemory.buffer_base);
			break;
		}

		case INTEL_GET_BRIGHTNESS_LEGACY:
		case INTEL_SET_BRIGHTNESS_LEGACY:
		{
			intel_brightness_legacy brightnessLegacy;
			if (user_memcpy(&brightnessLegacy, buffer,
					sizeof(brightnessLegacy)) < B_OK)
				return B_BAD_ADDRESS;

			if (brightnessLegacy.magic != INTEL_PRIVATE_DATA_MAGIC)
				break;
			if (op == INTEL_GET_BRIGHTNESS_LEGACY) {
				brightnessLegacy.lpc = get_pci_config(info->pci, LEGACY_BACKLIGHT_BRIGHTNESS, 1);
				// copy result
				if (user_memcpy(buffer, &brightnessLegacy, sizeof(brightnessLegacy)) < B_OK)
					return B_BAD_ADDRESS;
			} else {
				set_pci_config(info->pci, LEGACY_BACKLIGHT_BRIGHTNESS, 1, brightnessLegacy.lpc);
			}
			return B_OK;
		}

		case INTEL_GET_DISPLAY_COUNT:
		{
			if (buffer == NULL || bufferLength < sizeof(uint32))
				return B_BAD_VALUE;

			uint32 count = 0;
			// More accurate count would be from shared_info->active_display_count
			// after accelerant initializes it properly.
			// For now, we can estimate based on detected ports that have pipes.
			// This is a placeholder until accelerant fully populates shared_info.
			for (int i = 0; i < MAX_PIPES; i++) {
				if (info->shared_info->pipe_display_configs[i].is_active)
					count++;
			}
			if (count == 0 && info->shared_info->active_display_count > 0) {
				// Fallback if is_active isn't set yet but accelerant might know.
				count = info->shared_info->active_display_count;
			} else if (count == 0) {
				// A very rough estimate if nothing else is set:
				// This part is problematic as kernel doesn't know about accelerant's port probing directly.
				// User-space should rely on accelerant's detection primarily.
				// This ioctl is better implemented by accelerant if it can handle ioctls,
				// or shared_info must be very reliably populated.
				// For now, let's assume shared_info->active_display_count is the authority.
				count = info->shared_info->active_display_count;
			}

			if (user_memcpy(buffer, &count, sizeof(uint32)) < B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case INTEL_GET_DISPLAY_INFO:
		{
			if (buffer == NULL || bufferLength < sizeof(intel_display_info_params))
				return B_BAD_VALUE;

			intel_display_info_params params;
			if (user_memcpy(&params, buffer, sizeof(intel_display_info_params)) < B_OK)
				return B_BAD_ADDRESS;

			if (params.magic != INTEL_PRIVATE_DATA_MAGIC)
				return B_BAD_VALUE;

			uint32 pipeIndex = params.id.pipe_index;
			if (pipeIndex >= MAX_PIPES)
				return B_BAD_INDEX; // Or a more specific error

			// Populate params from shared_info
			params.is_connected = info->shared_info->pipe_display_configs[pipeIndex].is_active; // Approximation
			params.is_currently_active = info->shared_info->pipe_display_configs[pipeIndex].is_active;
			params.has_edid = info->shared_info->has_edid[pipeIndex];
			if (params.has_edid) {
				memcpy(&params.edid_data, &info->shared_info->edid_infos[pipeIndex], sizeof(edid1_info));
			} else {
				memset(&params.edid_data, 0, sizeof(edid1_info));
			}
			if (params.is_currently_active) {
				params.current_mode = info->shared_info->pipe_display_configs[pipeIndex].current_mode;
			} else {
				memset(&params.current_mode, 0, sizeof(display_mode));
			}
			// TODO: Populate connector_name if that info is available in shared_info

			if (user_memcpy(buffer, &params, sizeof(intel_display_info_params)) < B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case INTEL_SET_DISPLAY_CONFIG:
		{
			if (buffer == NULL || bufferLength < sizeof(intel_multi_display_config))
				return B_BAD_VALUE;

			intel_multi_display_config multi_config;
			if (user_memcpy(&multi_config, buffer, sizeof(intel_multi_display_config)) < B_OK)
				return B_BAD_ADDRESS;

			if (multi_config.magic != INTEL_PRIVATE_DATA_MAGIC)
				return B_BAD_VALUE;

			// Critical section: update shared_info with the new target configuration.
			// The accelerant will pick this up and apply it.
			// This requires careful synchronization if the accelerant reads this asynchronously.
			// For now, assume direct update and accelerant reads on its next mode set call.
			acquire_sem(info->shared_info->accelerant_lock_sem); // Assuming a sem for this

			info->shared_info->active_display_count = 0; // Reset before counting active ones from config
			for (uint32 i = 0; i < MAX_PIPES; i++) {
				// First, mark all as inactive
				info->shared_info->pipe_display_configs[i].is_active = false;
			}

			for (uint32 i = 0; i < multi_config.display_count; i++) {
				uint32 pipeIdx = multi_config.configs[i].id.pipe_index;
				if (pipeIdx < MAX_PIPES) {
					info->shared_info->pipe_display_configs[pipeIdx].current_mode = multi_config.configs[i].mode; // This is the TARGET mode
					info->shared_info->pipe_display_configs[pipeIdx].is_active = multi_config.configs[i].is_active;
					// TODO: Store pos_x, pos_y if layout manager is in kernel/shared_info
					if (multi_config.configs[i].is_active) {
						info->shared_info->active_display_count++;
						// Logic to set primary_pipe_index if this display is primary
						// For now, user-space should ensure one primary or driver picks first active.
						// if (multi_config.configs[i].is_primary)
						// info->shared_info->primary_pipe_index = pipeIdx;
					}
				}
			}
			// If no primary was explicitly set, default logic in accelerant might pick one.
			// Or, ensure primary_pipe_index is valid among active displays.
			bool primaryStillActive = false;
			if (info->shared_info->primary_pipe_index < MAX_PIPES &&
				info->shared_info->pipe_display_configs[info->shared_info->primary_pipe_index].is_active) {
				primaryStillActive = true;
			}
			if (!primaryStillActive && info->shared_info->active_display_count > 0) {
				for(uint32 i=0; i < MAX_PIPES; ++i) {
					if (info->shared_info->pipe_display_configs[i].is_active) {
						info->shared_info->primary_pipe_index = i;
						break;
					}
				}
			}


			release_sem(info->shared_info->accelerant_lock_sem);

			// TODO: How to trigger the accelerant to re-evaluate and apply the mode?
			// This might involve a condition variable, or the accelerant periodically checks,
			// or the next call to an accelerant function like intel_set_display_mode (which would
			// need to be adapted to take no arguments and read from shared_info).
			// For now, this ioctl just updates shared_info. The actual mode switch
			// is assumed to be triggered by a subsequent call to the (modified)
			// intel_set_display_mode accelerant hook.
			// A call to a function like `intel_extreme_set_display_configuration_hook(info)` could be made here
			// if such a hook is exposed by the accelerant via the driver.

			return B_OK;
		}

		case INTEL_GET_DISPLAY_CONFIG:
		{
			if (buffer == NULL || bufferLength < sizeof(intel_multi_display_config))
				return B_BAD_VALUE;

			intel_multi_display_config multi_config;
			multi_config.magic = INTEL_PRIVATE_DATA_MAGIC;
			multi_config.display_count = 0;

			acquire_sem(info->shared_info->accelerant_lock_sem); // Protect shared_info access

			for (uint32 i = 0; i < MAX_PIPES; i++) {
				if (info->shared_info->pipe_display_configs[i].is_active) {
					uint32 cfgIdx = multi_config.display_count++;
					multi_config.configs[cfgIdx].id.pipe_index = i;
					multi_config.configs[cfgIdx].mode = info->shared_info->pipe_display_configs[i].current_mode;
					multi_config.configs[cfgIdx].is_active = true;
					// TODO: Populate pos_x, pos_y if stored
					// multi_config.configs[cfgIdx].is_primary = (i == info->shared_info->primary_pipe_index);
				}
			}
			release_sem(info->shared_info->accelerant_lock_sem);

			if (user_memcpy(buffer, &multi_config, sizeof(intel_multi_display_config)) < B_OK)
				return B_BAD_ADDRESS;
			return B_OK;
		}

		case INTEL_PROPOSE_DISPLAY_CONFIG:
			// This would be similar to SET, but calls a "propose" function in the accelerant
			// which doesn't apply the mode, only validates it.
			// For now, returning B_UNSUPPORTED as the full validation logic isn't in place.
			return B_UNSUPPORTED;

		default:
			ERROR("ioctl() unknown message %" B_PRIu32 " (length = %"
				B_PRIuSIZE ")\n", op, bufferLength);
			break;
	}

	return B_DEV_INVALID_IOCTL;
}


static status_t
device_read(void* /*data*/, off_t /*pos*/, void* /*buffer*/, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
device_write(void* /*data*/, off_t /*pos*/, const void* /*buffer*/,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}

