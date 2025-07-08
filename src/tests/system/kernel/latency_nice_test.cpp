#include <OS.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h> // For sleep()

void busy_loop(bigtime_t duration, const char* name) {
    bigtime_t startTime = system_time();
    printf("Thread '%s' (ID: %" B_PRId32 ") starting busy loop for %" B_PRId64 " us.\n",
           name, find_thread(NULL), duration);
    fflush(stdout);
    while (system_time() - startTime < duration) {
        // Keep CPU busy
        for (volatile int i = 0; i < 1000; ++i);
    }
    printf("Thread '%s' (ID: %" B_PRId32 ") finished busy loop.\n", name, find_thread(NULL));
    fflush(stdout);
}

status_t thread_func_high_latency(void* data) {
    status_t status_set = set_thread_latency_nice(find_thread(NULL), 15); // Favor throughput
    if (status_set != B_OK) {
        printf("HighLatencyThread: Failed to set latency_nice: %s\n", strerror(status_set));
    }
    int8 current_nice_val;
    status_t status_get = get_thread_latency_nice(find_thread(NULL), &current_nice_val);
    if (status_get == B_OK) {
        printf("High latency thread (ID: %" B_PRId32 ") nice set to: %d (expected 15)\n",
               find_thread(NULL), current_nice_val);
    } else {
        printf("HighLatencyThread: Failed to get latency_nice: %s\n", strerror(status_get));
    }
    fflush(stdout);
    busy_loop(5000000, "HighLatency"); // 5 seconds
    return B_OK;
}

status_t thread_func_low_latency(void* data) {
    status_t status_set = set_thread_latency_nice(find_thread(NULL), -15); // Favor latency
    if (status_set != B_OK) {
        printf("LowLatencyThread: Failed to set latency_nice: %s\n", strerror(status_set));
    }
    int8 current_nice_val;
    status_t status_get = get_thread_latency_nice(find_thread(NULL), &current_nice_val);
    if (status_get == B_OK) {
        printf("Low latency thread (ID: %" B_PRId32 ") nice set to: %d (expected -15)\n",
               find_thread(NULL), current_nice_val);
    } else {
        printf("LowLatencyThread: Failed to get latency_nice: %s\n", strerror(status_get));
    }
    fflush(stdout);
    busy_loop(5000000, "LowLatency"); // 5 seconds
    return B_OK;
}

status_t thread_func_default_latency(void* data) {
    // Uses default latency nice
    int8 current_nice_val;
    status_t status_get = get_thread_latency_nice(find_thread(NULL), &current_nice_val);
    if (status_get == B_OK) {
        printf("Default latency thread (ID: %" B_PRId32 ") nice is: %d (expected %d)\n",
               find_thread(NULL), current_nice_val, LATENCY_NICE_DEFAULT);
    } else {
        printf("DefaultLatencyThread: Failed to get latency_nice: %s\n", strerror(status_get));
    }
    fflush(stdout);
    busy_loop(5000000, "DefaultLatency"); // 5 seconds
    return B_OK;
}

