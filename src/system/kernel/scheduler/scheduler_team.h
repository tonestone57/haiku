/*
 * Copyright 2025, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */

#ifndef _SCHEDULER_TEAM_H
#define _SCHEDULER_TEAM_H

#include <OS.h>
#include <SupportDefs.h>
#include <lock.h>
#include <timer.h>
#include <util/DoublyLinkedList.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
struct timer;

// Team scheduler constants
#define SCHEDULER_TEAM_QUOTA_RESET_INTERVAL	1000000  // 1 second in microseconds
#define SCHEDULER_TEAM_MAX_ITERATIONS		100      // Max teams per timer event
#define SCHEDULER_TEAM_INVALID_ID		-1       // Invalid team ID marker

// Team scheduler data structure
struct TeamSchedulerData {
	// Doubly linked list linkage
	DoublyLinkedListLink<TeamSchedulerData> link;
	
	// Team identification
	team_id teamID;
	
	// Synchronization
	spinlock lock;
	
	// Quota management
	bigtime_t quota_period_usage;    // CPU time used in current period
	bigtime_t quota_limit;           // Maximum CPU time per period
	bool quota_exhausted;            // Whether quota has been exceeded
	bigtime_t last_quota_reset;      // When quota was last reset
	
	// Scheduling statistics
	bigtime_t total_cpu_time;        // Total CPU time consumed
	uint32 context_switches;         // Number of context switches
	uint32 preemptions;              // Number of preemptions
	
	// Priority and scheduling policy
	int32 base_priority;             // Base priority for team threads
	int32 current_priority;          // Current effective priority
	uint32 scheduling_policy;        // Scheduling policy flags
	
	// Performance metrics
	bigtime_t avg_runtime;           // Average thread runtime
	bigtime_t max_runtime;           // Maximum thread runtime
	uint32 thread_count;             // Active thread count
	
	// Constructor for C++ compatibility
	#ifdef __cplusplus
	TeamSchedulerData() 
		: teamID(SCHEDULER_TEAM_INVALID_ID)
		, lock(B_SPINLOCK_INITIALIZER)
		, quota_period_usage(0)
		, quota_limit(0)
		, quota_exhausted(false)
		, last_quota_reset(0)
		, total_cpu_time(0)
		, context_switches(0)
		, preemptions(0)
		, base_priority(B_NORMAL_PRIORITY)
		, current_priority(B_NORMAL_PRIORITY)
		, scheduling_policy(0)
		, avg_runtime(0)
		, max_runtime(0)
		, thread_count(0)
	{
	}
	#endif
};

// Statistics structure for monitoring
struct team_scheduler_stats {
	uint32 total_teams;              // Total number of teams
	uint32 quota_exhausted_teams;    // Teams that exceeded quota
	bigtime_t total_quota_usage;     // Sum of all team quota usage
	bigtime_t last_quota_reset;      // Last global quota reset time
	uint32 quota_resets_per_second;  // Rate of quota resets
	bigtime_t avg_team_cpu_time;     // Average CPU time per team
	uint32 total_context_switches;   // Total context switches
	uint32 total_preemptions;        // Total preemptions
};

// Callback function type for safe team data access
typedef status_t (*team_scheduler_callback_t)(TeamSchedulerData* tsd, void* data);

// Function prototypes

/**
 * Timer event handler for resetting team CPU quotas.
 * Called periodically to reset quota usage for all teams.
 * 
 * @param timer The timer that triggered this event
 * @return B_HANDLED_INTERRUPT on success
 */
int32 scheduler_reset_team_quotas_event(timer* timer);

/**
 * Add a new team to the scheduler tracking system.
 * Initializes team scheduler data and adds it to the global list.
 * 
 * @param tsd Pointer to team scheduler data structure
 * @return B_OK on success, B_BAD_VALUE, B_NAME_IN_USE on error
 */
status_t add_team_scheduler_data(TeamSchedulerData* tsd);

/**
 * Remove a team from the scheduler tracking system.
 * Removes team from global list and marks it as invalid.
 * 
 * @param tsd Pointer to team scheduler data structure
 * @return B_OK on success, B_BAD_VALUE, B_ENTRY_NOT_FOUND on error
 */
