/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Jules Maintainer
 */

#include "hpd.h"
#include "accelerant.h"

#include <syslog.h>
#include <unistd.h>

#include <notify.h>
#include <AppDefs.h>
#include <Messenger.h>


extern "C" status_t app_server_notify_display_changed(bool active);


// HPD Monitoring Thread function
int32
hpd_monitoring_thread_entry(void* data)
{
	accelerant_info* localGInfo = (accelerant_info*)data;
	if (!localGInfo || localGInfo->device_fd < 0) {
		syslog(LOG_ERR, "intel_i915_hpd: Thread started with invalid localGInfo or device_fd.\n");
		return B_BAD_VALUE;
	}

	TRACE("HPD: Event thread (ID: %" B_PRId32 ") started for accelerant instance: %s, fd: %d\n",
		find_thread(NULL), localGInfo->device_path_suffix, localGInfo->device_fd);

	while (localGInfo->hpd_thread_active) {
		struct i915_display_change_event_ioctl_data event_data;
		memset(&event_data, 0, sizeof(event_data));
		event_data.version = 0;
		event_data.timeout_us = 2000000; // 2 seconds timeout

		status_t ioctl_status = ioctl(localGInfo->device_fd, INTEL_I915_WAIT_FOR_DISPLAY_CHANGE, &event_data, sizeof(event_data));

		if (!localGInfo->hpd_thread_active) {
			TRACE("HPD: Thread signaled to exit while or after IOCTL.\n");
			break;
		}

		if (ioctl_status == B_OK) {
			if (event_data.changed_hpd_mask != 0) {
				TRACE("HPD: Display change detected by kernel (mask 0x%lx). Refreshing config & notifying app_server.\n", event_data.changed_hpd_mask);

				// 1. Refresh Connector Information for changed ports (and potentially all)
				// The kernel's i915_handle_hotplug_event already updates port_state including EDID.
				// Here, we primarily ensure our accelerant's view of shared_info (if it caches details)
				// or pipe_framebuffers (if a pipe was implicitly disabled/enabled by kernel due to HPD) is synced.
				// A full GET_DISPLAY_CONFIG is generally the most robust way to sync up.

				// Optional: Iterate changed_hpd_mask and call GET_CONNECTOR_INFO for each.
				// This is mostly for debug or if accelerant caches extensive connector details not in shared_info.
				// For now, we rely on GET_DISPLAY_CONFIG to update pipe-related info.
				// Example of iterating mask:
				// for (uint32 hpd_line_idx = 0; hpd_line_idx < I915_HPD_MAX_LINES; ++hpd_line_idx) {
				//    if (event_data.changed_hpd_mask & (1 << hpd_line_idx)) {
				//        intel_i915_get_connector_info_args cargs;
				//        cargs.connector_id = hpd_line_idx; // This mapping needs to be correct
				//        ioctl(localGInfo->device_fd, INTEL_I915_GET_CONNECTOR_INFO, &cargs, sizeof(cargs));
				//        TRACE("HPD: Refreshed connector for HPD line %lu, Name: %s, Connected: %d\n",
				//             hpd_line_idx, cargs.name, cargs.is_connected);
				//    }
				// }


				// 2. Refresh overall display configuration (active pipes, modes, FBs)
				// This is crucial to update gInfo->pipe_framebuffers and shared_info view.
				struct i915_get_display_config_args get_config_args;
				struct i915_display_pipe_config kernel_pipe_configs[I915_MAX_PIPES_USER];
				memset(&get_config_args, 0, sizeof(get_config_args));
				memset(kernel_pipe_configs, 0, sizeof(kernel_pipe_configs));
				get_config_args.pipe_configs_ptr = (uint64)(uintptr_t)kernel_pipe_configs;
				get_config_args.max_pipe_configs_to_get = I915_MAX_PIPES_USER;

				if (ioctl(localGInfo->device_fd, INTEL_I915_GET_DISPLAY_CONFIG, &get_config_args, sizeof(get_config_args)) == B_OK) {
					TRACE("HPD: Refreshed display config: %lu active pipes. Primary user pipe: %u\n",
						get_config_args.num_pipe_configs, get_config_args.primary_pipe_id);

					// Update gInfo->pipe_framebuffers based on kernel_pipe_configs
					// First, mark all as inactive, then update active ones.
					for(uint32 pipe_idx_user = 0; pipe_idx_user < I915_MAX_PIPES_USER; ++pipe_idx_user) {
						gInfo->pipe_framebuffers[pipe_idx_user].is_active = false;
						// Don't clear gem_handle here, it might be reused if mode is same.
						// accel_ensure_framebuffer_for_pipe handles GEM handle lifecycle.
					}

					for (uint32 k_idx = 0; k_idx < get_config_args.num_pipe_configs; k_idx++) {
						struct i915_display_pipe_config* kcfg = &kernel_pipe_configs[k_idx];
						if (kcfg->pipe_id < I915_MAX_PIPES_USER) {
							enum accel_pipe_id pipe_user_enum = (enum accel_pipe_id)kcfg->pipe_id;
							struct pipe_framebuffer_info* pfb = &gInfo->pipe_framebuffers[pipe_user_enum];

							pfb->is_active = kcfg->active;
							if (kcfg->active) {
								// If the GEM handle changed, or dimensions/depth changed,
								// the old mapping (if any) is invalid.
								if (pfb->gem_handle != kcfg->fb_gem_handle ||
									pfb->width != kcfg->mode.virtual_width ||
									pfb->height != kcfg->mode.virtual_height ||
									pfb->depth != _get_bpp_from_colorspace_accel(kcfg->mode.space))
								{
									if (pfb->mapping_area >= B_OK) {
										delete_area(pfb->mapping_area);
										pfb->mapping_area = -1;
										pfb->base_address = NULL;
									}
								}
								pfb->gem_handle = kcfg->fb_gem_handle;
								pfb->width = kcfg->mode.virtual_width;
								pfb->height = kcfg->mode.virtual_height;
								pfb->depth = _get_bpp_from_colorspace_accel(kcfg->mode.space);

								// Stride, GTT offset, tiling would require another IOCTL like GET_GEM_INFO per handle.
								// These are critical for 2D ops.
								// For now, these might be missing or stale in pipe_framebuffers after HPD.
								// A TODO for init_common and HPD to call GET_GEM_INFO.
								// pfb->stride = ???; pfb->gtt_offset_pages = ???; pfb->tiling_mode = ???;

								TRACE("HPD: PipeUser %u now active: GEM %u, Mode %ux%u\n",
									pipe_user_enum, kcfg->fb_gem_handle, kcfg->mode.virtual_width, kcfg->mode.virtual_height);
							}
						}
					}
					// Update the cloned shared_info (gInfo->shared_info) to reflect the new state from kernel.
					// The kernel directly updates its shared_info, so the clone becomes stale.
					// Re-cloning is heavy. Best is if kernel's SET_DISPLAY_CONFIG and HPD handler
					// ensure the shared_info is accurate.
					// For now, assume app_server will use GET_DISPLAY_CONFIGURATION hook which uses IOCTL.
				} else {
					TRACE("HPD: Failed to refresh display config via GET_DISPLAY_CONFIG IOCTL.\n");
				}

				// 3. Notify app_server
				app_server_notify_display_changed(true);
			} else {
				// IOCTL B_OK but mask 0: No specific HPD line change reported by kernel, or timeout with no event.
			}
		} else if (ioctl_status == B_TIMED_OUT) {
			// Normal timeout, no display event from kernel's perspective.
		} else if (ioctl_status == B_INTERRUPTED || ioctl_status == B_BAD_SEM_ID || ioctl_status == B_FILE_ERROR) {
			// B_FILE_ERROR can happen if the device fd is closed while thread is waiting.
			TRACE("HPD: Wait IOCTL interrupted or fd error (%s); thread exiting.\n", strerror(ioctl_status));
			break;
		} else {
			syslog(LOG_ERR, "intel_i915_hpd: Error from INTEL_I915_WAIT_FOR_DISPLAY_CHANGE IOCTL: %s (0x%lx)\n", strerror(ioctl_status), ioctl_status);
			snooze(1000000); // 1 second before retrying on other errors
		}
	}

	TRACE("HPD: Event thread (ID: %" B_PRId32 ") for %s exiting.\n", find_thread(NULL), localGInfo->device_path_suffix);
	return B_OK;
}
