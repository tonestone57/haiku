# Design for a New Work-Stealing Algorithm

## 1. Overview

This document outlines the design for a new work-stealing algorithm for the Haiku kernel. The new algorithm aims to be more selective, take into account the cost of moving a thread, and be more aggressive than the existing algorithm.

## 2. Key Improvements

### 2.1. Dynamic Starvation Threshold

The existing algorithm uses a fixed threshold (`kMinUnweightedNormWorkToSteal`) to determine if a task is starved. This can be suboptimal, as the ideal threshold may vary depending on the system load.

The new algorithm will use a dynamic starvation threshold that is calculated as follows:

```
dynamic_threshold = base_threshold * (1 + (thief_load - victim_load))
```

where:

*   `base_threshold` is a new tunable parameter that will replace `kMinUnweightedNormWorkToSteal`.
*   `thief_load` is the load on the thief CPU.
*   `victim_load` is the load on the victim CPU.

This dynamic threshold will make the algorithm more aggressive when the thief CPU is idle and the victim CPU is busy, and less aggressive when the load is more balanced.

### 2.2. Cache-Aware Migration Cost

The existing algorithm only considers the cost of migration in a very basic way, by using a cooldown period. The new algorithm will introduce a more sophisticated metric that estimates the cache-warming cost of migrating a thread.

The migration cost will be calculated as follows:

```
migration_cost = base_cost * distance(thief_cpu, victim_cpu)
```

where:

*   `base_cost` is a new tunable parameter.
*   `distance(thief_cpu, victim_cpu)` is a function that returns the distance between the thief and victim CPUs in the CPU topology. The distance will be 0 for SMT siblings, 1 for different cores in the same package, and 2 for different packages.

The migration cost will be used to penalize migrations between distant CPUs. The starvation threshold will be adjusted as follows:

```
adjusted_threshold = dynamic_threshold + migration_cost
```

A task will only be stolen if its `unweightedNormWorkOwed` is greater than the `adjusted_threshold`.

### 2.3. Simplified and More Aggressive big.LITTLE Policy

The existing big.LITTLE policy is quite complex and can be difficult to tune. The new algorithm will use a simplified policy that is more aggressive in moving tasks to "big" cores.

The new policy will be as follows:

*   A task running on a "little" core can be stolen by a "big" core if the task is starved, regardless of its priority or the load on the "big" core.
*   A task running on a "big" core can only be stolen by a "little" core if the "big" core is overloaded and the "little" core is idle.

This new policy will ensure that high-priority tasks are quickly moved to "big" cores, while still allowing for some load balancing between "big" and "little" cores.

## 3. Implementation Details

The new algorithm will be implemented by modifying the `_attempt_one_steal` function in `scheduler.cpp`. The `scheduler_try_work_steal` function will not need to be changed.

The following new tunable parameters will be introduced:

*   `base_starvation_threshold`
*   `migration_cost_base`

These parameters will be exposed through the `/dev/scheduler` interface, so that they can be tuned without recompiling the kernel.

The following new debugger commands will be introduced:

*   `dump_ws_stats`: Dumps work-stealing statistics.
*   `set_starvation_threshold <threshold>`: Sets the base starvation threshold.
*   `get_starvation_threshold`: Gets the base starvation threshold.
*   `set_migration_cost <cost>`: Sets the base migration cost.
*   `get_migration_cost`: Gets the base migration cost.
