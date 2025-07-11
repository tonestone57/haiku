#ifndef KERNEL_SCHEDULER_TEAM_H
#define KERNEL_SCHEDULER_TEAM_H

#include <OS.h>
#include <kernel.h> // For team_id
#include <lock.h>   // For spinlock
#include <util/DoublyLinkedList.h>


namespace Scheduler {

struct TeamSchedulerData : DoublyLinkedListLinkImpl<TeamSchedulerData> {
    team_id     teamID;
    uint32      cpu_quota_percent;          // Quota percentage (0-100, or other scale)
    bigtime_t   quota_period_usage;         // CPU time consumed in current period (µs)
    bigtime_t   current_quota_allowance;    // Actual µs this team can run in current period
    bool        quota_exhausted;
    spinlock    lock;                       // Protects this structure's mutable fields

    // Used for a global list of all TeamSchedulerData instances
    // DoublyLinkedListLinkImpl takes care of next/prev via inheritance.

    TeamSchedulerData(team_id id)
        :
        teamID(id),
        cpu_quota_percent(0),   // Default: 0% means no guaranteed quota initially
        quota_period_usage(0),
        current_quota_allowance(0),
        quota_exhausted(false)
    {
        B_INITIALIZE_SPINLOCK(&lock);
    }

    // No complex destructor needed for now as it doesn't own heap resources directly.
    // ~TeamSchedulerData();
};

} // namespace Scheduler

#endif // KERNEL_SCHEDULER_TEAM_H
