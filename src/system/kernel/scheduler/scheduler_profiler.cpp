/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */

#include "scheduler_profiler.h"

#include <debug.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>

#ifdef SCHEDULER_PROFILING

using namespace Scheduler;
using namespace Scheduler::Profiling;

// Static members
Profiler* Profiler::sProfiler = NULL;
ProfilerSpinlock Profiler::sInstanceLock;

static int dump_profiler(int argc, char** argv);

// Architecture-independent time function
static inline nanotime_t
get_system_time_nsecs()
{
#if defined(__HAIKU__)
	return system_time_nsecs();
#elif defined(__linux__)
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
		return (nanotime_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
	}
	return 0;
#elif defined(_WIN32)
	LARGE_INTEGER frequency, counter;
	if (QueryPerformanceFrequency(&frequency) && 
	    QueryPerformanceCounter(&counter)) {
		return (counter.QuadPart * 1000000000LL) / frequency.QuadPart;
	}
	return 0;
#else
	// Fallback to microsecond precision
	return system_time() * 1000LL;
#endif
}

// Architecture-independent CPU count
static inline int32
get_cpu_count()
{
#if defined(__HAIKU__)
	extern int32 smp_get_num_cpus();
	return smp_get_num_cpus();
#elif defined(__linux__)
	return sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(_WIN32)
	SYSTEM_INFO sysinfo;
	GetSystemInfo(&sysinfo);
	return sysinfo.dwNumberOfProcessors;
#else
	return 1; // Conservative fallback
#endif
}

Profiler::Profiler()
	:
	fFunctionData(NULL),
	fFunctionDataCount(0),
	fInitialized(false),
	fStatus(B_NO_MEMORY)
{
	// Initialize arrays
	memset(fFunctionStacks, 0, sizeof(fFunctionStacks));
	memset(fFunctionStackPointers, 0, sizeof(fFunctionStackPointers));

	// Allocate function data array
	fFunctionData = new(std::nothrow) FunctionData[kMaxFunctionEntries];
	if (fFunctionData == NULL) {
		fStatus = B_NO_MEMORY;
		return;
	}
	memset(fFunctionData, 0, sizeof(FunctionData) * kMaxFunctionEntries);

	// Allocate function stacks for each CPU
	int32 cpuCount = get_cpu_count();
	if (cpuCount > SMP_MAX_CPUS)
		cpuCount = SMP_MAX_CPUS;

	for (int32 i = 0; i < cpuCount; i++) {
		fFunctionStacks[i] = new(std::nothrow) FunctionEntry[kMaxFunctionStackEntries];
		if (fFunctionStacks[i] == NULL) {
			fStatus = B_NO_MEMORY;
			return;
		}
		memset(fFunctionStacks[i], 0, sizeof(FunctionEntry) * kMaxFunctionStackEntries);
		
		// Initialize all entries as invalid
		for (uint32 j = 0; j < kMaxFunctionStackEntries; j++) {
			fFunctionStacks[i][j].fValid = false;
		}
	}

	fStatus = B_OK;
	fInitialized = true;
}

Profiler::~Profiler()
{
	Shutdown();
}

status_t
Profiler::Initialize()
{
	if (fInitialized)
		return B_OK;
		
	// This would be called from the constructor, but kept for compatibility
	return fStatus;
}

void
Profiler::Shutdown()
{
	if (!fInitialized)
		return;

	SpinlockGuard guard(&fFunctionLock);
	
	// Clean up function stacks
	int32 cpuCount = get_cpu_count();
	if (cpuCount > SMP_MAX_CPUS)
		cpuCount = SMP_MAX_CPUS;
		
	for (int32 i = 0; i < cpuCount; i++) {
		delete[] fFunctionStacks[i];
		fFunctionStacks[i] = NULL;
		fFunctionStackPointers[i] = 0;
	}

	// Clean up function data
	delete[] fFunctionData;
	fFunctionData = NULL;
	fFunctionDataCount = 0;
	
	fInitialized = false;
}

void
Profiler::EnterFunction(int32 cpu, const char* functionName)
{
	if (!fInitialized || !_IsValidCpu(cpu) || functionName == NULL)
		return;

	nanotime_t start = _GetCurrentTime();

	FunctionData* function = _FindFunction(functionName);
	if (function == NULL) {
		function = _AllocateFunction(functionName);
		if (function == NULL)
			return;
	}

	// Atomically increment call count
	AtomicOps::TestAndSet((volatile int32*)&function->fCalled, 
		function->fCalled + 1, function->fCalled);

	// Check stack bounds
	if (fFunctionStackPointers[cpu] >= kMaxFunctionStackEntries) {
		// Stack overflow - skip this entry
		return;
	}

	FunctionEntry* stackEntry = &fFunctionStacks[cpu][fFunctionStackPointers[cpu]];
	fFunctionStackPointers[cpu]++;

	stackEntry->fFunction = function;
	stackEntry->fEntryTime = start;
	stackEntry->fOthersTime = 0;
	stackEntry->fValid = true;

	nanotime_t stop = _GetCurrentTime();
	stackEntry->fProfilerTime = stop - start;
}

