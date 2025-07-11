#ifndef KERNEL_SCHEDULER_TEAM_H
#define KERNEL_SCHEDULER_TEAM_H

#include <OS.h>
#include <kernel.h> // For team_id
#include <lock.h>   // For spinlock
#include <util/DoublyLinkedList.h>


namespace Scheduler {

/*! \struct TeamSchedulerData
    \brief Holds scheduler-specific data for a team, primarily for CPU quota management.

    Each team in the system that is subject to CPU quota controls will have an
    associated TeamSchedulerData structure. This structure tracks the team's
    CPU usage within a defined quota period, its allocated quota, and its
    fairness metrics for team-level scheduling decisions.
*/
struct TeamSchedulerData : DoublyLinkedListLinkImpl<TeamSchedulerData> {
    team_id     teamID;                     //!< The ID of the team this data belongs to.
    uint32      cpu_quota_percent;          //!< Configured CPU quota percentage (0-100 typically).
                                            //!< 0 means no explicit quota (behaves as unlimited unless constrained by system load).
    bigtime_t   quota_period_usage;         //!< CPU time consumed by this team in the current quota period (in microseconds).
    bigtime_t   current_quota_allowance;    //!< Actual CPU time (µs) this team is allowed in the current period, derived from \c cpu_quota_percent and \c gQuotaPeriod.
    bool        quota_exhausted;            //!< True if \c quota_period_usage >= \c current_quota_allowance and allowance > 0.

    bigtime_t   team_virtual_runtime;       //!< Team-level virtual runtime, used for fair-share selection among teams (Tier 1 scheduler).
                                            //!< Lower vruntime means higher priority for team selection. Advances based on CPU usage and quota percentage.

    spinlock    lock;                       //!< Protects mutable fields of this structure (e.g., usage, allowance, exhausted status).

    // This struct inherits from DoublyLinkedListLinkImpl to be part of a global list
    // (\c gTeamSchedulerDataList) for easy iteration and management.

    TeamSchedulerData(team_id id)
        :
        teamID(id),
        cpu_quota_percent(0),   // Default: 0% means no explicit quota initially.
        quota_period_usage(0),
        current_quota_allowance(0),
        quota_exhausted(false), // A team with 0% quota is not considered "exhausted" by default.
        team_virtual_runtime(0) // Initialized to 0; updated to gGlobalMinTeamVRuntime when added to global list.
    {
        B_INITIALIZE_SPINLOCK(&lock);
    }

    // No complex destructor needed for now as it doesn't own heap resources directly.
    // If resources were allocated (e.g., via new), they would be freed here.
    // ~TeamSchedulerData();
};

} // namespace Scheduler

#endif // KERNEL_SCHEDULER_TEAM_H
