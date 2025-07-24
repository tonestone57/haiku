/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

#include "scheduler_thread.h"

#include <util/Random.h>
#include <util/atomic.h>

#include <algorithm>

namespace Scheduler {
int32* gHaikuContinuousWeights = NULL;
}

using namespace Scheduler;



CoreEntry*
Scheduler::ThreadData::_ChooseCore() const
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(!gSingleCore);

	if (system_time() - fLastMigrationTime < 10000) {
		return fCore;
	}

	if (gCurrentMode == NULL) {
		// Add bounds checking for core selection
		if (gCoreCount <= 0) {
			TRACE_SCHED_WARNING("_ChooseCore: Invalid gCoreCount %" B_PRId32 "\n", gCoreCount);
			return &gCoreEntries[0]; // Fallback to first core
		}
		return &gCoreEntries[get_random<int32>() % gCoreCount];
	}
	return gCurrentMode->choose_core(this);
}

CPUEntry*
Scheduler::ThreadData::_ChooseCPU(CoreEntry* core, bool& rescheduleNeeded, const CPUSet& mask) const
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(core != NULL);

	const bool useMask = !mask.IsEmpty();
	ASSERT(!useMask || mask.Matches(core->CPUMask()));

	rescheduleNeeded = false; // Default
	CPUEntry* chosenCPU = NULL;

	// Check previous CPU for cache affinity first, only if it's on the chosen core.
	cpu_ent* previousCpuEnt = fThread->previous_cpu;
	if (previousCpuEnt != NULL && previousCpuEnt->cpu_num >= 0 &&
		previousCpuEnt->cpu_num < smp_get_num_cpus() &&
		!gCPU[previousCpuEnt->cpu_num].disabled) {

		CPUEntry* previousCPUEntry = CPUEntry::GetCPU(previousCpuEnt->cpu_num);
		if (previousCPUEntry != NULL && previousCPUEntry->Core() == core &&
			(!useMask || mask.GetBit(previousCpuEnt->cpu_num))) {

			// Previous CPU is on the target core and matches affinity.
			// Check its SMT-aware effective load.
			float smtPenalty = 0.0f;
			if (core->CPUCount() > 1) { // SMT is possible
				int32 prevCpuID = previousCPUEntry->ID();
				int32 prevSMTID = gCPU[prevCpuID].topology_id[CPU_TOPOLOGY_SMT];
				if (prevSMTID != -1) {
					for (int32 k = 0; k < smp_get_num_cpus(); k++) {
						if (k == prevCpuID || gCPU[k].disabled ||
							CPUEntry::GetCPU(k)->Core() != core)
							continue;
						if (gCPU[k].topology_id[CPU_TOPOLOGY_SMT] == prevSMTID) {
							smtPenalty += CPUEntry::GetCPU(k)->GetInstantaneousLoad() * gSchedulerSMTConflictFactor;
						}
					}
				}
			}
			float effectiveLoad = previousCPUEntry->GetInstantaneousLoad() + smtPenalty;

			// If previous CPU is not too loaded (SMT-aware), prefer it.
			if (effectiveLoad < 0.75f) {
				chosenCPU = previousCPUEntry;
				TRACE_SCHED_SMT("_ChooseCPU: T %" B_PRId32 " to previous CPU %" B_PRId32 " on core %" B_PRId32 " (effLoad %.2f)\n",
					fThread->id, chosenCPU->ID(), core->ID(), effectiveLoad);
			}
		}
	}

	if (fThread->pinned_to_cpu > 0) {
		chosenCPU = CPUEntry::GetCPU(fThread->pinned_to_cpu - 1);
	} else if (chosenCPU == NULL) {
		// Previous CPU not suitable or not on this core.
		// Iterate all enabled CPUs on the chosen core, select the one with the best SMT-aware effective load.
		CPUEntry* bestCandidateOnCore = NULL;
		float lowestEffectiveLoad = 2.0f; // Start high (max load is 1.0 + penalty)

		CPUSet coreCPUs = core->CPUMask();
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (!coreCPUs.GetBit(i) || gCPU[i].disabled)
				continue;

			CPUEntry* candidateCPU = CPUEntry::GetCPU(i);
			if (candidateCPU == NULL || candidateCPU->Core() != core)
				continue;

			if (useMask && !mask.GetBit(i)) // Check affinity mask
				continue;

			float currentInstLoad = candidateCPU->GetInstantaneousLoad();
			float smtPenalty = 0.0f;

			if (core->CPUCount() > 1) { // SMT is possible
				int32 candidateCpuID = candidateCPU->ID();
				int32 candidateSMTID = gCPU[candidateCpuID].topology_id[CPU_TOPOLOGY_SMT];

				if (candidateSMTID != -1) {
					for (int32 k = 0; k < smp_get_num_cpus(); k++) {
						if (k == candidateCpuID || gCPU[k].disabled ||
							CPUEntry::GetCPU(k)->Core() != core)
							continue;
						// Check if CPU 'k' is an SMT sibling of 'candidateCPU'
						if (gCPU[k].topology_id[CPU_TOPOLOGY_SMT] == candidateSMTID) {
							smtPenalty += CPUEntry::GetCPU(k)->GetInstantaneousLoad() * gSchedulerSMTConflictFactor;
						}
					}
				}
			}
			float effectiveLoad = currentInstLoad + smtPenalty;

			if (effectiveLoad < lowestEffectiveLoad || bestCandidateOnCore == NULL) {
				lowestEffectiveLoad = effectiveLoad;
				bestCandidateOnCore = candidateCPU;
			} else if (effectiveLoad == lowestEffectiveLoad) {
				// Tie-breaking: prefer CPU with fewer total tasks
				if (candidateCPU->GetTotalThreadCount() < bestCandidateOnCore->GetTotalThreadCount()) {
					bestCandidateOnCore = candidateCPU;
				}
			}
		}
		chosenCPU = bestCandidateOnCore;
		if (chosenCPU != NULL) {
			TRACE_SCHED_SMT("_ChooseCPU: T %" B_PRId32 " to best SMT-aware CPU %" B_PRId32 " on core %" B_PRId32 " (effLoad %.2f)\n",
				fThread->id, chosenCPU->ID(), core->ID(), lowestEffectiveLoad);
		}
	}

	if (chosenCPU == NULL) {
		// Last resort: just pick the first available CPU on this core
		CPUSet coreCPUs = core->CPUMask();
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (coreCPUs.GetBit(i) && !gCPU[i].disabled &&
				(!useMask || mask.GetBit(i))) {
				chosenCPU = CPUEntry::GetCPU(i);
				TRACE_SCHED_WARNING("_ChooseCPU: T %" B_PRId32 " fallback to CPU %" B_PRId32 " on core %" B_PRId32 "\n",
					fThread->id, i, core->ID());
				break;
			}
		}
	}

	ASSERT(chosenCPU != NULL && "Could not find a schedulable CPU on the chosen core");

	// Determine if reschedule is needed
	if (chosenCPU != NULL) {
		int32 cpuId = chosenCPU->ID();
		if (cpuId >= 0 && cpuId < smp_get_num_cpus()) {
			if (gCPU[cpuId].running_thread == NULL ||
				thread_is_idle_thread(gCPU[cpuId].running_thread)) {
				rescheduleNeeded = true;
			} else if (fThread->cpu != &gCPU[cpuId]) {
				rescheduleNeeded = true;
			}
		}
	}

	return chosenCPU;
}

