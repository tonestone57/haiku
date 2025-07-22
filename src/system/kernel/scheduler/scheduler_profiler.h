/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */
#ifndef KERNEL_SCHEDULER_PROFILER_H
#define KERNEL_SCHEDULER_PROFILER_H

#include <SupportDefs.h>
#include <OS.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations for C compatibility

#ifdef __cplusplus
}
#endif

// Architecture-independent CPU detection
#ifndef SMP_MAX_CPUS
	#if defined(__i386__) || defined(__x86_64__)
		#define SMP_MAX_CPUS 64
	#elif defined(__arm__) || defined(__aarch64__)
		#define SMP_MAX_CPUS 32
	#elif defined(__powerpc__) || defined(__powerpc64__)
		#define SMP_MAX_CPUS 32
	#elif defined(__riscv)
		#define SMP_MAX_CPUS 32
	#else
		#define SMP_MAX_CPUS 16  // Conservative default
	#endif
#endif

// Architecture-independent CPU ID retrieval
#ifdef __cplusplus
extern "C" {
#endif

inline int32 smp_get_current_cpu(void);

#ifdef __cplusplus
}
#endif

//#define SCHEDULER_PROFILING
#ifdef SCHEDULER_PROFILING

#define SCHEDULER_ENTER_FUNCTION()	\
	Scheduler::Profiling::Function schedulerProfiler(__PRETTY_FUNCTION__)

#define SCHEDULER_EXIT_FUNCTION()	\
	do { schedulerProfiler.Exit(); } while (0)

#ifdef __cplusplus

namespace Scheduler {

namespace Profiling {

// Architecture-independent spinlock implementation
struct ProfilerSpinlock {
	volatile int32 value;
	
	ProfilerSpinlock() : value(0) {}
};

// Atomic operations wrapper for cross-platform compatibility
class AtomicOps {
public:
	static inline int32 TestAndSet(volatile int32* value, int32 newValue, int32 testValue);
	static inline void Set(volatile int32* value, int32 newValue);
	static inline int32 Get(volatile int32* value);
};

// Spinlock operations
class SpinlockOps {
public:
	static inline void Acquire(ProfilerSpinlock* lock);
	static inline void Release(ProfilerSpinlock* lock);
	static inline bool TryAcquire(ProfilerSpinlock* lock);
};

// RAII spinlock guard
class SpinlockGuard {
public:
	explicit SpinlockGuard(ProfilerSpinlock* lock) : fLock(lock)
	{
		if (fLock != NULL)
			SpinlockOps::Acquire(fLock);
	}
	
	~SpinlockGuard()
	{
		if (fLock != NULL)
			SpinlockOps::Release(fLock);
	}

private:
	ProfilerSpinlock* fLock;
	
	// Non-copyable
	SpinlockGuard(const SpinlockGuard&);
	SpinlockGuard& operator=(const SpinlockGuard&);
};

class Profiler {
public:
							Profiler();
							~Profiler(); // Added destructor

			status_t		Initialize(); // Made non-static for proper initialization
			void			Shutdown(); // Added cleanup method

			void			EnterFunction(int32 cpu, const char* function);
			void			ExitFunction(int32 cpu, const char* function);

			void			DumpCalled(uint32 count);
			void			DumpTimeInclusive(uint32 count);
			void			DumpTimeExclusive(uint32 count);
			void			DumpTimeInclusivePerCall(uint32 count);
			void			DumpTimeExclusivePerCall(uint32 count);

			status_t		GetStatus() const	{ return fStatus; }
			bool			IsInitialized() const { return fInitialized; }

	static	Profiler*		Get();
	static	void			Initialize(); // Static wrapper
	static	void			Shutdown(); // Static wrapper

private:
	struct FunctionData {
			const char*		fFunction;

			uint32			fCalled;

			bigtime_t		fTimeInclusive;
			bigtime_t		fTimeExclusive;
			
			// Added for better tracking
			bigtime_t		fMinTimeInclusive;
			bigtime_t		fMaxTimeInclusive;
			bigtime_t		fMinTimeExclusive;
			bigtime_t		fMaxTimeExclusive;
	};

	struct FunctionEntry {
			FunctionData*	fFunction;

			nanotime_t		fEntryTime;
			nanotime_t		fOthersTime;
			nanotime_t		fProfilerTime;
			
			// Added safety check
			bool			fValid;
	};

			uint32			_FunctionCount() const;
			void			_Dump(uint32 count);
			void			_Reset(); // Added reset functionality

			FunctionData*	_FindFunction(const char* function);
			FunctionData*	_AllocateFunction(const char* function);

			bool			_IsValidCpu(int32 cpu) const;
			nanotime_t		_GetCurrentTime() const;

			template<typename Type, Type FunctionData::*Member>
	static	int				_CompareFunctions(const void* a, const void* b);

