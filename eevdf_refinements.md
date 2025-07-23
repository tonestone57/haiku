# Proposed Refinements for the EEVDF Scheduler

## 1. Overview

This document outlines a set of proposed refinements for the EEVDF scheduler in the Haiku kernel. These refinements aim to improve the performance, adaptability, and clarity of the scheduler.

## 2. Proposed Refinements

### 2.1. Adaptive Slice Duration

The current slice duration calculation in `CalculateDynamicQuantum` will be replaced with a simpler and more adaptive mechanism. The new mechanism will be based on the following formula:

```
slice_duration = target_latency * (weight / total_weight)
```

where:

*   `target_latency` is a tunable parameter that represents the desired scheduling latency.
*   `weight` is the weight of the thread.
*   `total_weight` is the total weight of all runnable threads on the CPU.

This new mechanism will automatically adjust the slice duration based on the number of runnable threads, which will help to improve the responsiveness of the system under high load.

### 2.2. Improved I/O-Bound Thread Handling

The I/O-bound thread detection logic will be improved by considering the number of voluntary context switches a thread has made, in addition to the average run burst time. A thread will be considered I/O-bound if it has made a high number of voluntary context switches and has a short average run burst time.

I/O-bound threads will be given a higher priority by the scheduler. This will be achieved by giving them a higher weight, which will result in a shorter slice duration and a lower virtual deadline.

### 2.3. Principled Interactivity Heuristics

The `fInteractivityClass` will be replaced with a more principled interactivity score. The interactivity score will be calculated based on a formula that takes into account both the average run burst time and the number of voluntary context switches.

The interactivity score will be used to adjust the thread's weight. Threads with a high interactivity score will be given a higher weight, which will result in a shorter slice duration and a lower virtual deadline.

### 2.4. EEVDF-Aware Work-Stealing

The work-stealing algorithm will be made more aware of the EEVDF parameters. When a CPU is looking for a task to steal, it will consider the virtual deadline and lag of the candidate tasks, in addition to the other factors that are already considered.

Tasks with a low virtual deadline and a high lag will be given a higher priority by the work-stealing algorithm. This will help to ensure that the most "starved" tasks are stolen first.

### 2.5. Code Clarity and Documentation

The EEVDF-related code will be refactored to improve its clarity and readability. More comments will be added to explain the logic of the code, and more descriptive variable names will be used.

## 3. Expected Benefits

The proposed refinements are expected to provide the following benefits:

*   **Improved responsiveness:** The adaptive slice duration will help to improve the responsiveness of the system under high load.
*   **Better I/O-bound thread handling:** The improved I/O-bound thread detection logic will help to ensure that I/O-bound threads are given a higher priority by the scheduler.
*   **More accurate interactivity heuristics:** The new interactivity score will provide a more accurate measure of a thread's interactivity, which will help to improve the scheduling of interactive threads.
*   **More effective work-stealing:** The EEVDF-aware work-stealing algorithm will be more effective at balancing the load across the available CPUs.
*   **Improved code maintainability:** The refactored code will be easier to understand and maintain.
