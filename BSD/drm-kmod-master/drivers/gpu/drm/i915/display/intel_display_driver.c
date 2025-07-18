// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022-2023 Intel Corporation
 *
 * High level display driver entry points. This is a layer between top level
 * driver code and low level display functionality; no low level display code or
 * details here.
 */

#include <linux/vga_switcheroo.h>
#include <acpi/video.h>
#include <drm/display/drm_dp_mst_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_privacy_screen_consumer.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "i915_drv.h"
#include "i9xx_wm.h"
#include "intel_acpi.h"
#include "intel_atomic.h"
#include "intel_audio.h"
#include "intel_bios.h"
#include "intel_bw.h"
#include "intel_cdclk.h"
#include "intel_color.h"
#include "intel_crtc.h"
#include "intel_display_debugfs.h"
#include "intel_display_driver.h"
#include "intel_display_irq.h"
#include "intel_display_power.h"
#include "intel_display_types.h"
#include "intel_display_wa.h"
#include "intel_dkl_phy.h"
#include "intel_dmc.h"
#include "intel_dp.h"
#include "intel_dpll.h"
#include "intel_dpll_mgr.h"
#include "intel_fb.h"
#include "intel_fbc.h"
#include "intel_fbdev.h"
#include "intel_fdi.h"
#include "intel_gmbus.h"
#include "intel_hdcp.h"
#include "intel_hotplug.h"
#include "intel_hti.h"
#include "intel_modeset_setup.h"
#include "intel_opregion.h"
#include "intel_overlay.h"
#include "intel_plane_initial.h"
#include "intel_pmdemand.h"
#include "intel_pps.h"
#include "intel_quirks.h"
#include "intel_vga.h"
#include "intel_wm.h"
#include "skl_watermark.h"

bool intel_display_driver_probe_defer(struct pci_dev *pdev)
{
	struct drm_privacy_screen *privacy_screen;

	/*
	 * apple-gmux is needed on dual GPU MacBook Pro
	 * to probe the panel if we're the inactive GPU.
	 */
	if (vga_switcheroo_client_probe_defer(pdev))
		return true;

	/* If the LCD panel has a privacy-screen, wait for it */
	privacy_screen = drm_privacy_screen_get(&pdev->dev, NULL);
	if (IS_ERR(privacy_screen) && PTR_ERR(privacy_screen) == -EPROBE_DEFER)
		return true;

	drm_privacy_screen_put(privacy_screen);

	return false;
}

void intel_display_driver_init_hw(struct drm_i915_private *i915)
{
	struct intel_cdclk_state *cdclk_state;

	if (!HAS_DISPLAY(i915))
		return;

	cdclk_state = to_intel_cdclk_state(i915->display.cdclk.obj.state);

	intel_update_cdclk(i915);
	intel_cdclk_dump_config(i915, &i915->display.cdclk.hw, "Current CDCLK");
	cdclk_state->logical = cdclk_state->actual = i915->display.cdclk.hw;

	intel_display_wa_apply(i915);
}

static const struct drm_mode_config_funcs intel_mode_funcs = {
	.fb_create = intel_user_framebuffer_create,
	.get_format_info = intel_fb_get_format_info,
	.output_poll_changed = intel_fbdev_output_poll_changed,
	.mode_valid = intel_mode_valid,
	.atomic_check = intel_atomic_check,
	.atomic_commit = intel_atomic_commit,
	.atomic_state_alloc = intel_atomic_state_alloc,
	.atomic_state_clear = intel_atomic_state_clear,
	.atomic_state_free = intel_atomic_state_free,
};

static const struct drm_mode_config_helper_funcs intel_mode_config_funcs = {
	.atomic_commit_setup = drm_dp_mst_atomic_setup_commit,
};

static void intel_mode_config_init(struct drm_i915_private *i915)
{
	struct drm_mode_config *mode_config = &i915->drm.mode_config;

	drm_mode_config_init(&i915->drm);
	INIT_LIST_HEAD(&i915->display.global.obj_list);

	mode_config->min_width = 0;
	mode_config->min_height = 0;

	mode_config->preferred_depth = 24;
	mode_config->prefer_shadow = 1;

	mode_config->funcs = &intel_mode_funcs;
	mode_config->helper_private = &intel_mode_config_funcs;

	mode_config->async_page_flip = HAS_ASYNC_FLIPS(i915);

	/*
	 * Maximum framebuffer dimensions, chosen to match
	 * the maximum render engine surface size on gen4+.
	 */
	if (DISPLAY_VER(i915) >= 7) {
		mode_config->max_width = 16384;
		mode_config->max_height = 16384;
	} else if (DISPLAY_VER(i915) >= 4) {
		mode_config->max_width = 8192;
		mode_config->max_height = 8192;
	} else if (DISPLAY_VER(i915) == 3) {
		mode_config->max_width = 4096;
		mode_config->max_height = 4096;
	} else {
		mode_config->max_width = 2048;
		mode_config->max_height = 2048;
	}

	if (IS_I845G(i915) || IS_I865G(i915)) {
		mode_config->cursor_width = IS_I845G(i915) ? 64 : 512;
		mode_config->cursor_height = 1023;
	} else if (IS_I830(i915) || IS_I85X(i915) ||
		   IS_I915G(i915) || IS_I915GM(i915)) {
		mode_config->cursor_width = 64;
		mode_config->cursor_height = 64;
	} else {
		mode_config->cursor_width = 256;
		mode_config->cursor_height = 256;
	}
}