void
Profiler::ExitFunction(int32 cpu, const char* functionName)
{
	if (!fInitialized || !_IsValidCpu(cpu) || functionName == NULL)
		return;

	nanotime_t start = _GetCurrentTime();

	// Check stack bounds
	if (fFunctionStackPointers[cpu] == 0) {
		// Stack underflow - nothing to pop
		return;
	}

	fFunctionStackPointers[cpu]--;
	FunctionEntry* stackEntry = &fFunctionStacks[cpu][fFunctionStackPointers[cpu]];

	// Validate stack entry
	if (!stackEntry->fValid || stackEntry->fFunction == NULL) {
		return;
	}

	// Verify function name matches (safety check)
	if (strcmp(stackEntry->fFunction->fFunction, functionName) != 0) {
		// Function name mismatch - possible stack corruption
		stackEntry->fValid = false;
		return;
	}

	nanotime_t timeSpent = start - stackEntry->fEntryTime;
	if (timeSpent < stackEntry->fProfilerTime) {
		// Negative time - clock went backwards or overflow
		stackEntry->fValid = false;
		return;
	}
	
	timeSpent -= stackEntry->fProfilerTime;

	// Update timing statistics atomically
	SpinlockGuard guard(&fFunctionLock);
	
	FunctionData* function = stackEntry->fFunction;
	function->fTimeInclusive += timeSpent;
	
	nanotime_t exclusiveTime = timeSpent - stackEntry->fOthersTime;
	if (exclusiveTime >= 0) {
		function->fTimeExclusive += exclusiveTime;
		
		// Update min/max statistics
		if (function->fMinTimeInclusive == 0 || timeSpent < function->fMinTimeInclusive)
			function->fMinTimeInclusive = timeSpent;
		if (timeSpent > function->fMaxTimeInclusive)
			function->fMaxTimeInclusive = timeSpent;
			
		if (function->fMinTimeExclusive == 0 || exclusiveTime < function->fMinTimeExclusive)
			function->fMinTimeExclusive = exclusiveTime;
		if (exclusiveTime > function->fMaxTimeExclusive)
			function->fMaxTimeExclusive = exclusiveTime;
	}

	nanotime_t profilerTime = stackEntry->fProfilerTime;
	stackEntry->fValid = false;

	// Update parent function's timing
	if (fFunctionStackPointers[cpu] > 0) {
		stackEntry = &fFunctionStacks[cpu][fFunctionStackPointers[cpu] - 1];
		if (stackEntry->fValid) {
			stackEntry->fOthersTime += timeSpent;
			stackEntry->fProfilerTime += profilerTime;

			nanotime_t stop = _GetCurrentTime();
			stackEntry->fProfilerTime += stop - start;
		}
	}
}

void
Profiler::DumpCalled(uint32 maxCount)
{
	if (!fInitialized)
		return;

	SpinlockGuard guard(&fFunctionLock);
	uint32 count = _FunctionCount();

	if (count == 0) {
		kprintf("No profiling data available.\n");
		return;
	}

	qsort(fFunctionData, count, sizeof(FunctionData),
		&_CompareFunctions<uint32, &FunctionData::fCalled>);

	if (maxCount > 0 && maxCount < count)
		count = maxCount;
	_Dump(count);
}

void
Profiler::DumpTimeInclusive(uint32 maxCount)
{
	if (!fInitialized)
		return;

	SpinlockGuard guard(&fFunctionLock);
	uint32 count = _FunctionCount();

	if (count == 0) {
		kprintf("No profiling data available.\n");
		return;
	}

	qsort(fFunctionData, count, sizeof(FunctionData),
		&_CompareFunctions<bigtime_t, &FunctionData::fTimeInclusive>);

	if (maxCount > 0 && maxCount < count)
		count = maxCount;
	_Dump(count);
}

void
Profiler::DumpTimeExclusive(uint32 maxCount)
{
	if (!fInitialized)
		return;

	SpinlockGuard guard(&fFunctionLock);
	uint32 count = _FunctionCount();

	if (count == 0) {
		kprintf("No profiling data available.\n");
		return;
	}

	qsort(fFunctionData, count, sizeof(FunctionData),
		&_CompareFunctions<bigtime_t, &FunctionData::fTimeExclusive>);

	if (maxCount > 0 && maxCount < count)
		count = maxCount;
	_Dump(count);
}