int main() {
    printf("Starting latency-nice test application.\n");
    printf("LATENCY_NICE_MIN: %d, LATENCY_NICE_MAX: %d, LATENCY_NICE_DEFAULT: %d\n",
           LATENCY_NICE_MIN, LATENCY_NICE_MAX, LATENCY_NICE_DEFAULT);

    thread_id main_thread_id = find_thread(NULL);
    printf("Main thread ID: %" B_PRId32 "\n", main_thread_id);
    fflush(stdout);

    int8 initial_nice_val;
    status_t status_get = get_thread_latency_nice(main_thread_id, &initial_nice_val);
    if (status_get == B_OK) {
        printf("Main thread initial latency_nice: %d (expected %d)\n", initial_nice_val, LATENCY_NICE_DEFAULT);
    } else {
        printf("Failed to get initial latency_nice for main thread: %s\n", strerror(status_get));
    }
    fflush(stdout);

    status_t status_set = set_thread_latency_nice(main_thread_id, 5);
    if (status_set == B_OK) {
        int8 current_nice_val;
        status_get = get_thread_latency_nice(main_thread_id, &current_nice_val);
        if (status_get == B_OK) {
            printf("Main thread latency_nice set to 5. New value: %d (expected 5)\n", current_nice_val);
        } else {
            printf("Main thread latency_nice set to 5, but failed to get new value: %s\n", strerror(status_get));
        }
    } else {
        printf("Failed to set latency_nice for main thread to 5: %s\n", strerror(status_set));
    }
    fflush(stdout);

    status_set = set_thread_latency_nice(main_thread_id, LATENCY_NICE_DEFAULT); // Reset
    if (status_set == B_OK) {
        int8 current_nice_val;
        status_get = get_thread_latency_nice(main_thread_id, &current_nice_val);
        if (status_get == B_OK) {
            printf("Main thread latency_nice reset to default. New value: %d (expected %d)\n",
                   current_nice_val, LATENCY_NICE_DEFAULT);
        } else {
            printf("Main thread latency_nice reset to default, but failed to get new value: %s\n", strerror(status_get));
        }
    } else {
        printf("Failed to reset latency_nice for main thread: %s\n", strerror(status_set));
    }
    fflush(stdout);

    // Test invalid values
    status_set = set_thread_latency_nice(main_thread_id, 127);
    if (status_set == B_BAD_VALUE) {
        printf("Correctly failed to set latency_nice to 127 (B_BAD_VALUE).\n");
    } else {
        printf("Incorrectly handled setting latency_nice to 127: %s (%" B_PRId32 ")\n", strerror(status_set), status_set);
    }
    fflush(stdout);
    status_set = set_thread_latency_nice(main_thread_id, -128);
    if (status_set == B_BAD_VALUE) {
        printf("Correctly failed to set latency_nice to -128 (B_BAD_VALUE).\n");
    } else {
        printf("Incorrectly handled setting latency_nice to -128: %s (%" B_PRId32 ")\n", strerror(status_set), status_set);
    }
    fflush(stdout);


    // Test with invalid thread ID
    int8 nice_invalid_thread_val;
    status_get = get_thread_latency_nice(-5, &nice_invalid_thread_val);
    if (status_get == B_BAD_THREAD_ID) {
        printf("Correctly failed to get latency_nice for invalid thread ID -5 (B_BAD_THREAD_ID).\n");
    } else {
        printf("Incorrectly handled get_thread_latency_nice for invalid ID -5: %s (val: %d)\n",
            strerror(status_get), nice_invalid_thread_val);
    }
    fflush(stdout);

    status_set = set_thread_latency_nice(-5, 0);
     if (status_set == B_BAD_THREAD_ID) {
        printf("Correctly failed to set latency_nice for invalid thread ID -5 (B_BAD_THREAD_ID).\n");
    } else {
        printf("Incorrectly handled set_thread_latency_nice for invalid thread ID -5: %s (%" B_PRId32 ")\n", strerror(status_set), status_set);
    }
    fflush(stdout);

    printf("\nSpawning threads with different latency preferences...\n");
    fflush(stdout);

    thread_id thread1 = spawn_thread(thread_func_high_latency, "HighLatencyThread", B_NORMAL_PRIORITY, NULL);
    thread_id thread2 = spawn_thread(thread_func_low_latency, "LowLatencyThread", B_NORMAL_PRIORITY, NULL);
    thread_id thread3 = spawn_thread(thread_func_default_latency, "DefaultLatencyThread", B_NORMAL_PRIORITY, NULL);

    if (thread1 < B_OK || thread2 < B_OK || thread3 < B_OK) {
        printf("Error spawning threads: t1=%" B_PRId32 ", t2=%" B_PRId32 ", t3=%" B_PRId32 "\n", thread1, thread2, thread3);
        return 1;
    }

    printf("Waiting for threads to complete (approx 5-10 seconds)...\n");
    fflush(stdout);
    status_t ret1, ret2, ret3;
    wait_for_thread(thread1, &ret1);
    wait_for_thread(thread2, &ret2);
    wait_for_thread(thread3, &ret3);

    printf("All threads finished.\n");
    printf("Test application finished.\n");
    fflush(stdout);

    return 0;
}