static void intel_mode_config_cleanup(struct drm_i915_private *i915)
{
	intel_atomic_global_obj_cleanup(i915);
	drm_mode_config_cleanup(&i915->drm);
}

static void intel_plane_possible_crtcs_init(struct drm_i915_private *dev_priv)
{
	struct intel_plane *plane;

	for_each_intel_plane(&dev_priv->drm, plane) {
		struct intel_crtc *crtc = intel_crtc_for_pipe(dev_priv,
							      plane->pipe);

		plane->base.possible_crtcs = drm_crtc_mask(&crtc->base);
	}
}

void intel_display_driver_early_probe(struct drm_i915_private *i915)
{
	if (!HAS_DISPLAY(i915))
		return;

	spin_lock_init(&i915->display.fb_tracking.lock);
	mutex_init(&i915->display.backlight.lock);
	mutex_init(&i915->display.audio.mutex);
	mutex_init(&i915->display.wm.wm_mutex);
	mutex_init(&i915->display.pps.mutex);
	mutex_init(&i915->display.hdcp.hdcp_mutex);

	intel_display_irq_init(i915);
	intel_dkl_phy_init(i915);
	intel_color_init_hooks(i915);
	intel_init_cdclk_hooks(i915);
	intel_audio_hooks_init(i915);
	intel_dpll_init_clock_hook(i915);
	intel_init_display_hooks(i915);
	intel_fdi_init_hook(i915);
}

/* part #1: call before irq install */
int intel_display_driver_probe_noirq(struct drm_i915_private *i915)
{
	int ret;

	if (i915_inject_probe_failure(i915))
		return -ENODEV;

	if (HAS_DISPLAY(i915)) {
		ret = drm_vblank_init(&i915->drm,
				      INTEL_NUM_PIPES(i915));
		if (ret)
			return ret;
	}

	intel_bios_init(i915);

	ret = intel_vga_register(i915);
	if (ret)
		goto cleanup_bios;

	/* FIXME: completely on the wrong abstraction layer */
	ret = intel_power_domains_init(i915);
	if (ret < 0)
		goto cleanup_vga;

	intel_pmdemand_init_early(i915);

	intel_power_domains_init_hw(i915, false);

	if (!HAS_DISPLAY(i915))
		return 0;

	intel_dmc_init(i915);

	i915->display.wq.modeset = alloc_ordered_workqueue("i915_modeset", 0);
	i915->display.wq.flip = alloc_workqueue("i915_flip", WQ_HIGHPRI |
#ifdef __linux__
						WQ_UNBOUND, WQ_UNBOUND_MAX_ACTIVE);
#elif defined(__FreeBSD__)
						WQ_UNBOUND, 512);
#endif

	intel_mode_config_init(i915);

	ret = intel_cdclk_init(i915);
	if (ret)
		goto cleanup_vga_client_pw_domain_dmc;

	ret = intel_color_init(i915);
	if (ret)
		goto cleanup_vga_client_pw_domain_dmc;

	ret = intel_dbuf_init(i915);
	if (ret)
		goto cleanup_vga_client_pw_domain_dmc;

	ret = intel_bw_init(i915);
	if (ret)
		goto cleanup_vga_client_pw_domain_dmc;

	ret = intel_pmdemand_init(i915);
	if (ret)
		goto cleanup_vga_client_pw_domain_dmc;

	intel_init_quirks(i915);

	intel_fbc_init(i915);

	return 0;

cleanup_vga_client_pw_domain_dmc:
	intel_dmc_fini(i915);
	intel_power_domains_driver_remove(i915);
cleanup_vga:
	intel_vga_unregister(i915);
cleanup_bios:
	intel_bios_driver_remove(i915);

	return ret;
}

