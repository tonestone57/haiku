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
    (Note: ``SCHEDULER_WEIGHT_SCALE`` and ``thread_weight`` are conceptual; the
    current implementation uses a simplified, unweighted addition of
    ``actual_duration`` to ``fVirtualRuntime`` as a placeholder).
  - **Initialization**: When a new thread becomes runnable or a sleeping thread
    wakes, its ``fVirtualRuntime`` is typically initialized to be slightly
    greater than or equal to the minimum virtual runtime currently in its target
    CPU's run queue. This ensures fairness and prevents immediate starvation or
    unfair preemption.

Lag (``fLag``)
~~~~~~~~~~~~~~
Lag measures the difference between the CPU time a thread *should have* received
(its fair entitlement based on virtual time progression) and the CPU time it
*actually* received.

  - **Positive Lag**: The thread has received less CPU time than its fair share.
    It is "owed" CPU time.
  - **Negative Lag**: The thread has received more CPU time than its fair share.
    It has a "debt" of CPU time.
  - **Calculation**:
    ``Lag = CurrentEntitlement - ServiceReceived``
    Conceptually, when a thread runs, its lag decreases by the amount of service
    (weighted runtime) it consumed. When it's time for it to get a new slice, its
    lag increases by the duration of that new slice (its entitlement).
    The precise calculation involves comparing the thread's virtual runtime with
    a reference virtual time (e.g., the minimum virtual runtime of the run queue).
    (Note: Current implementation uses simplified updates to ``fLag``).

Slice Duration (``fSliceDuration``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
This is the target duration a thread should run for once it's selected by the
scheduler before its state (lag, deadline) is re-evaluated.

  - **Determination**: Influenced by the thread's base priority and potentially
    a future "latency-nice" parameter. Higher priority or more latency-sensitive
    threads might receive shorter slices to allow for quicker preemption and
    turnaround, even if their total CPU share over time is the same as other
    threads of similar weight.
  - **Current Implementation**: Derived from Haiku's existing priority levels
    using ``ThreadData::GetBaseQuantumForLevel(ThreadData::MapPriorityToMLFQLevel(...))``.

Eligible Time (``fEligibleTime``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
The wall-clock time at which a thread becomes eligible to run.

  - **Calculation**:
    If ``Lag >= 0``, ``EligibleTime = system_time()`` (eligible immediately).
    If ``Lag < 0``, ``EligibleTime = system_time() + (-Lag / ServiceRate)``, where
    ``ServiceRate`` is related to the thread's weight. This means a thread that
    has run too much (negative lag) will only become eligible again in the future
    once its entitlement "catches up."
    (Note: Current implementation uses a placeholder for negative lag eligibility).

Virtual Deadline (``fVirtualDeadline``)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
The core scheduling key. It's the wall-clock time by which a thread *should*
ideally complete its current ``SliceDuration``, considering its eligibility.

  - **Calculation**: ``VirtualDeadline = EligibleTime + (SliceDuration / NormalizedWeight)``
    For a simplified, unweighted interpretation or where slice duration is already
    weight-adjusted: ``VirtualDeadline = EligibleTime + SliceDuration``.
  - **Scheduling Decision**: The scheduler picks the *eligible* thread with the
    *earliest* (smallest) ``VirtualDeadline``.

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
    *   ``fSliceDuration``: Calculated (currently based on priority via MLFQ mapping).
    *   ``fVirtualRuntime``: Initialized. Ideally, set to be slightly >= the minimum
      virtual runtime of the target CPU's EEVDF run queue to ensure fairness.
      (Current implementation has a placeholder for this).
    *   ``fLag``: Initialized (e.g., to 0, or based on preserved entitlement if waking).
      (Current implementation resets to 0).
    *   ``fEligibleTime``: Calculated based on current time and lag.
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
    *   Then, it calls ``CPUEntry::PeekEligibleNextThread()`` to find the thread
      in its ``fEevdfRunQueue`` with the earliest virtual deadline that is also
      currently eligible (``Lag >= 0`` or ``system_time() >= EligibleTime``).
      (Note: The eligibility check in ``PeekEligibleNextThread`` is currently a TODO).
    *   If no eligible non-idle thread is found, the CPU's designated idle thread
      (``CPUEntry::fIdleThread``) is chosen.
    *   The chosen non-idle ``nextThread`` is removed from the ``EevdfRunQueue``.
4.  **New Thread Setup**:
    *   The ``nextThread``'s state is set to ``B_THREAD_RUNNING``.
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
    The existing mechanism in ``scheduler_perform_load_balance()`` for identifying
    overloaded and underloaded cores (using ``gCoreHighLoadHeap`` and
    ``gCoreLoadHeap`` based on historical core load) is retained.
    *   *Thread Selection for Migration*: The logic to pick a specific thread from
      the source CPU's EEVDF run queue is currently a placeholder and needs
      a more EEVDF-aware strategy (e.g., migrating threads with high positive lag).
    *   *Parameter Re-initialization*: When a thread is migrated, its EEVDF
      parameters (especially ``fVirtualRuntime`` and ``fLag``) must be
      re-initialized appropriately for the target CPU's run queue. This is
      also currently a placeholder.
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

TODOs / Future Work (Key Placeholders)
--------------------------------------
The current EEVDF implementation is structural. Key algorithmic details
require further work:

  - **Precise Virtual Runtime Management**:
    *   Correct initialization for new/waking threads relative to the target
      queue's minimum virtual runtime.
    *   Accurate weighted advancement of virtual runtime based on priority/weight.
  - **Accurate Lag Calculation**: Based on the difference between a thread's
    virtual runtime and the run queue's reference virtual time.
  - **Eligible Time Calculation**: Proper calculation for threads with negative
    lag, considering their service rate (weight).
  - **Eligibility Check**: Robust implementation in
    ``CPUEntry::PeekEligibleNextThread()`` and ``reschedule()``.
  - **Slice Duration Calculation**: Implement "latency-nice" and use it alongside
    priority to determine slice durations for better latency control.
  - **Load Balancing Thread Selection**: Develop a better heuristic for choosing
    which thread to migrate from an EEVDF queue.
  - **Tie-Breaking**: Refine tie-breaking in ``_scheduler_select_cpu_on_core``
    for EEVDF if virtual deadlines or fitness scores are equal.
  - **Real-time Threads**: The current EEVDF implementation primarily targets
    normal (FAIR) threads. How real-time priorities integrate or bypass EEVDF
    needs to be clearly defined (currently, they'd likely get very short slices
    and early deadlines via the temporary priority mapping). A separate
    real-time scheduling class might still be needed alongside EEVDF.
  - **Testing and Benchmarking**: Extensive testing is required.
