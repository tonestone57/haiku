#include "dp.h"
#include "intel_i915_priv.h"
#include "registers.h"

status_t
intel_dp_init(intel_i915_device_info* devInfo, enum intel_port_id_priv port)
{
	uint8_t dpcd[16];
	if (intel_dp_read_dpcd(devInfo, &devInfo->ports[port], 0, dpcd, sizeof(dpcd)) != B_OK)
		return B_ERROR;

	devInfo->ports[port].dpcd_data.revision = dpcd[0];
	devInfo->ports[port].dpcd_data.max_link_rate = dpcd[1];
	devInfo->ports[port].dpcd_data.max_lane_count = dpcd[2] & 0x1f;
	devInfo->ports[port].dpcd_data.tps3_supported = (dpcd[2] & (1 << 6)) != 0;

	return B_OK;
}

static status_t
intel_dp_link_training_clock_recovery(intel_i915_device_info* devInfo, intel_output_port_state* port,
                                       const intel_clock_params_t* clock_params)
{
	uint8_t voltage_swing;
	uint8_t pre_emphasis;
	uint8_t lane_status[2];
	int tries;

	for (voltage_swing = 0; voltage_swing < 4; voltage_swing++) {
		for (pre_emphasis = 0; pre_emphasis < 4; pre_emphasis++) {
			intel_dp_set_lane_voltage_swing_pre_emphasis(devInfo, port, 0, voltage_swing, pre_emphasis);
			intel_dp_set_lane_voltage_swing_pre_emphasis(devInfo, port, 1, voltage_swing, pre_emphasis);
			intel_dp_set_lane_voltage_swing_pre_emphasis(devInfo, port, 2, voltage_swing, pre_emphasis);
			intel_dp_set_lane_voltage_swing_pre_emphasis(devInfo, port, 3, voltage_swing, pre_emphasis);

			intel_dp_set_link_train_pattern(devInfo, port, DPCD_TRAINING_PATTERN_1);

			for (tries = 0; tries < 5; tries++) {
				spin(100);
				intel_dp_get_lane_status_and_adjust_request(devInfo, port, lane_status, NULL);
				if (intel_dp_is_cr_done(lane_status, clock_params->dp_lane_count))
					return B_OK;
			}
		}
	}

	return B_ERROR;
}

static status_t
intel_dp_link_training_channel_equalization(intel_i915_device_info* devInfo, intel_output_port_state* port,
                                             const intel_clock_params_t* clock_params)
{
	uint8_t voltage_swing;
	uint8_t pre_emphasis;
	uint8_t lane_status[2];
	int tries;

	for (voltage_swing = 0; voltage_swing < 4; voltage_swing++) {
		for (pre_emphasis = 0; pre_emphasis < 4; pre_emphasis++) {
			intel_dp_set_lane_voltage_swing_pre_emphasis(devInfo, port, 0, voltage_swing, pre_emphasis);
			intel_dp_set_lane_voltage_swing_pre_emphasis(devInfo, port, 1, voltage_swing, pre_emphasis);
			intel_dp_set_lane_voltage_swing_pre_emphasis(devInfo, port, 2, voltage_swing, pre_emphasis);
			intel_dp_set_lane_voltage_swing_pre_emphasis(devInfo, port, 3, voltage_swing, pre_emphasis);

			intel_dp_set_link_train_pattern(devInfo, port, DPCD_TRAINING_PATTERN_2);

			for (tries = 0; tries < 5; tries++) {
				spin(400);
				intel_dp_get_lane_status_and_adjust_request(devInfo, port, lane_status, NULL);
				if (intel_dp_is_ce_done(lane_status, clock_params->dp_lane_count))
					return B_OK;
			}
		}
	}

	return B_ERROR;
}

status_t
intel_dp_link_training(intel_i915_device_info* devInfo, intel_output_port_state* port,
                        const intel_clock_params_t* clock_params)
{
	status_t status;

	status = intel_dp_link_training_clock_recovery(devInfo, port, clock_params);
	if (status != B_OK)
		return status;

	status = intel_dp_link_training_channel_equalization(devInfo, port, clock_params);
	if (status != B_OK)
		return status;

	return B_OK;
}

void
intel_dp_uninit(intel_i915_device_info* devInfo, enum intel_port_id_priv port)
{
	// TODO: Implement Display Port uninitialization.
}
