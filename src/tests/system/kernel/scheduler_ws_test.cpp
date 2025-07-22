/*
 * Copyright 2023, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <OS.h>
#include <stdio.h>
#include <stdlib.h>


static const int kThreadCount = 16;
static const int kRunTime = 10000000; // 10 seconds


static int32
busy_thread(void* data)
{
	bigtime_t startTime = system_time();
	while (system_time() - startTime < kRunTime) {
		// Do nothing, just keep the CPU busy.
	}
	return 0;
}


int
main(int argc, char** argv)
{
	thread_id threads[kThreadCount];

	for (int i = 0; i < kThreadCount; i++) {
		threads[i] = spawn_thread(busy_thread, "busy_thread", B_NORMAL_PRIORITY, NULL);
		if (threads[i] < 0) {
			fprintf(stderr, "Failed to spawn thread %d\n", i);
			return 1;
		}
	}

	for (int i = 0; i < kThreadCount; i++) {
		resume_thread(threads[i]);
	}

	for (int i = 0; i < kThreadCount; i++) {
		status_t result;
		wait_for_thread(threads[i], &result);
	}

	return 0;
}
