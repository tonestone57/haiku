/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2011, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_SCHEDULER_COMMON_H
#define KERNEL_SCHEDULER_COMMON_H


#include <algorithm>

#include <debug.h>
#include <kscheduler.h>
#include <load_tracking.h>
#include <smp.h>
#include <thread.h>
#include <user_debugger.h>
#include <util/MinMaxHeap.h>

#include "RunQueue.h"


//#define TRACE_SCHEDULER
#ifdef TRACE_SCHEDULER
#	define TRACE(...) dprintf_no_syslog(__VA_ARGS__)
#else
#	define TRACE(...) do { } while (false)
#endif


namespace Scheduler {


class CPUEntry;
class CoreEntry;
class ThreadData; // Forward declaration

// --- MLFQ and DTQ Definitions ---
#define NUM_MLFQ_LEVELS 16
#define DEFAULT_K_DIST_FACTOR 0.25f // Start with a conservative value

// Base time quanta for each MLFQ level (in microseconds)
// Level 0 (highest priority) to NUM_MLFQ_LEVELS - 1 (lowest priority)
static const bigtime_t kBaseQuanta[NUM_MLFQ_LEVELS] = {
	2000,   // Level 0
	3000,   // Level 1
	4000,   // Level 2
	5000,   // Level 3
	6000,   // Level 4
	7000,   // Level 5
	8000,   // Level 6
	10000,  // Level 7
	12000,  // Level 8
	15000,  // Level 9
	18000,  // Level 10
	22000,  // Level 11
	26000,  // Level 12
	30000,  // Level 13
	40000,  // Level 14
	50000   // Level 15 (lowest priority)
};

// Aging thresholds (in system_time units - microseconds) for each MLFQ level
// Time a thread can wait in a queue (levels 1 to NUM_MLFQ_LEVELS-1) before promotion
static const bigtime_t kAgingThresholds[NUM_MLFQ_LEVELS] = {
	0,      // Level 0 doesn't age up (highest)
	50000,  // Level 1
	100000, // Level 2
	150000, // Level 3
	200000, // Level 4
	250000, // Level 5
	300000, // Level 6
	400000, // Level 7
	500000, // Level 8
	600000, // Level 9
	700000, // Level 10
	800000, // Level 11
	900000, // Level 12
	1000000, // Level 13
	1500000, // Level 14
	2000000  // Level 15
};

// Global minimum and maximum effective quantum
static const bigtime_t kMinEffectiveQuantum = 500;     // 0.5 ms
static const bigtime_t kMaxEffectiveQuantum = 100000;  // 100 ms

// EWMA alpha for CPUEntry instantaneous load calculation
static const float kInstantLoadEWMAAlpha = 0.4f;

// --- End MLFQ and DTQ Definitions ---


const int kLowLoad = kMaxLoad * 20 / 100;
const int kTargetLoad = kMaxLoad * 55 / 100;
const int kHighLoad = kMaxLoad * 70 / 100;
const int kMediumLoad = (kHighLoad + kTargetLoad) / 2;
const int kVeryHighLoad = (kMaxLoad + kHighLoad) / 2;

const int kLoadDifference = kMaxLoad * 20 / 100;

extern bool gSingleCore;
extern bool gTrackCoreLoad;
extern bool gTrackCPULoad;
extern float gKernelKDistFactor; // To be initialized, potentially by scheduler modes


void init_debug_commands();


}	// namespace Scheduler


#endif	// KERNEL_SCHEDULER_COMMON_H

