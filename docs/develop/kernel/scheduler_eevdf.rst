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

  - **Determination**: Influenced by the thread's base priority (via ``MapPriorityToEffectiveLevel``
    and ``kBaseQuanta``) and its ``fLatencyNice`` parameter (which defaults to 0).
    Higher priority or more latency-sensitive threads (lower ``fLatencyNice``)
    can receive shorter slices.
  - **Current Implementation**: Derived using
    ``ThreadData::CalculateDynamicQuantum()`` which incorporates base priority and ``fLatencyNice``.
    The ``fLatencyNice`` mechanism is structurally in place; user-space controls for it are future work.

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
      which considers base priority and ``fLatencyNice``.
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

Priority and Latency-Nice Handling (Intended)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  - **Priority (Nice Value)**: Affects a thread's "weight". Higher priority
    (lower nice value) means a higher weight. This translates to its
    ``fVirtualRuntime`` advancing slower for the same amount of CPU time,
    allowing it to receive more CPU time over a longer period. It also
    influences the base ``fSliceDuration``.
  - **Latency-Nice (Future)**: A planned parameter that would more directly
    control the ``fSliceDuration``. Threads with tighter latency requirements
    (lower latency-nice value) would get shorter slices, leading to earlier
    virtual deadlines and thus more responsive scheduling, without necessarily
    getting more *total* CPU time than other threads of similar weight.

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

Relevant Debugger Commands
--------------------------
  - ``eevdf_run_queue`` (aliased to ``run_queue``): Dumps the state of the
    EEVDF run queue for each CPU, showing thread ID, virtual deadline, lag, etc.
    (Note: Full heap iteration for dump is a TODO).
  - Other commands like ``threads``, ``cpu``, ``scheduler_get_smt_factor``
    remain relevant.

TODOs / Future Work
-------------------
The EEVDF implementation has been significantly fleshed out with core parameter
calculations (Virtual Runtime, Lag, Eligible Time) and eligibility checks now
reflecting EEVDF principles. Load balancing is also more EEVDF-aware.

Outstanding areas for future work and refinement include:

  - **Slice Duration and Latency-Nice**: The ``fLatencyNice`` field and its use in
    ``ThreadData::CalculateDynamicQuantum()`` are in place. Full user-space
    exposure via syscalls and development of policies for its use are future
    enhancements. The current slice duration derivation from ``kBaseQuanta``
    and ``MapPriorityToEffectiveLevel`` may also benefit from further tuning.
  - **Load Balancing Heuristics**: The current heuristic for selecting a thread
    to migrate (based on positive lag) is a good start. More advanced heuristics
    could be developed, potentially considering the target CPU's state more deeply.
  - **Tie-Breaking**: Tie-breaking in ``_scheduler_select_cpu_on_core`` (based
    on task count for CPUs with equal load scores) is generally acceptable but could
    be reviewed for EEVDF-specific scenarios if issues arise.
  - **``EevdfRunQueue::Update()`` Efficiency**: The `EevdfRunQueue::Update()` method,
    which is called when a thread's parameters (like `VirtualDeadline`) change
    (e.g., in `scheduler_set_thread_priority`), utilizes the underlying
    `SchedulerHeap::Update()` method. This heap update is efficient (O(log N)),
    performing a sift-up followed by a sift-down operation from the element's
    current position, rather than a less efficient remove-then-add sequence.
    The precondition is that the element's key (VirtualDeadline) must be
    updated by the caller *before* invoking the update. This is correctly
    handled by current callers.
  - **Priority-to-Weight Tuning**: The ``scheduler_priority_to_weight()`` mapping
    needs extensive real-world testing and tuning to achieve desired Haiku
    application responsiveness and fairness characteristics across the full
    spectrum of Haiku priorities.
  - **Interaction with ``scheduler_set_thread_priority()``**: When a thread's
    priority (and thus weight) changes, its current ``fLag`` and ``fVirtualRuntime``
    are not retrospectively adjusted against past execution. This could be a
    refinement for more immediate fairness upon priority change, though it adds
    complexity.
  - **Real-time Threads**: The current EEVDF implementation primarily targets
    normal (FAIR) threads. How real-time priorities integrate with or bypass EEVDF
    (currently they receive very high weights leading to short effective slices and
    early deadlines) needs ongoing evaluation. A separate, dedicated real-time
    scheduling class might still be beneficial alongside EEVDF for hard real-time
    guarantees.
  - **Testing and Benchmarking**: Extensive testing and benchmarking are crucial
    to validate correctness, tune parameters (like ``SCHEDULER_TARGET_LATENCY``,
    weights, slice duration constants), and measure performance across various
    workloads (interactive, batch, mixed).
