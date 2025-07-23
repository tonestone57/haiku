# Design for an EEVDF-Aware Work-Stealing Algorithm

## 1. Overview

This document outlines the design for a new EEVDF-aware work-stealing algorithm for the Haiku kernel. The new algorithm aims to make more informed decisions by taking into account the EEVDF parameters of the candidate tasks.

## 2. Key Improvements

### 2.1. Steal Score

The new algorithm will introduce a new metric called "steal score", which will be used to evaluate the candidate tasks. The steal score will be calculated as follows:

```
steal_score = (w1 * lag) - (w2 * virtual_deadline)
```

where:

*   `w1` and `w2` are tunable weights.
*   `lag` is the lag of the candidate task.
*   `virtual_deadline` is the virtual deadline of the candidate task.

The work-stealing algorithm will choose the task with the highest steal score. This will ensure that the algorithm prioritizes tasks that are both starved (high lag) and have a high urgency (low virtual deadline).

### 2.2. Tunable Weights

The weights `w1` and `w2` will be exposed as tunable parameters through the `/dev/scheduler` interface. This will allow system administrators to tune the work-stealing algorithm to their specific needs.

## 3. Implementation Details

The new algorithm will be implemented by modifying the `_attempt_one_steal` function in `scheduler.cpp`. The function will be modified to calculate the steal score for each candidate task and to choose the task with the highest score.

The following new tunable parameters will be introduced:

*   `ws_lag_weight`
*   `ws_vd_weight`

These parameters will be exposed through the debugger interface.
