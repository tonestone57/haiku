#include "scheduler_team.h"
#include <kernel.h>
#include <lock.h>
#include <util/DoublyLinkedList.h>
#include "scheduler_locking.h"
#include "scheduler_cpu.h"

namespace Scheduler {

// Global team scheduler data management
static spinlock gTeamSchedulerListLock = B_SPINLOCK_INITIALIZER;
static DoublyLinkedList<TeamSchedulerData> gTeamSchedulerDataList;

// Performance optimization: cache frequently accessed data
static volatile int32 gTeamCount = 0;
static bigtime_t gLastQuotaReset = 0;

// Constants for better maintainability
static const bigtime_t kQuotaResetInterval = 1000000; // 1 second in microseconds
static const int32 kMaxTeamsPerIteration = 100; // Limit work per timer event


int32
scheduler_reset_team_quotas_event(timer* timer)
{
	// Validate timer parameter
	if (timer == NULL) {
		return B_HANDLED_INTERRUPT;
	}
	
	// Performance optimization: early exit if no teams
	if (atomic_get(&gTeamCount) == 0) {
		return B_HANDLED_INTERRUPT;
	}
	
	bigtime_t currentTime = system_time();
	
	// Prevent excessive resets - rate limiting
	if (currentTime - gLastQuotaReset < kQuotaResetInterval) {
		return B_HANDLED_INTERRUPT;
	}
	
	cpu_status state = disable_interrupts();
	acquire_spinlock(&gTeamSchedulerListLock);
	
	// Batch processing to limit lock hold time
	int32 processedCount = 0;
	TeamSchedulerData* tsd = gTeamSchedulerDataList.Head();
	TeamSchedulerData* nextTsd;
	
	while (tsd != NULL && processedCount < kMaxTeamsPerIteration) {
		// Get next pointer before potentially modifying current node
		nextTsd = gTeamSchedulerDataList.GetNext(tsd);
		
		// Verify team data is still valid
		if (tsd->teamID <= 0) {
			// Invalid team data - remove it
			gTeamSchedulerDataList.Remove(tsd);
			atomic_add(&gTeamCount, -1);
		} else {
			// Nested spinlock - use try_acquire to avoid deadlock
			if (try_acquire_spinlock(&tsd->lock)) {
				// Reset quota data atomically
				tsd->quota_period_usage = 0;
				tsd->quota_exhausted = false;
				tsd->last_quota_reset = currentTime;
				
				release_spinlock(&tsd->lock);
			}
			// If we can't acquire the team lock, skip this team
			// It will be handled in the next iteration
		}
		
		tsd = nextTsd;
		processedCount++;
	}
	
	// Update global reset timestamp
	gLastQuotaReset = currentTime;
	
	release_spinlock(&gTeamSchedulerListLock);
	restore_interrupts(state);
	
	return B_HANDLED_INTERRUPT;
}


// Team scheduler data management functions
status_t
add_team_scheduler_data(TeamSchedulerData* tsd)
{
	if (tsd == NULL || tsd->teamID <= 0) {
		return B_BAD_VALUE;
	}
	
	// Initialize team-specific data
	B_INITIALIZE_SPINLOCK(&tsd->lock);
	tsd->quota_period_usage = 0;
	tsd->quota_exhausted = false;
	tsd->last_quota_reset = system_time();
	
	cpu_status state = disable_interrupts();
	acquire_spinlock(&gTeamSchedulerListLock);
	
	// Check for duplicate team IDs
	TeamSchedulerData* existing = gTeamSchedulerDataList.Head();
	while (existing != NULL) {
		if (existing->teamID == tsd->teamID) {
			release_spinlock(&gTeamSchedulerListLock);
			restore_interrupts(state);
			return B_NAME_IN_USE;
		}
		existing = gTeamSchedulerDataList.GetNext(existing);
	}
	
	gTeamSchedulerDataList.Add(tsd);
	atomic_add(&gTeamCount, 1);
	
	release_spinlock(&gTeamSchedulerListLock);
	restore_interrupts(state);
	
	return B_OK;
}


status_t
remove_team_scheduler_data(TeamSchedulerData* tsd)
{
	if (tsd == NULL) {
		return B_BAD_VALUE;
	}
	
	cpu_status state = disable_interrupts();
	acquire_spinlock(&gTeamSchedulerListLock);
	
	// Verify the team data is in our list
	TeamSchedulerData* current = gTeamSchedulerDataList.Head();
	bool found = false;
	while (current != NULL) {
		if (current == tsd) {
			found = true;
			break;
		}
		current = gTeamSchedulerDataList.GetNext(current);
	}
	
	if (found) {
		// Acquire team lock to ensure no concurrent access
		acquire_spinlock(&tsd->lock);
		
		gTeamSchedulerDataList.Remove(tsd);
		atomic_add(&gTeamCount, -1);
		
		// Mark as invalid
		tsd->teamID = -1;
		
		release_spinlock(&tsd->lock);
	}
	
	release_spinlock(&gTeamSchedulerListLock);
	restore_interrupts(state);
	
	return found ? B_OK : B_ENTRY_NOT_FOUND;
}


TeamSchedulerData*
find_team_scheduler_data(team_id teamID)
{
	if (teamID <= 0) {
		return NULL;
	}
	
	// Performance optimization: early exit if no teams
	if (atomic_get(&gTeamCount) == 0) {
		return NULL;
	}
	
	TeamSchedulerData* result = NULL;
	
	cpu_status state = disable_interrupts();
	acquire_spinlock(&gTeamSchedulerListLock);
	
	TeamSchedulerData* current = gTeamSchedulerDataList.Head();
	while (current != NULL) {
		if (current->teamID == teamID) {
			result = current;
			break;
		}
		current = gTeamSchedulerDataList.GetNext(current);
	}
	
	release_spinlock(&gTeamSchedulerListLock);
	restore_interrupts(state);
	
	return result;
}


// Utility function for safe team data access
status_t
with_team_scheduler_data(team_id teamID, 
	status_t (*callback)(TeamSchedulerData*, void*), void* data)
{
	if (callback == NULL || teamID <= 0) {
		return B_BAD_VALUE;
	}
	
	TeamSchedulerData* tsd = find_team_scheduler_data(teamID);
	if (tsd == NULL) {
		return B_ENTRY_NOT_FOUND;
	}
	
	cpu_status state = disable_interrupts();
	acquire_spinlock(&tsd->lock);
	
	// Verify team is still valid
	status_t result = B_ERROR;
	if (tsd->teamID == teamID) {
		result = callback(tsd, data);
	} else {
		result = B_ENTRY_NOT_FOUND;
	}
	
	release_spinlock(&tsd->lock);
	restore_interrupts(state);
	
	return result;
}


// Statistics and debugging support
void
get_team_scheduler_stats(team_scheduler_stats* stats)
{
	if (stats == NULL) {
		return;
	}
	
	memset(stats, 0, sizeof(team_scheduler_stats));
	
	cpu_status state = disable_interrupts();
	acquire_spinlock(&gTeamSchedulerListLock);
	
	stats->total_teams = atomic_get(&gTeamCount);
	stats->last_quota_reset = gLastQuotaReset;
	
	// Count teams by state
	TeamSchedulerData* tsd = gTeamSchedulerDataList.Head();
	while (tsd != NULL) {
		if (try_acquire_spinlock(&tsd->lock)) {
			if (tsd->quota_exhausted) {
				stats->quota_exhausted_teams++;
			}
			stats->total_quota_usage += tsd->quota_period_usage;
			release_spinlock(&tsd->lock);
		}
		tsd = gTeamSchedulerDataList.GetNext(tsd);
	}
	
	release_spinlock(&gTeamSchedulerListLock);
	restore_interrupts(state);
}

} // namespace Scheduler