/* part #2: call after irq install, but before gem init */
int intel_display_driver_probe_nogem(struct drm_i915_private *i915)
{
	struct drm_device *dev = &i915->drm;
	enum pipe pipe;
	struct intel_crtc *crtc;
	int ret;

	if (!HAS_DISPLAY(i915))
		return 0;

	intel_wm_init(i915);

	intel_panel_sanitize_ssc(i915);

	intel_pps_setup(i915);

	intel_gmbus_setup(i915);

	drm_dbg_kms(&i915->drm, "%d display pipe%s available.\n",
		    INTEL_NUM_PIPES(i915),
		    INTEL_NUM_PIPES(i915) > 1 ? "s" : "");

	for_each_pipe(i915, pipe) {
		ret = intel_crtc_init(i915, pipe);
		if (ret) {
			intel_mode_config_cleanup(i915);
			return ret;
		}
	}

	intel_plane_possible_crtcs_init(i915);
	intel_shared_dpll_init(i915);
	intel_fdi_pll_freq_update(i915);

	intel_update_czclk(i915);
	intel_display_driver_init_hw(i915);
	intel_dpll_update_ref_clks(i915);

	intel_hdcp_component_init(i915);

	if (i915->display.cdclk.max_cdclk_freq == 0)
		intel_update_max_cdclk(i915);

	intel_hti_init(i915);

	/* Just disable it once at startup */
	intel_vga_disable(i915);
	intel_setup_outputs(i915);

	drm_modeset_lock_all(dev);
	intel_modeset_setup_hw_state(i915, dev->mode_config.acquire_ctx);
	intel_acpi_assign_connector_fwnodes(i915);
	drm_modeset_unlock_all(dev);

	for_each_intel_crtc(dev, crtc) {
		if (!to_intel_crtc_state(crtc->base.state)->uapi.active)
			continue;
		intel_crtc_initial_plane_config(crtc);
	}

	/*
	 * Make sure hardware watermarks really match the state we read out.
	 * Note that we need to do this after reconstructing the BIOS fb's
	 * since the watermark calculation done here will use pstate->fb.
	 */
	if (!HAS_GMCH(i915))
		ilk_wm_sanitize(i915);

	return 0;
}

/* part #3: call after gem init */
int intel_display_driver_probe(struct drm_i915_private *i915)
{
	int ret;

	if (!HAS_DISPLAY(i915))
		return 0;

	/*
	 * Force all active planes to recompute their states. So that on
	 * mode_setcrtc after probe, all the intel_plane_state variables
	 * are already calculated and there is no assert_plane warnings
	 * during bootup.
	 */
	ret = intel_initial_commit(&i915->drm);
	if (ret)
		drm_dbg_kms(&i915->drm, "Initial modeset failed, %d\n", ret);

	intel_overlay_setup(i915);

	ret = intel_fbdev_init(&i915->drm);
	if (ret)
		return ret;

	/* Only enable hotplug handling once the fbdev is fully set up. */
	intel_hpd_init(i915);
	intel_hpd_poll_disable(i915);

	skl_watermark_ipc_init(i915);

	return 0;
}

void intel_display_driver_register(struct drm_i915_private *i915)
{
	struct drm_printer p = drm_debug_printer("i915 display info:");

	if (!HAS_DISPLAY(i915))
		return;

	/* Must be done after probing outputs */
	intel_opregion_register(i915);
	intel_acpi_video_register(i915);

	intel_audio_init(i915);

	intel_display_debugfs_register(i915);

	/*
	 * Some ports require correctly set-up hpd registers for
	 * detection to work properly (leading to ghost connected
	 * connector status), e.g. VGA on gm45.  Hence we can only set
	 * up the initial fbdev config after hpd irqs are fully
	 * enabled. We do it last so that the async config cannot run
	 * before the connectors are registered.
	 */
	intel_fbdev_initial_config_async(i915);

	/*
	 * We need to coordinate the hotplugs with the asynchronous
	 * fbdev configuration, for which we use the
	 * fbdev->async_cookie.
	 */
	drm_kms_helper_poll_init(&i915->drm);

	intel_display_device_info_print(DISPLAY_INFO(i915),
					DISPLAY_RUNTIME_INFO(i915), &p);
}

/* part #1: call before irq uninstall */
void intel_display_driver_remove(struct drm_i915_private *i915)
{
	if (!HAS_DISPLAY(i915))
		return;

	flush_workqueue(i915->display.wq.flip);
	flush_workqueue(i915->display.wq.modeset);

	/*
	 * MST topology needs to be suspended so we don't have any calls to
	 * fbdev after it's finalized. MST will be destroyed later as part of
	 * drm_mode_config_cleanup()
	 */
	intel_dp_mst_suspend(i915);
}

