/*
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SCHEDULER_WEIGHTS_H_
#define _SCHEDULER_WEIGHTS_H_

#include <OS.h>

namespace BKernel {
	struct Thread;
}

namespace Scheduler {
	class CPUEntry;
}

void scheduler_init_weights();
int32 scheduler_priority_to_weight(BKernel::Thread* thread, Scheduler::CPUEntry* cpu = NULL);

#endif // _SCHEDULER_WEIGHTS_H_