status_t remove_team_scheduler_data(TeamSchedulerData* tsd);

/**
 * Find team scheduler data by team ID.
 * Searches the global team list for the specified team.
 * 
 * @param teamID Team identifier to search for
 * @return Pointer to team data on success, NULL if not found
 */
TeamSchedulerData* find_team_scheduler_data(team_id teamID);

/**
 * Safely access team scheduler data with callback.
 * Acquires appropriate locks and calls the provided callback function.
 * 
 * @param teamID Team identifier
 * @param callback Function to call with locked team data
 * @param data User data to pass to callback
 * @return Status from callback, or error if team not found
 */
status_t with_team_scheduler_data(team_id teamID, 
	team_scheduler_callback_t callback, void* data);

/**
 * Get comprehensive team scheduler statistics.
 * Collects statistics from all teams for monitoring and debugging.
 * 
 * @param stats Pointer to statistics structure to fill
 */
void get_team_scheduler_stats(team_scheduler_stats* stats);

// Inline utility functions for common operations

#ifdef __cplusplus

namespace Scheduler {

/**
 * Check if a team has exceeded its CPU quota.
 * 
 * @param tsd Team scheduler data
 * @return true if quota exceeded, false otherwise
 */
inline bool IsQuotaExhausted(const TeamSchedulerData* tsd)
{
	return tsd != NULL && tsd->quota_exhausted;
}

/**
 * Get the percentage of quota used by a team.
 * 
 * @param tsd Team scheduler data
 * @return Percentage (0.0 to 100.0+) of quota used
 */
inline double GetQuotaUsagePercent(const TeamSchedulerData* tsd)
{
	if (tsd == NULL || tsd->quota_limit == 0)
		return 0.0;
	return (double)tsd->quota_period_usage / (double)tsd->quota_limit * 100.0;
}

/**
 * Calculate team's average thread runtime.
 * 
 * @param tsd Team scheduler data
 * @return Average runtime per thread in microseconds
 */
inline bigtime_t GetAverageThreadRuntime(const TeamSchedulerData* tsd)
{
	if (tsd == NULL || tsd->thread_count == 0)
		return 0;
	return tsd->total_cpu_time / tsd->thread_count;
}

/**
 * Check if team data is valid and accessible.
 * 
 * @param tsd Team scheduler data
 * @return true if valid, false otherwise
 */
inline bool IsTeamDataValid(const TeamSchedulerData* tsd)
{
	return tsd != NULL && tsd->teamID > 0;
}

} // namespace Scheduler

#endif /* __cplusplus */

// Global variables (declared in scheduler_team.cpp)
extern spinlock gTeamSchedulerListLock;
extern DoublyLinkedList<TeamSchedulerData> gTeamSchedulerDataList;

// Macros for team scheduler operations

/**
 * Safely iterate over all teams with proper locking.
 * Usage: SCHEDULER_FOR_EACH_TEAM(tsd) { ... }
 */
#define SCHEDULER_FOR_EACH_TEAM(tsd) \
	for (cpu_status __state = disable_interrupts(), \
		 __dummy = (acquire_spinlock(&gTeamSchedulerListLock), 0); \
		 __dummy == 0; \
		 release_spinlock(&gTeamSchedulerListLock), \
		 restore_interrupts(__state), __dummy = 1) \
	for (TeamSchedulerData* tsd = gTeamSchedulerDataList.Head(); \
		 tsd != NULL; \
		 tsd = gTeamSchedulerDataList.GetNext(tsd))

/**
 * Lock a team's scheduler data safely.
 * Usage: SCHEDULER_LOCK_TEAM(tsd) { ... } SCHEDULER_UNLOCK_TEAM(tsd)
 */
#define SCHEDULER_LOCK_TEAM(tsd) \
	do { \
		cpu_status __team_state = disable_interrupts(); \
		acquire_spinlock(&(tsd)->lock);

#define SCHEDULER_UNLOCK_TEAM(tsd) \
		release_spinlock(&(tsd)->lock); \
		restore_interrupts(__team_state); \
	} while (0)

#ifdef __cplusplus
}
#endif

#endif /* _SCHEDULER_TEAM_H */