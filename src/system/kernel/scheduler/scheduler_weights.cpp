/*
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

#include "scheduler_weights.h"

#include <OS.h>
#include <debug.h>
#include <kernel.h>
#include <thread_types.h>
#include <algorithm>
#include <sched.h>

#include "scheduler_cpu.h"
#include "scheduler_defs.h"
#include "scheduler_thread.h"
#include "scheduler_team.h"
#include <sched.h>


static const int32 kNewMinActiveWeight = 15;
static const int32 kNewMaxWeightCap = 35000000;


static int32
calculate_weight(int32 priority)
{
	if (priority <= B_IDLE_PRIORITY)
		return 1;
	if (priority < B_LOWEST_ACTIVE_PRIORITY)
		return 2 + (priority - 1) * 2;

	int32 calcPrio = std::max(B_LOWEST_ACTIVE_PRIORITY,
		std::min(priority, sched_get_priority_max(SCHED_RR)));

	const double haiku_priority_step_factor = 1.091507805494422;
	double weight_fp;

	int exponent = calcPrio - B_NORMAL_PRIORITY;

	if (calcPrio >= B_REAL_TIME_DISPLAY_PRIORITY) {
		// Start with a high base for all RT threads
		weight_fp = 88761.0;
		exponent = calcPrio - B_REAL_TIME_DISPLAY_PRIORITY;
	} else {
		weight_fp = (double)SCHEDULER_WEIGHT_SCALE;
	}

	if (exponent > 0) {
		for (int i = 0; i < exponent; i++)
			weight_fp *= haiku_priority_step_factor;
	} else if (exponent < 0) {
		for (int i = 0; i > exponent; i--)
			weight_fp /= haiku_priority_step_factor;
	}

	int32 calculated_weight = static_cast<int32>(weight_fp + 0.5);

	if (calculated_weight < kNewMinActiveWeight && calcPrio >= B_LOWEST_ACTIVE_PRIORITY)
		calculated_weight = kNewMinActiveWeight;
	if (calculated_weight > kNewMaxWeightCap)
		calculated_weight = kNewMaxWeightCap;
	if (calculated_weight <= 1 && calcPrio >= B_LOWEST_ACTIVE_PRIORITY)
		calculated_weight = kNewMinActiveWeight;

	return calculated_weight;
}


void
scheduler_init_weights()
{
	dprintf("Scheduler: Initializing continuous weights table...\n");
	int max_prio = sched_get_priority_max(SCHED_RR);
	Scheduler::gHaikuContinuousWeights = new(std::nothrow) int32[max_prio + 1];
	for (int32 i = 0; i <= max_prio; i++) {
		Scheduler::gHaikuContinuousWeights[i] = calculate_weight(i);
	}
	dprintf("Scheduler: Continuous weights table initialized.\n");
}


using namespace BKernel;


int32
scheduler_priority_to_weight(Thread* thread, Scheduler::CPUEntry* cpu)
{
	if (thread == NULL)
		return 1;

	int32 priority = thread->priority;
	if (priority < 0)
		priority = 0;
	if (priority > sched_get_priority_max(SCHED_RR))
		priority = sched_get_priority_max(SCHED_RR);

	int32 weight = Scheduler::gHaikuContinuousWeights[priority];

	if (thread->team != NULL && thread->team->team_scheduler_data != NULL
		&& thread->priority < B_REAL_TIME_DISPLAY_PRIORITY) {
		Scheduler::TeamSchedulerData* teamData = thread->team->team_scheduler_data;
		InterruptsSpinLocker teamLocker(teamData->lock);
		if (teamData->quota_exhausted) {
			bool isBorrowing = false;
			if (Scheduler::gSchedulerElasticQuotaMode && cpu != NULL && cpu->GetCurrentActiveTeam() == teamData) {
				isBorrowing = true;
			}

			if (!isBorrowing) {
				if (Scheduler::gTeamQuotaExhaustionPolicy == TEAM_QUOTA_EXHAUST_STARVATION_LOW) {
					return Scheduler::gHaikuContinuousWeights[B_IDLE_PRIORITY];
				}
			}
		}
	}

	return weight;
}