// #pragma mark - Core Logic

/*! Chooses the best logical CPU (CPUEntry) on a given physical core for this thread.
	This function is SMT-aware. It prioritizes the thread's previous CPU on this
	core if its cache is likely warm and its SMT-aware effective load is low.
	Otherwise, it iterates all enabled CPUs on the core, calculating an
	SMT-aware "effective load" for each. It selects the CPU with the lowest
	effective load. Affinity masks are respected.
	\param core The physical core on which to choose a logical CPU.
	\param rescheduleNeeded Output parameter; set to true if a reschedule is likely
	       needed on the chosen CPU.
	+eturn The chosen CPUEntry.
*/
inline CPUEntry*
Scheduler::ThreadData::_ChooseCPU(CoreEntry* core, bool& rescheduleNeeded) const
{
	SCHEDULER_ENTER_FUNCTION();
	ASSERT(core != NULL);

	CPUSet mask = GetCPUMask();
	const bool useMask = !mask.IsEmpty();
	ASSERT(!useMask || mask.Matches(core->CPUMask()));

	rescheduleNeeded = false; // Default
	CPUEntry* chosenCPU = NULL;

	// Check previous CPU for cache affinity first, only if it's on the chosen core.
	cpu_ent* previousCpuEnt = fThread->previous_cpu;
	if (previousCpuEnt != NULL && previousCpuEnt->cpu_num >= 0 &&
		previousCpuEnt->cpu_num < smp_get_num_cpus() &&
		!gCPU[previousCpuEnt->cpu_num].disabled) {

		CPUEntry* previousCPUEntry = CPUEntry::GetCPU(previousCpuEnt->cpu_num);
		if (previousCPUEntry != NULL && previousCPUEntry->Core() == core &&
			(!useMask || mask.GetBit(previousCpuEnt->cpu_num))) {

			// Previous CPU is on the target core and matches affinity.
			// Check its SMT-aware effective load.
			float smtPenalty = 0.0f;
			if (core->CPUCount() > 1) { // SMT is possible
				int32 prevCpuID = previousCPUEntry->ID();
				int32 prevSMTID = gCPU[prevCpuID].topology_id[CPU_TOPOLOGY_SMT];
				if (prevSMTID != -1) {
					for (int32 k = 0; k < smp_get_num_cpus(); k++) {
						if (k == prevCpuID || gCPU[k].disabled ||
							CPUEntry::GetCPU(k)->Core() != core)
							continue;
						if (gCPU[k].topology_id[CPU_TOPOLOGY_SMT] == prevSMTID) {
							smtPenalty += CPUEntry::GetCPU(k)->GetInstantaneousLoad() * gSchedulerSMTConflictFactor;
						}
					}
				}
			}
			float effectiveLoad = previousCPUEntry->GetInstantaneousLoad() + smtPenalty;

			// If previous CPU is not too loaded (SMT-aware), prefer it.
			if (effectiveLoad < 0.75f) {
				chosenCPU = previousCPUEntry;
				TRACE_SCHED_SMT("_ChooseCPU: T %" B_PRId32 " to previous CPU %" B_PRId32 " on core %" B_PRId32 " (effLoad %.2f)\n",
					fThread->id, chosenCPU->ID(), core->ID(), effectiveLoad);
			}
		}
	}

	if (fThread->pinned_to_cpu > 0) {
		chosenCPU = CPUEntry::GetCPU(fThread->pinned_to_cpu - 1);
	} else if (chosenCPU == NULL) {
		// Previous CPU not suitable or not on this core.
		// Iterate all enabled CPUs on the chosen core, select the one with the best SMT-aware effective load.
		CPUEntry* bestCandidateOnCore = NULL;
		float lowestEffectiveLoad = 2.0f; // Start high (max load is 1.0 + penalty)

		CPUSet coreCPUs = core->CPUMask();
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (!coreCPUs.GetBit(i) || gCPU[i].disabled)
				continue;

			CPUEntry* candidateCPU = CPUEntry::GetCPU(i);
			if (candidateCPU == NULL || candidateCPU->Core() != core)
				continue;

			if (useMask && !mask.GetBit(i)) // Check affinity mask
				continue;

			float currentInstLoad = candidateCPU->GetInstantaneousLoad();
			float smtPenalty = 0.0f;

			if (core->CPUCount() > 1) { // SMT is possible
				int32 candidateCpuID = candidateCPU->ID();
				int32 candidateSMTID = gCPU[candidateCpuID].topology_id[CPU_TOPOLOGY_SMT];

				if (candidateSMTID != -1) {
					for (int32 k = 0; k < smp_get_num_cpus(); k++) {
						if (k == candidateCpuID || gCPU[k].disabled ||
							CPUEntry::GetCPU(k)->Core() != core)
							continue;
						// Check if CPU 'k' is an SMT sibling of 'candidateCPU'
						if (gCPU[k].topology_id[CPU_TOPOLOGY_SMT] == candidateSMTID) {
							smtPenalty += CPUEntry::GetCPU(k)->GetInstantaneousLoad() * gSchedulerSMTConflictFactor;
						}
					}
				}
			}
			float effectiveLoad = currentInstLoad + smtPenalty;

			if (effectiveLoad < lowestEffectiveLoad || bestCandidateOnCore == NULL) {
				lowestEffectiveLoad = effectiveLoad;
				bestCandidateOnCore = candidateCPU;
			} else if (effectiveLoad == lowestEffectiveLoad) {
				// Tie-breaking: prefer CPU with fewer total tasks
				if (candidateCPU->GetTotalThreadCount() < bestCandidateOnCore->GetTotalThreadCount()) {
					bestCandidateOnCore = candidateCPU;
				}
			}
		}
		chosenCPU = bestCandidateOnCore;
		if (chosenCPU != NULL) {
			TRACE_SCHED_SMT("_ChooseCPU: T %" B_PRId32 " to best SMT-aware CPU %" B_PRId32 " on core %" B_PRId32 " (effLoad %.2f)\n",
				fThread->id, chosenCPU->ID(), core->ID(), lowestEffectiveLoad);
		}
	}

	if (chosenCPU == NULL) {
		// Last resort: just pick the first available CPU on this core
		CPUSet coreCPUs = core->CPUMask();
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (coreCPUs.GetBit(i) && !gCPU[i].disabled &&
				(!useMask || mask.GetBit(i))) {
				chosenCPU = CPUEntry::GetCPU(i);
				TRACE_SCHED_WARNING("_ChooseCPU: T %" B_PRId32 " fallback to CPU %" B_PRId32 " on core %" B_PRId32 "\n",
					fThread->id, i, core->ID());
				break;
			}
		}
	}

	ASSERT(chosenCPU != NULL && "Could not find a schedulable CPU on the chosen core");

	// Determine if reschedule is needed
	if (chosenCPU != NULL) {
		int32 cpuId = chosenCPU->ID();
		if (cpuId >= 0 && cpuId < smp_get_num_cpus()) {
			if (gCPU[cpuId].running_thread == NULL ||
				thread_is_idle_thread(gCPU[cpuId].running_thread)) {
				rescheduleNeeded = true;
			} else if (fThread->cpu != &gCPU[cpuId]) {
				rescheduleNeeded = true;
			}
		}
	}

	return chosenCPU;
}