/* part #2: call after irq uninstall */
void intel_display_driver_remove_noirq(struct drm_i915_private *i915)
{
	if (!HAS_DISPLAY(i915))
		return;

	/*
	 * Due to the hpd irq storm handling the hotplug work can re-arm the
	 * poll handlers. Hence disable polling after hpd handling is shut down.
	 */
	intel_hpd_poll_fini(i915);

	/* poll work can call into fbdev, hence clean that up afterwards */
	intel_fbdev_fini(i915);

#ifdef __linux__
	intel_unregister_dsm_handler();
#endif

	/* flush any delayed tasks or pending work */
	flush_workqueue(i915->unordered_wq);

	intel_hdcp_component_fini(i915);

	intel_mode_config_cleanup(i915);

	intel_overlay_cleanup(i915);

	intel_gmbus_teardown(i915);

	destroy_workqueue(i915->display.wq.flip);
	destroy_workqueue(i915->display.wq.modeset);

	intel_fbc_cleanup(i915);
}

/* part #3: call after gem init */
void intel_display_driver_remove_nogem(struct drm_i915_private *i915)
{
	intel_dmc_fini(i915);

	intel_power_domains_driver_remove(i915);

	intel_vga_unregister(i915);

	intel_bios_driver_remove(i915);
}

void intel_display_driver_unregister(struct drm_i915_private *i915)
{
	if (!HAS_DISPLAY(i915))
		return;

	intel_fbdev_unregister(i915);
	intel_audio_deinit(i915);

	/*
	 * After flushing the fbdev (incl. a late async config which
	 * will have delayed queuing of a hotplug event), then flush
	 * the hotplug events.
	 */
	drm_kms_helper_poll_fini(&i915->drm);
	drm_atomic_helper_shutdown(&i915->drm);

	acpi_video_unregister();
	intel_opregion_unregister(i915);
}

/*
 * turn all crtc's off, but do not adjust state
 * This has to be paired with a call to intel_modeset_setup_hw_state.
 */
int intel_display_driver_suspend(struct drm_i915_private *i915)
{
	struct drm_atomic_state *state;
	int ret;

	if (!HAS_DISPLAY(i915))
		return 0;

	state = drm_atomic_helper_suspend(&i915->drm);
	ret = PTR_ERR_OR_ZERO(state);
	if (ret)
		drm_err(&i915->drm, "Suspending crtc's failed with %i\n",
			ret);
	else
		i915->display.restore.modeset_state = state;
	return ret;
}

int
__intel_display_driver_resume(struct drm_i915_private *i915,
			      struct drm_atomic_state *state,
			      struct drm_modeset_acquire_ctx *ctx)
{
	struct drm_crtc_state *crtc_state;
	struct drm_crtc *crtc;
	int ret, i;

	intel_modeset_setup_hw_state(i915, ctx);
	intel_vga_redisable(i915);

	if (!state)
		return 0;

	/*
	 * We've duplicated the state, pointers to the old state are invalid.
	 *
	 * Don't attempt to use the old state until we commit the duplicated state.
	 */
	for_each_new_crtc_in_state(state, crtc, crtc_state, i) {
		/*
		 * Force recalculation even if we restore
		 * current state. With fast modeset this may not result
		 * in a modeset when the state is compatible.
		 */
		crtc_state->mode_changed = true;
	}

	/* ignore any reset values/BIOS leftovers in the WM registers */
	if (!HAS_GMCH(i915))
		to_intel_atomic_state(state)->skip_intermediate_wm = true;

	ret = drm_atomic_helper_commit_duplicated_state(state, ctx);

	drm_WARN_ON(&i915->drm, ret == -EDEADLK);

	return ret;
}

void intel_display_driver_resume(struct drm_i915_private *i915)
{
	struct drm_atomic_state *state = i915->display.restore.modeset_state;
	struct drm_modeset_acquire_ctx ctx;
	int ret;

	if (!HAS_DISPLAY(i915))
		return;

	i915->display.restore.modeset_state = NULL;
	if (state)
		state->acquire_ctx = &ctx;

	drm_modeset_acquire_init(&ctx, 0);

	while (1) {
		ret = drm_modeset_lock_all_ctx(&i915->drm, &ctx);
		if (ret != -EDEADLK)
			break;

		drm_modeset_backoff(&ctx);
	}

	if (!ret)
		ret = __intel_display_driver_resume(i915, state, &ctx);

	skl_watermark_ipc_update(i915);
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);

	if (ret)
		drm_err(&i915->drm,
			"Restoring old state failed with %i\n", ret);
	if (state)
		drm_atomic_state_put(state);
}
