/*
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SCHEDULER_WEIGHTS_H_
#define _SCHEDULER_WEIGHTS_H_

#include <OS.h>

struct CPUEntry;
struct Thread;

void scheduler_init_weights();
int32 scheduler_priority_to_weight(Thread* thread, CPUEntry* cpu = NULL);

#endif // _SCHEDULER_WEIGHTS_H_