void
Profiler::DumpTimeInclusivePerCall(uint32 maxCount)
{
	if (!fInitialized)
		return;

	SpinlockGuard guard(&fFunctionLock);
	uint32 count = _FunctionCount();

	if (count == 0) {
		kprintf("No profiling data available.\n");
		return;
	}

	qsort(fFunctionData, count, sizeof(FunctionData),
		&_CompareFunctionsPerCall<bigtime_t, &FunctionData::fTimeInclusive>);

	if (maxCount > 0 && maxCount < count)
		count = maxCount;
	_Dump(count);
}

void
Profiler::DumpTimeExclusivePerCall(uint32 maxCount)
{
	if (!fInitialized)
		return;

	SpinlockGuard guard(&fFunctionLock);
	uint32 count = _FunctionCount();

	if (count == 0) {
		kprintf("No profiling data available.\n");
		return;
	}

	qsort(fFunctionData, count, sizeof(FunctionData),
		&_CompareFunctionsPerCall<bigtime_t, &FunctionData::fTimeExclusive>);

	if (maxCount > 0 && maxCount < count)
		count = maxCount;
	_Dump(count);
}

/* static */ Profiler*
Profiler::Get()
{
	if (sProfiler != NULL)
		return sProfiler;

	// Double-checked locking pattern
	SpinlockGuard guard(&sInstanceLock);
	if (sProfiler == NULL) {
		sProfiler = new(std::nothrow) Profiler();
	}
	
	return sProfiler;
}

/* static */ void
Profiler::Initialize()
{
	Profiler* profiler = Get();
	if (profiler == NULL || profiler->GetStatus() != B_OK) {
		kprintf("Scheduler::Profiling::Profiler: could not initialize profiler\n");
		return;
	}

#if defined(__HAIKU__)
	add_debugger_command_etc("scheduler_profiler", &dump_profiler,
		"Show data collected by scheduler profiler",
		"[ <field> [ <count> ] ]\n"
		"Shows data collected by scheduler profiler\n"
		"  <field>   - Field used to sort functions. Available: called,"
			" time-inclusive, time-inclusive-per-call, time-exclusive,"
			" time-exclusive-per-call.\n"
		"              (defaults to \"called\")\n"
		"  <count>   - Maximum number of showed functions.\n", 0);
#endif
}

/* static */ void
Profiler::Shutdown()
{
	SpinlockGuard guard(&sInstanceLock);
	if (sProfiler != NULL) {
		sProfiler->Shutdown();
		delete sProfiler;
		sProfiler = NULL;
	}
}

uint32
Profiler::_FunctionCount() const
{
	uint32 count = 0;
	for (count = 0; count < kMaxFunctionEntries; count++) {
		if (fFunctionData[count].fFunction == NULL)
			break;
	}
	return count;
}

void
Profiler::_Dump(uint32 count)
{
	kprintf("Function calls (%" B_PRIu32 " functions):\n", count);
	kprintf("    called time-inclusive per-call time-exclusive per-call "
		"function\n");
	
	for (uint32 i = 0; i < count; i++) {
		FunctionData* function = &fFunctionData[i];
		if (function->fFunction == NULL || function->fCalled == 0)
			continue;
			
		bigtime_t inclusivePerCall = function->fTimeInclusive / function->fCalled;
		bigtime_t exclusivePerCall = function->fTimeExclusive / function->fCalled;
		
		kprintf("%10" B_PRIu32 " %14" B_PRId64 " %8" B_PRId64 " %14" B_PRId64
			" %8" B_PRId64 " %s\n", 
			function->fCalled,
			function->fTimeInclusive,
			inclusivePerCall,
			function->fTimeExclusive,
			exclusivePerCall, 
			function->fFunction);
	}
}

void
Profiler::_Reset()
{
	if (!fInitialized)
		return;

	SpinlockGuard guard(&fFunctionLock);
	
	// Reset function data
	if (fFunctionData != NULL) {
		memset(fFunctionData, 0, sizeof(FunctionData) * kMaxFunctionEntries);
	}
	fFunctionDataCount = 0;

	// Reset function stacks
	memset(fFunctionStackPointers, 0, sizeof(fFunctionStackPointers));
	
	int32 cpuCount = get_cpu_count();
	if (cpuCount > SMP_MAX_CPUS)
		cpuCount = SMP_MAX_CPUS;
		
	for (int32 i = 0; i < cpuCount; i++) {
		if (fFunctionStacks[i] != NULL) {
			memset(fFunctionStacks[i], 0, sizeof(FunctionEntry) * kMaxFunctionStackEntries);
			for (uint32 j = 0; j < kMaxFunctionStackEntries; j++) {
				fFunctionStacks[i][j].fValid = false;
			}
		}
	}
}

