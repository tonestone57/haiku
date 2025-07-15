#ifndef _KSU_KSCHEDULER_H
#define _KSU_KSCHEDULER_H

#include <sched.h>

#ifdef __cplusplus
extern "C" {
#endif

int sched_get_priority_max(int policy);

#ifdef __cplusplus
}
#endif

#endif /* _KSU_KSCHEDULER_H */
