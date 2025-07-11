.. SPDX-License-Identifier: MIT

=================================
EEVDF Scheduler in Haiku (Draft)
=================================

.. contents::
   :local:

Introduction
------------

This document describes the implementation of the Earliest Eligible Virtual
Deadline First (EEVDF) scheduler in Haiku. EEVDF is a weighted fair queuing
scheduler designed to provide fair CPU time allocation among competing threads
while also catering to latency-sensitive applications. It aims to replace complex
heuristics found in some traditional schedulers with more deterministic,
algorithmically driven decisions.

This implementation is a clean-room design based on the principles of EEVDF,
without direct use of Linux kernel code, and is licensed under the MIT license.

Core Concepts
-------------

The EEVDF scheduler revolves around several key concepts to manage thread execution:

Virtual Runtime (``fVirtualRuntime``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Each thread accumulates virtual runtime as it executes. This is not wall-clock
time but rather actual runtime weighted by the thread's priority (or "weight").
A higher priority (lower nice value) thread has a higher weight, causing its
virtual runtime to advance slower relative to CPU time consumed, effectively
giving it a larger share of the CPU.

  - **Calculation**: When a thread runs for ``actual_duration``, its
    ``virtual_runtime`` increases by approximately
    ``(actual_duration * SCHEDULER_WEIGHT_SCALE) / thread_weight``.
    ``(actual_duration * SCHEDULER_WEIGHT_SCALE) / thread_weight``.
  - **Initialization**: When a new thread becomes runnable or a sleeping thread
    wakes, its ``fVirtualRuntime`` is initialized to ``max(current_vruntime, min_vruntime_of_target_queue)``.
    This ensures fairness and prevents immediate starvation or unfair preemption.

Lag (``fLag``)
~~~~~~~~~~~~~~
Lag measures the difference between the CPU time a thread *should have* received
(its fair entitlement based on virtual time progression) and the CPU time it
*actually* received, expressed in weighted time units.

  - **Positive Lag**: The thread has received less CPU time than its fair share.
    It is "owed" CPU time.
  - **Negative Lag**: The thread has received more CPU time than its fair share.
    It has a "debt" of CPU time.
  - **Calculation**:
    Upon enqueue/requeue: ``Lag = WeightedSliceEntitlement - (VirtualRuntime - QueueMinVirtualRuntime)``.
    When service is received: ``Lag -= WeightedServiceReceived``.
    When new entitlement is granted (for next slice): ``Lag += WeightedSliceEntitlement``.

Slice Duration (``fSliceDuration``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
This is the target wall-clock duration a thread should run for once it's selected by the
scheduler before its state (lag, deadline) is re-evaluated.

  - **Determination**: Influenced by the thread's base priority (converted to a weight),
    its real-time status, I/O behavior heuristics, and system contention levels.
    The ``fLatencyNice`` parameter and its associated syscalls for direct slice
    duration tuning have been removed.
  - **Current Implementation**: Derived using
    ``ThreadData::CalculateDynamicQuantum()`` which incorporates these factors.

Eligible Time (``fEligibleTime``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
The wall-clock time at which a thread becomes eligible to run.

  - **Calculation**:
    If ``Lag >= 0`` (thread is owed time or is on schedule), ``EligibleTime = system_time()`` (eligible immediately).
    If ``Lag < 0`` (thread has run too much),
    ``EligibleTime = system_time() + Delay``, where ``Delay`` is calculated as
    ``(-Lag_weighted_units * SCHEDULER_WEIGHT_SCALE) / thread_weight_units``.
    The delay is capped (e.g., ``SCHEDULER_TARGET_LATENCY * 2``) and has a minimum floor (``SCHEDULER_MIN_GRANULARITY``).

Virtual Deadline (``fVirtualDeadline``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
The core scheduling key. It's the wall-clock time by which a thread *should*
ideally complete its current ``fSliceDuration``, considering its eligibility.

  - **Calculation**: ``VirtualDeadline = EligibleTime + SliceDuration``.
    Both ``EligibleTime`` and ``SliceDuration`` are wall-clock times. The EEVDF
    fairness properties emerge from how ``VirtualRuntime``, ``Lag``, and subsequently
    ``EligibleTime`` are managed in a weighted manner.
  - **Scheduling Decision**: The scheduler picks the *eligible* thread (where
    ``system_time() >= EligibleTime``) with the *earliest* (smallest) ``VirtualDeadline``.

Key Data Structures
-------------------

ThreadData EEVDF Fields
~~~~~~~~~~~~~~~~~~~~~~~
The ``ThreadData`` struct in ``src/system/kernel/scheduler/scheduler_thread.h``
has been augmented with the following fields for EEVDF:

  - ``bigtime_t fVirtualDeadline``: Stores the thread's calculated virtual deadline.
  - ``bigtime_t fLag``: Stores the thread's current lag.
  - ``bigtime_t fEligibleTime``: Stores the time the thread becomes eligible.
  - ``bigtime_t fSliceDuration``: The target runtime for the current slice.
  - ``bigtime_t fVirtualRuntime``: Accumulated weighted runtime.
  - ``Scheduler::EevdfRunQueueLink fEevdfLink``: Link for the EEVDF run queue.

EevdfRunQueue
~~~~~~~~~~~~~
Defined in ``src/system/kernel/scheduler/EevdfRunQueue.h`` and ``.cpp``.

  - Each ``CPUEntry`` maintains one ``EevdfRunQueue``.
  - It uses Haiku's ``Util::Heap`` internally.
  - Stores ``ThreadData*`` pointers.
  - Ordered by ``VirtualDeadline`` (earliest deadline at the top/root of the heap)
    using a custom ``EevdfDeadlineCompare`` policy.
  - Provides methods: ``Add()``, ``Remove()``, ``PeekMinimum()``, ``PopMinimum()``,
    ``Update()`` (currently remove+add).
  - Access is protected by a spinlock within the ``EevdfRunQueue`` object.

Scheduling Logic
----------------

Thread Enqueueing (New/Wakeup)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Handled by ``scheduler_enqueue_in_run_queue()`` in ``scheduler.cpp``:

1.  **Target CPU/Core Selection**: ``ThreadData::ChooseCoreAndCPU()`` is called.
    This considers affinity and current scheduler mode policies. The underlying
    CPU fitness metrics (load) used by ``_ChooseCPU`` are still relevant.
2.  **EEVDF Parameter Initialization**: For the thread being enqueued:
    *   ``fSliceDuration``: Calculated using ``ThreadData::CalculateDynamicQuantum()``,
      which considers base priority (weight) and other heuristics.
    *   ``fVirtualRuntime``: Initialized to be ``max(current_vruntime, min_vruntime_of_target_queue)``.
    *   ``fLag``: Calculated as ``WeightedSliceEntitlement - (VirtualRuntime - QueueMinVirtualRuntime)``.
    *   ``fEligibleTime``: Calculated based on current time and the new ``fLag``.
    *   ``fVirtualDeadline``: Calculated as ``fEligibleTime + fSliceDuration``.
3.  **Add to Run Queue**: The thread is added to the target ``CPUEntry``'s
    ``fEevdfRunQueue`` using ``CPUEntry::AddThread()``.
4.  **Invoke Scheduler**: If the newly enqueued thread might preempt the currently
    running thread on the target CPU (i.e., it's eligible and has an earlier
    virtual deadline), an IPI is sent or a reschedule flag is set.

Reschedule Operation
~~~~~~~~~~~~~~~~~~~~
The main ``reschedule()`` function in ``scheduler.cpp`` is invoked when the
current thread blocks, yields (conceptually), its slice ends, or a higher
priority (earlier deadline) thread becomes runnable.

1.  **Old Thread Accounting**:
    *   The ``oldThread`` (currently running) has its CPU time usage updated.
    *   Its ``fVirtualRuntime`` is advanced by the weighted time it just ran.
    *   Its ``fLag`` is reduced by the service (weighted time) it received.
2.  **Old Thread Re-Enqueue (if still runnable)**:
    *   If ``oldThread`` is still ready to run and not the idle thread:
        *   Its ``fSliceDuration`` is determined for its next execution period.
        *   Its ``fLag`` is increased by this new ``fSliceDuration`` (entitlement).
        *   New ``fEligibleTime`` and ``fVirtualDeadline`` are calculated.
        *   It's re-inserted into the current CPU's ``EevdfRunQueue`` by
          ``CPUEntry::ChooseNextThread()`` (which calls ``CPUEntry::AddThread()``).
3.  **Select Next Thread**:
    *   ``CPUEntry::ChooseNextThread()`` is called.
    *   It first considers re-enqueueing ``oldThread`` as above if applicable.
    *   Then, it calls the (now non-const) ``CPUEntry::PeekEligibleNextThread()``.
      This method iterates through the CPU's ``fEevdfRunQueue`` (by temporarily
      popping and re-adding entries) to find the first thread (ordered by
      ``VirtualDeadline``) that is currently eligible (i.e., ``system_time() >= EligibleTime``).
    *   If an eligible non-idle thread is found, ``PeekEligibleNextThread``
      removes it from the run queue and returns it.
    *   If no eligible non-idle thread is found, ``CPUEntry::ChooseNextThread()``
      selects the CPU's designated idle thread (``CPUEntry::fIdleThread``).
4.  **New Thread Setup**:
    *   The chosen ``nextThread`` (which could be an active thread or the idle thread)
      has its state set to ``B_THREAD_RUNNING``.
    *   Its CPU time accounting starts.
    *   The hardware timer is set to fire after ``nextThread->SliceDuration()``.
5.  **Context Switch**: If ``nextThread`` is different from ``oldThread``, a context
    switch occurs.

Priority and Weight Handling
~~~~~~~~~~~~~~~~~~~~~~~~~~~~
The EEVDF scheduler uses a combination of thread priority and its derived weight
to influence thread behavior. The direct ``latency-nice`` parameter for slice
tuning has been removed.

  - **Priority and Weight**: A thread's base priority (typically corresponding
    to its "nice" value) is converted into a numerical "weight" via the
    ``scheduler_priority_to_weight()`` function. This function now utilizes a
    continuous mapping (``gHaikuContinuousWeights`` generated by
    ``calculate_continuous_haiku_weight_prototype``) for more granular weight
    assignments across the Haiku priority spectrum. A higher priority results
    in a higher weight. This weight is fundamental to EEVDF:
    *   It scales how ``fVirtualRuntime`` advances:
        ``virtual_increment = (actual_duration * SCHEDULER_WEIGHT_SCALE) / thread_weight``.
        Higher weight means slower virtual runtime advancement for the same CPU
        time, leading to a larger CPU share.
    *   It scales the "weighted slice entitlement" used in ``fLag`` calculations.

Real-Time Thread Handling
~~~~~~~~~~~~~~~~~~~~~~~~~
Threads with priorities ``B_REAL_TIME_DISPLAY_PRIORITY`` (20) and above receive
special treatment to enhance their real-time characteristics:

  - **High Weights**: They are assigned very high weights by
    ``scheduler_priority_to_weight()``, ensuring they are strongly favored by
    the EEVDF fairness calculations.
  - **Immediate Eligibility**: When a real-time thread (priority >= 20) becomes
    runnable (e.g., wakes from sleep or is newly created), its ``fEligibleTime``
    is set to the current system time. This allows it to preempt lower-priority
    threads immediately, without being subject to potential delays from negative
    lag that normal threads might experience. This behavior is primarily governed
    by the ``ThreadData::IsRealTime()`` check (which now uses
    ``B_REAL_TIME_DISPLAY_PRIORITY`` as its threshold) within
    ``ThreadData::UpdateEevdfParameters()``.
  - **Minimum Guaranteed Slice**: Real-time threads (priority >= 20) are
    guaranteed a minimum slice duration defined by ``RT_MIN_GUARANTEED_SLICE``
    (typically 2ms). This prevents their slice from becoming excessively short
    due to very high weights, which could lead to high scheduling overhead.
    This floor is applied in ``ThreadData::CalculateDynamicQuantum()``.

The combination of these factors (very high weight, immediate eligibility, and
a guaranteed minimum slice leading to frequent re-evaluation with early
virtual deadlines) allows EEVDF to provide strong soft real-time performance,
enabling RT threads to be highly responsive and preemptive.

Timer Management
----------------
The primary scheduler timer associated with a running thread (``cpu->quantum_timer``)
is set by ``CPUEntry::StartQuantumTimer()`` within ``reschedule()``.
  - For non-idle threads, this timer is set to the thread's current
    ``fSliceDuration``. When it fires, it triggers ``reschedule()``.
  - For idle threads, a longer periodic timer is set, primarily to ensure
    periodic load updates (``_UpdateLoadEvent``).

EEVDF does not use an aging timer like MLFQ. Fairness and starvation prevention
are handled by the lag and virtual runtime mechanisms.

Load Balancing, SMT, and Cache Awareness
----------------------------------------
These aspects are handled as follows:

  - **Load Balancing**:
    The mechanism in ``scheduler_perform_load_balance()`` identifies
    overloaded and underloaded cores.
    *   *Thread Selection for Migration*: From the source CPU's EEVDF run queue,
      it selects a migratable thread, prioritizing those with significant
      positive ``fLag`` (i.e., threads that are "owed" CPU time).
    *   *Parameter Re-initialization*: When a thread is migrated, its EEVDF
      parameters (``fVirtualRuntime``, ``fLag``, ``fEligibleTime``, ``fVirtualDeadline``)
      are re-initialized relative to the target CPU's run queue state.
  - **SMT Awareness**:
    ``_scheduler_select_cpu_on_core()`` includes a penalty for selecting a CPU
    whose SMT siblings are busy. This logic, scaled by
    ``gSchedulerSMTConflictFactor``, is retained as it's generally beneficial.
  - **Cache Awareness**:
    Mechanisms like ``ThreadData::HasCacheExpired()`` and the preference for
    ``fThread->previous_cpu`` in ``ThreadData::_ChooseCPU()`` (if still on the
    chosen core and cache is warm) are retained. These are largely orthogonal
    to the core scheduling algorithm.

Interaction with Scheduler Modes
--------------------------------
The existing scheduler modes (Low Latency, Power Saving) are adapted:

  - **``switch_to_mode()``**: Assignments to MLFQ-specific parameters like
    ``gSchedulerAgingThresholdMultiplier`` are removed. Settings for
    ``gSchedulerSMTConflictFactor``, IRQ balancing parameters, and
    ``gSchedulerLoadBalancePolicy`` (SPREAD vs. CONSOLIDATE) are retained and
    set by each mode. The role of ``gKernelKDistFactor`` is currently diminished
    but kept.
  - **``choose_core()``**: The mode-specific core selection logic remains, as it
    relies on load metrics, cache affinity, and consolidation strategies that
    are still relevant to EEVDF.
  - **Power Saving Consolidation**: The concept of ``sSmallTaskCore`` and related
    functions in power-saving mode are retained.

Big.LITTLE Architecture Awareness
---------------------------------
The EEVDF scheduler incorporates awareness for heterogeneous CPU architectures
(e.g., Arm's big.LITTLE) to optimize task placement and energy efficiency:

  - **Load Balancing**: The ``scheduler_perform_load_balance()`` mechanism is
    type-aware.
    *   The load difference required to trigger migration between cores can vary
      based on the types of the source and target cores (e.g., P-core vs. E-core),
      as determined by ``scheduler_get_bl_aware_load_difference_threshold()``.
    *   When selecting a thread to migrate, the benefit score considers task
      characteristics such as "P-critical" (prefers Performance-cores) or
      "E-preferring" (suitable for Efficiency-cores) and the type compatibility
      between the task and potential target cores.
  - **Work Stealing**: The ``_attempt_one_steal()`` logic is also b.L-aware. For
    instance, E-cores are more conservative about stealing P-critical tasks from
    P-cores, potentially only doing so if all P-cores are saturated and the task
    is light enough.
  - **Capacity-Aware Calculations**: Virtual runtime (``fVirtualRuntime``) and lag
    (``fLag``) calculations are normalized by the performance capacity of the core
    a thread runs on. This ensures that a thread consuming CPU time on a
    lower-capacity E-core is accounted for fairly relative to a thread running
    on a higher-capacity P-core.
  - **CPU Selection on Core**: The tie-breaking logic in
    ``_scheduler_select_cpu_on_core()``, when choosing between logical CPUs (SMT
    threads) on the same physical core, now uses EEVDF-specific metrics like
    run queue depth and minimum virtual runtime, in addition to SMT-aware load scores.

IRQ Handling and Task Colocation
--------------------------------
The scheduler includes several mechanisms for managing Interrupt Request (IRQ)
handling, aiming to improve efficiency and allow for task-specific optimizations:

  - **IRQ-Task Colocation Syscall**: The ``_user_set_irq_task_colocation()``
    syscall allows privileged applications to create an affinity between a
    specific IRQ vector and a thread. When such an affinity exists, the
    scheduler attempts to handle the IRQ on the same CPU or core where the
    affinitized thread is running or homed. This is particularly useful for
    network-intensive applications or other I/O workloads where processing IRQ
    data on the same core that handles the consuming thread can improve cache
    locality and reduce latency. Clearing the affinity reverts the IRQ to
    system-wide balancing.
  - **IRQ Following Task Migration**: When a thread with affinitized IRQs is
    migrated between cores by the load balancer, the
    ``scheduler_maybe_follow_task_irqs()`` function is invoked. This function
    evaluates whether to also move the affinitized IRQs to the thread's new
    core. This decision considers IRQ movement cooldowns and the suitability
    of CPUs on the new core (via ``SelectTargetCPUForIRQ``).
  - **Dynamic IRQ Target Load**: The maximum IRQ load a CPU is considered
    capable of handling is dynamic (``scheduler_get_dynamic_max_irq_target_load()``).
    CPUs currently busy with thread execution will have a reduced IRQ handling
    capacity, encouraging IRQs to be placed on less busy CPUs.
  - **Task-Contextual IRQ Re-evaluation**: During ``reschedule()``, if a highly
    latency-sensitive thread is about to run, the
    ``_find_quiet_alternative_cpu_for_irq()`` mechanism may attempt to move
    potentially disruptive, heavy IRQs off the CPU where this sensitive thread
    will execute. This helps protect latency-critical tasks from IRQ interference.
  - **Intelligent IRQ Placement**: The ``SelectTargetCPUForIRQ()`` function, used
    by various IRQ balancing and colocation mechanisms, makes an informed choice
    when selecting a target CPU for an IRQ. It considers current IRQ load on
    candidate CPUs, their thread load (including SMT penalties), CPU energy
    efficiency (on b.L systems), and any explicit task affinity for the IRQ.

Relevant Debugger Commands
--------------------------
  - ``eevdf_run_queue`` (aliased to ``run_queue``): Dumps the state of the
    EEVDF run queue for each CPU, showing thread ID, virtual deadline, lag, etc.
    This command iterates the heap to display multiple queued threads.
  - ``thread_sched_info <id>``: Dumps detailed EEVDF parameters, load metrics,
    and affinity information for a specific thread.
  - Other commands like ``threads``, ``cpu``, ``scheduler_get_smt_factor``
    remain relevant.

TODOs / Future Work
-------------------
The EEVDF scheduler has undergone significant development, incorporating
mechanisms for weighted fair queuing, latency-nice control, big.LITTLE
awareness, and advanced IRQ handling. Key areas previously marked as TODOs,
such as refined priority-to-weight mapping,
fairness in ``scheduler_set_thread_priority()``, enhanced tie-breaking, and
b.L-aware load balancing/work-stealing, have been substantially addressed.
The user-space control for a separate latency-nice parameter has been removed.

Ongoing and future areas for refinement and investigation include:

  - **Parameter Tuning and Validation**:
    *   **Priority-to-Weight and Slice Durations**: Extensive real-world testing
      and benchmarking are needed to fine-tune the ``gHaikuContinuousWeights``
      generation, ``SCHEDULER_TARGET_LATENCY``, ``SCHEDULER_MIN_GRANULARITY``, and
      ``RT_MIN_GUARANTEED_SLICE``. The goal is to optimize Haiku application
      responsiveness, fairness, and real-time performance across diverse workloads
      and hardware.
    *   **Load Balancing & Work Stealing Heuristics**: The numerous factors and
      thresholds within the load balancing benefit score and work-stealing logic
      (including b.L specific heuristics) require empirical validation and tuning.
    *   **IRQ Handling Parameters**: Default values for IRQ balancing intervals,
      load thresholds, and cooldowns should be reviewed with performance data.

  - **Real-Time Thread Integration**:
    *   The current model (priority >= 20 gets high weight, immediate eligibility,
      and `RT_MIN_GUARANTEED_SLICE`) aims to provide good soft RT behavior.
      Ongoing evaluation with demanding RT applications (e.g., professional audio)
      is needed to confirm its sufficiency and identify any remaining jitter or
      latency issues.
    *   Further investigation might explore if specific, very high priorities could
      benefit from even more specialized handling, though dedicated RT scheduling
      classes (like FIFO/RR) are currently deferred due to complexity concerns.

  - **Comprehensive Testing and Benchmarking**:
    *   A dedicated testing phase is crucial to validate the correctness,
      stability, and performance of all scheduler enhancements across various
      hardware (single-core, SMP, different big.LITTLE configurations if possible).
    *   This includes synthetic benchmarks targeting fairness, latency, and
      throughput, as well as real-world application performance profiles.

  - **Scheduler Profiling and Tracing Framework**:
    *   The existing ``SCHEDULER_TRACING`` macros provide valuable insight.
      However, the ``SCHEDULER_PROFILING`` hooks could be developed into a more
      comprehensive framework if deeper performance analysis or bottleneck
      identification becomes necessary.

  - **Advanced Architectural Considerations (Longer Term)**:
    *   **NUMA Awareness**: For systems with Non-Uniform Memory Access, making the
      scheduler NUMA-aware (e.g., by preferring to schedule threads on CPUs
      local to their memory allocations) could provide significant performance
      benefits. This would be a major undertaking.
    *   **Power Management Integration**: Deeper integration with CPU idle state
      management (cpuidle) and frequency scaling (cpufreq) beyond the current
      ``sSmallTaskCore`` logic in power-saving mode could yield further energy
      savings, especially on complex SoCs.