			template<typename Type, Type FunctionData::*Member>
	static	int				_CompareFunctionsPerCall(const void* a,
								const void* b);

			static const uint32	kMaxFunctionEntries = 1024;
			static const uint32	kMaxFunctionStackEntries = 64;

			FunctionEntry*	fFunctionStacks[SMP_MAX_CPUS];
			uint32			fFunctionStackPointers[SMP_MAX_CPUS];

			FunctionData*	fFunctionData;
			uint32			fFunctionDataCount;
			ProfilerSpinlock fFunctionLock;

			bool			fInitialized;
			status_t		fStatus;
			
	static	Profiler*		sProfiler;
	static	ProfilerSpinlock sInstanceLock;
};

class Function {
public:
	inline					Function(const char* functionName);
	inline					~Function();

	inline	void			Exit();

private:
			const char*		fFunctionName;
			int32			fCpu; // Store CPU to handle CPU migration
			bool			fExited; // Prevent double exit
};

// Inline implementations

inline
Function::Function(const char* functionName)
	:
	fFunctionName(functionName),
	fCpu(smp_get_current_cpu()),
	fExited(false)
{
	Profiler* profiler = Profiler::Get();
	if (profiler != NULL && profiler->IsInitialized()) {
		profiler->EnterFunction(fCpu, fFunctionName);
	} else {
		fFunctionName = NULL; // Mark as invalid
	}
}

inline
Function::~Function()
{
	if (!fExited && fFunctionName != NULL)
		Exit();
}

inline void
Function::Exit()
{
	if (!fExited && fFunctionName != NULL) {
		Profiler* profiler = Profiler::Get();
		if (profiler != NULL && profiler->IsInitialized()) {
			profiler->ExitFunction(fCpu, fFunctionName);
		}
		fExited = true;
		fFunctionName = NULL;
	}
}

// Atomic operations implementation
inline int32
AtomicOps::TestAndSet(volatile int32* value, int32 newValue, int32 testValue)
{
#if defined(__GNUC__)
	return __sync_val_compare_and_swap(value, testValue, newValue);
#elif defined(_MSC_VER)
	return _InterlockedCompareExchange((volatile long*)value, newValue, testValue);
#else
	// Fallback - not thread safe!
	int32 oldValue = *value;
	if (oldValue == testValue)
		*value = newValue;
	return oldValue;
#endif
}

inline void
AtomicOps::Set(volatile int32* value, int32 newValue)
{
#if defined(__GNUC__)
	__sync_lock_test_and_set(value, newValue);
#elif defined(_MSC_VER)
	_InterlockedExchange((volatile long*)value, newValue);
#else
	*value = newValue;
#endif
}

inline int32
AtomicOps::Get(volatile int32* value)
{
#if defined(__GNUC__)
	return __sync_fetch_and_add(value, 0);
#else
	return *value;
#endif
}

// Spinlock operations implementation
inline void
SpinlockOps::Acquire(ProfilerSpinlock* lock)
{
	if (lock == NULL)
		return;
		
	while (AtomicOps::TestAndSet(&lock->value, 1, 0) != 0) {
		// CPU-specific pause instructions for better performance
		#if defined(__i386__) || defined(__x86_64__)
			__asm__ __volatile__("pause" ::: "memory");
		#elif defined(__arm__) || defined(__aarch64__)
			__asm__ __volatile__("yield" ::: "memory");
		#elif defined(__powerpc__) || defined(__powerpc64__)
			__asm__ __volatile__("or 27,27,27" ::: "memory"); // PowerPC yield
		#else
			// Generic busy wait with small delay
			for (volatile int i = 0; i < 10; i++);
		#endif
	}
}

inline void
SpinlockOps::Release(ProfilerSpinlock* lock)
{
	if (lock != NULL)
		AtomicOps::Set(&lock->value, 0);
}

inline bool
SpinlockOps::TryAcquire(ProfilerSpinlock* lock)
{
	if (lock == NULL)
		return false;
	return AtomicOps::TestAndSet(&lock->value, 1, 0) == 0;
}

// Architecture-independent CPU ID function
inline int32
smp_get_current_cpu(void)
{
#if defined(__linux__)
	return sched_getcpu();
#elif defined(__HAIKU__)
	// Haiku-specific implementation would go here
	extern int32 smp_get_current_cpu(void);
	return smp_get_current_cpu();
#else
	// Fallback - always return 0 (single CPU assumption)
	return 0;
#endif
}

}	// namespace Profiling

}	// namespace Scheduler

#endif // __cplusplus

#else	// !SCHEDULER_PROFILING

#define SCHEDULER_ENTER_FUNCTION()	((void)0)
#define SCHEDULER_EXIT_FUNCTION()	((void)0)

#endif	// SCHEDULER_PROFILING

#endif	// KERNEL_SCHEDULER_PROFILER_H