Profiler::FunctionData*
Profiler::_FindFunction(const char* function)
{
	if (function == NULL)
		return NULL;

	// First pass without lock for performance
	for (uint32 i = 0; i < kMaxFunctionEntries; i++) {
		if (fFunctionData[i].fFunction == NULL)
			break;
		if (strcmp(fFunctionData[i].fFunction, function) == 0)
			return &fFunctionData[i];
	}

	return NULL;
}

Profiler::FunctionData*
Profiler::_AllocateFunction(const char* function)
{
	if (function == NULL)
		return NULL;

	SpinlockGuard guard(&fFunctionLock);
	
	// Double-check pattern
	for (uint32 i = 0; i < kMaxFunctionEntries; i++) {
		if (fFunctionData[i].fFunction == NULL) {
			// Initialize new entry
			fFunctionData[i].fFunction = function;
			fFunctionData[i].fCalled = 0;
			fFunctionData[i].fTimeInclusive = 0;
			fFunctionData[i].fTimeExclusive = 0;
			fFunctionData[i].fMinTimeInclusive = 0;
			fFunctionData[i].fMaxTimeInclusive = 0;
			fFunctionData[i].fMinTimeExclusive = 0;
			fFunctionData[i].fMaxTimeExclusive = 0;
			fFunctionDataCount++;
			return &fFunctionData[i];
		}
		if (strcmp(fFunctionData[i].fFunction, function) == 0)
			return &fFunctionData[i];
	}

	return NULL; // Table full
}

bool
Profiler::_IsValidCpu(int32 cpu) const
{
	if (cpu < 0 || cpu >= SMP_MAX_CPUS)
		return false;
	return (cpu < get_cpu_count());
}

nanotime_t
Profiler::_GetCurrentTime() const
{
	return get_system_time_nsecs();
}

template<typename Type, Type Profiler::FunctionData::*Member>
/* static */ int
Profiler::_CompareFunctions(const void* _a, const void* _b)
{
	const FunctionData* a = static_cast<const FunctionData*>(_a);
	const FunctionData* b = static_cast<const FunctionData*>(_b);

	// Handle null functions
	if (a->fFunction == NULL && b->fFunction == NULL)
		return 0;
	if (a->fFunction == NULL)
		return 1;
	if (b->fFunction == NULL)
		return -1;

	Type valueA = a->*Member;
	Type valueB = b->*Member;

	if (valueB > valueA)
		return 1;
	if (valueB < valueA)
		return -1;
	return 0;
}

template<typename Type, Type Profiler::FunctionData::*Member>
/* static */ int
Profiler::_CompareFunctionsPerCall(const void* _a, const void* _b)
{
	const FunctionData* a = static_cast<const FunctionData*>(_a);
	const FunctionData* b = static_cast<const FunctionData*>(_b);

	// Handle null functions or zero calls
	if (a->fFunction == NULL && b->fFunction == NULL)
		return 0;
	if (a->fFunction == NULL || a->fCalled == 0)
		return 1;
	if (b->fFunction == NULL || b->fCalled == 0)
		return -1;

	Type valueA = (a->*Member) / a->fCalled;
	Type valueB = (b->*Member) / b->fCalled;

	if (valueB > valueA)
		return 1;
	if (valueB < valueA)
		return -1;
	return 0;
}

static int
dump_profiler(int argc, char** argv)
{
	Profiler* profiler = Profiler::Get();
	if (profiler == NULL || !profiler->IsInitialized()) {
		kprintf("Scheduler profiler not initialized.\n");
		return 0;
	}

	if (argc < 2) {
		profiler->DumpCalled(0);
		return 0;
	}

	uint32 count = 0;
	if (argc >= 3) {
#if defined(__HAIKU__)
		count = parse_expression(argv[2]);
#else
		count = atoi(argv[2]);
#endif
	}
	if ((int32)count < 0)
		count = 0;

	if (strcmp(argv[1], "called") == 0)
		profiler->DumpCalled(count);
	else if (strcmp(argv[1], "time-inclusive") == 0)
		profiler->DumpTimeInclusive(count);
	else if (strcmp(argv[1], "time-inclusive-per-call") == 0)
		profiler->DumpTimeInclusivePerCall(count);
	else if (strcmp(argv[1], "time-exclusive") == 0)
		profiler->DumpTimeExclusive(count);
	else if (strcmp(argv[1], "time-exclusive-per-call") == 0)
		profiler->DumpTimeExclusivePerCall(count);
	else {
#if defined(__HAIKU__)
		print_debugger_command_usage(argv[0]);
#else
		kprintf("Invalid field. Available: called, time-inclusive, "
			"time-inclusive-per-call, time-exclusive, time-exclusive-per-call\n");
#endif
	}

	return 0;
}

#endif	// SCHEDULER_PROFILING