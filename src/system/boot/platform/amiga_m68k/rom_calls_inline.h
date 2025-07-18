/*
 * Copyright 2023, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT license.
 *
 * Author:
 *		François Revol, revol@free.fr.
 */
#ifndef _AMICALLS_INLINE_H
#define _AMICALLS_INLINE_H

#define EXEC_BASE_NAME SysBase

#define LP(offs, rt, name, bn, ...)                                     \
static inline rt name(__VA_ARGS__)                                     \
{                                                                      \
	register rt _##name##_re __asm("d0");                              \
	void *const _##name##_bn = (bn);                                   \
	__asm volatile (                                                   \
		"move.l %%a6,%%sp@-\n\t"                                        \
		"move.l %[libbase],%%a6\n\t"                                    \
		"jsr %%a6@(-"#offs":W)\n\t"                                     \
		"move.l %%sp@+,%%a6"                                            \
	: "=r" (_##name##_re)                                              \
	: [libbase] "a" (_##name##_bn)                                     \
	: "d1", "a0", "a1", "fp0", "fp1", "cc", "memory");                  \
	return _##name##_re;                                               \
}

#define LP0(offs, rt, name, bt, bn)                                    \
static inline rt name()                                                \
{                                                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	register rt _##name##_re __asm("d0");                              \
	void *const _##name##_bn = (bn);                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_##name##_re), "=r" (_d1), "=r" (_a0), "=r" (_a1)            \
	: [libbase] "a" (_##name##_bn)                                     \
	: "fp0", "fp1", "cc", "memory");                                   \
	return _##name##_re;                                               \
}

static inline void ColdReboot()
{
	__asm volatile (
		"move.l %%a6,%%sp@-\n\t"
		"move.l %0,%%a6\n\t"
		"jsr %%a6@(-0x2d6:W)\n\t"
		"move.l %%sp@+,%%a6"
		:
		: "a" (EXEC_BASE_NAME)
		: "d0", "d1", "a0", "a1", "fp0", "fp1", "cc", "memory");
}

#define LP1(offs, rt, name, t1, v1, r1, bt, bn)                         \
static inline rt name(t1 v1)                                           \
{                                                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	register rt _##name##_re __asm("d0");                              \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_##name##_re), "=r" (_d1), "=r" (_a0), "=r" (_a1)            \
	: [libbase] "a" (_##name##_bn), "rf"(_n1)                           \
	: "fp0", "fp1", "cc", "memory");                                   \
	return _##name##_re;                                               \
}

#define LP1NR(offs, name, t1, v1, r1, bt, bn)                           \
static inline void name(t1 v1)                                         \
{                                                                      \
	register int _d0 __asm("d0");                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_d0), "=r" (_d1), "=r" (_a0), "=r" (_a1)                     \
	: [libbase] "a" (_##name##_bn), "rf"(_n1)                           \
	: "fp0", "fp1", "cc", "memory");                                   \
}

/* Only graphics.library/AttemptLockLayerRom() */
#define LP1A5(offs, rt, name, t1, v1, r1, bt, bn)                       \
static inline rt name(t1 v1)                                           \
{                                                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	register rt _##name##_re __asm("d0");                              \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	__asm volatile ("exg d7,a5\n\tmove.l %%a6,%%sp@-\n\t"                \
		"move.l %[libbase],%%a6\n\tjsr %%a6@(-"#offs":W)\n\t"            \
		"move.l %%sp@+,%%a6\n\texg d7,a5"                               \
	: "=r" (_##name##_re), "=r" (_d1), "=r" (_a0), "=r" (_a1)            \
	: [libbase] "a" (_##name##_bn), "rf"(_n1)                           \
	: "fp0", "fp1", "cc", "memory");                                   \
	return _##name##_re;                                               \
}

/* Only graphics.library/LockLayerRom() and graphics.library/UnlockLayerRom() */
#define LP1NRA5(offs, name, t1, v1, r1, bt, bn)                         \
static inline void name(t1 v1)                                         \
{                                                                      \
	register int _d0 __asm("d0");                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	__asm volatile ("exg d7,a5\n\tmove.l %%a6,%%sp@-\n\t"                \
		"move.l %[libbase],%%a6\n\tjsr %%a6@(-"#offs":W)\n\t"            \
		"move.l %%sp@+,%%a6\n\texg d7,a5"                               \
	: "=r" (_d0), "=r" (_d1), "=r" (_a0), "=r" (_a1)                     \
	: [libbase] "a" (_##name##_bn), "rf"(_n1)                           \
	: "fp0", "fp1", "cc", "memory");                                   \
}

/* Only exec.library/Supervisor() */
#define LP1A5FP(offs, rt, name, t1, v1, r1, bt, bn, fpt)                \
static inline rt name(t1 v1)                                           \
{                                                                      \
	typedef fpt;                                                       \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	register rt _##name##_re __asm("d0");                              \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	__asm volatile ("exg d7,a5\n\tmove.l %%a6,%%sp@-\n\t"                \
		"move.l %[libbase],%%a6\n\tjsr %%a6@(-"#offs":W)\n\t"            \
		"move.l %%sp@+,%%a6\n\texg d7,a5"                               \
	: "=r" (_##name##_re), "=r" (_d1), "=r" (_a0), "=r" (_a1)            \
	: [libbase] "a" (_##name##_bn), "rf"(_n1)                           \
	: "fp0", "fp1", "cc", "memory");                                   \
	return _##name##_re;                                               \
}

#define LP2(offs, rt, name, t1, v1, r1, t2, v2, r2, bt, bn)             \
static inline rt name(t1 v1, t2 v2)                                    \
{                                                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	register rt _##name##_re __asm("d0");                              \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_##name##_re), "=r" (_d1), "=r" (_a0), "=r" (_a1)            \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2)                \
	: "fp0", "fp1", "cc", "memory");                                   \
	return _##name##_re;                                               \
}

#define LP2NR(offs, name, t1, v1, r1, t2, v2, r2, bt, bn)               \
static inline void name(t1 v1, t2 v2)                                  \
{                                                                      \
	register int _d0 __asm("d0");                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_d0), "=r" (_d1), "=r" (_a0), "=r" (_a1)                     \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2)                \
	: "fp0", "fp1", "cc", "memory");                                   \
}

/* Only cia.resource/AbleICR() and cia.resource/SetICR() */
#define LP2UB(offs, rt, name, t1, v1, r1, t2, v2, r2)                   \
static inline rt name(t1 v1, t2 v2)                                    \
{                                                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	register rt _##name##_re __asm("d0");                              \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_##name##_re), "=r" (_d1), "=r" (_a0), "=r" (_a1)            \
	: "r"(_n1), "rf"(_n2)                                              \
	: "fp0", "fp1", "cc", "memory");                                   \
	return _##name##_re;                                               \
}

/* Only dos.library/InternalUnLoadSeg() */
#define LP2FP(offs, rt, name, t1, v1, r1, t2, v2, r2, bt, bn, fpt)      \
static inline rt name(t1 v1, t2 v2)                                    \
{                                                                      \
	typedef fpt;                                                       \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	register rt _##name##_re __asm("d0");                              \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_##name##_re), "=r" (_d1), "=r" (_a0), "=r" (_a1)            \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2)                \
	: "fp0", "fp1", "cc", "memory");                                   \
	return _##name##_re;                                               \
}

#define LP3(offs, rt, name, t1, v1, r1, t2, v2, r2, t3, v3, r3, bt, bn) \
static inline rt name(t1 v1, t2 v2, t3 v3)                             \
{                                                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	register rt _##name##_re __asm("d0");                              \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	register t3 _n3 __asm(#r3) = v3;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_##name##_re), "=r" (_d1), "=r" (_a0), "=r" (_a1)            \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2), "rf"(_n3)      \
	: "fp0", "fp1", "cc", "memory");                                   \
	return _##name##_re;                                               \
}

#define LP3NR(offs, name, t1, v1, r1, t2, v2, r2, t3, v3, r3, bt, bn)    \
static inline void name(t1 v1, t2 v2, t3 v3)                           \
{                                                                      \
	register int _d0 __asm("d0");                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	register t3 _n3 __asm(#r3) = v3;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_d0), "=r" (_d1), "=r" (_a0), "=r" (_a1)                     \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2), "rf"(_n3)      \
	: "fp0", "fp1", "cc", "memory");                                   \
}

/* Only cia.resource/AddICRVector() */
#define LP3UB(offs, rt, name, t1, v1, r1, t2, v2, r2, t3, v3, r3)       \
static inline rt name(t1 v1, t2 v2, t3 v3)                             \
{                                                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	register rt _##name##_re __asm("d0");                              \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	register t3 _n3 __asm(#r3) = v3;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_##name##_re), "=r" (_d1), "=r" (_a0), "=r" (_a1)            \
	: "r"(_n1), "rf"(_n2), "rf"(_n3)                                    \
	: "fp0", "fp1", "cc", "memory");                                   \
	return _##name##_re;                                               \
}

/* Only cia.resource/RemICRVector() */
#define LP3NRUB(offs, name, t1, v1, r1, t2, v2, r2, t3, v3, r3)         \
static inline void name(t1 v1, t2 v2, t3 v3)                           \
{                                                                      \
	register int _d0 __asm("d0");                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	register t3 _n3 __asm(#r3) = v3;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_d0), "=r" (_d1), "=r" (_a0), "=r" (_a1)                     \
	: "r"(_n1), "rf"(_n2), "rf"(_n3)                                    \
	: "fp0", "fp1", "cc", "memory");                                   \
}

/* Only exec.library/SetFunction() */
#define LP3FP(offs, rt, name, t1, v1, r1, t2, v2, r2, t3, v3, r3, bt,   \
	bn, fpt)                                                           \
static inline rt name(t1 v1, t2 v2, t3 v3)                             \
{                                                                      \
	typedef fpt;                                                       \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	register rt _##name##_re __asm("d0");                              \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	register t3 _n3 __asm(#r3) = v3;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_##name##_re), "=r" (_d1), "=r" (_a0), "=r" (_a1)            \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2), "rf"(_n3)      \
	: "fp0", "fp1", "cc", "memory");                                   \
	return _##name##_re;                                               \
}

/* Only graphics.library/SetCollision() */
#define LP3NRFP(offs, name, t1, v1, r1, t2, v2, r2, t3, v3, r3, bt,     \
	bn, fpt)                                                           \
static inline void name(t1 v1, t2 v2, t3 v3)                           \
{                                                                      \
	typedef fpt;                                                       \
	register int _d0 __asm("d0");                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	register t3 _n3 __asm(#r3) = v3;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_d0), "=r" (_d1), "=r" (_a0), "=r" (_a1)                     \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2), "rf"(_n3)      \
	: "fp0", "fp1", "cc", "memory");                                   \
}

#define LP4(offs, rt, name, t1, v1, r1, t2, v2, r2, t3, v3, r3, t4,     \
	v4, r4, bt, bn)                                                    \
static inline rt name(t1 v1, t2 v2, t3 v3, t4 v4)                      \
{                                                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	register rt _##name##_re __asm("d0");                              \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	register t3 _n3 __asm(#r3) = v3;                                   \
	register t4 _n4 __asm(#r4) = v4;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_##name##_re), "=r" (_d1), "=r" (_a0), "=r" (_a1)            \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2), "rf"(_n3),     \
		"rf"(_n4)                                                      \
	: "fp0", "fp1", "cc", "memory");                                   \
	return _##name##_re;                                               \
}

#define LP4NR(offs, name, t1, v1, r1, t2, v2, r2, t3, v3, r3, t4, v4,   \
	r4, bt, bn)                                                        \
static inline void name(t1 v1, t2 v2, t3 v3, t4 v4)                     \
{                                                                      \
	register int _d0 __asm("d0");                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	register t3 _n3 __asm(#r3) = v3;                                   \
	register t4 _n4 __asm(#r4) = v4;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_d0), "=r" (_d1), "=r" (_a0), "=r" (_a1)                     \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2), "rf"(_n3),     \
		"rf"(_n4)                                                      \
	: "fp0", "fp1", "cc", "memory");                                   \
}

/* Only exec.library/RawDoFmt() */
#define LP4FP(offs, rt, name, t1, v1, r1, t2, v2, r2, t3, v3, r3, t4,   \
	v4, r4, bt, bn, fpt)                                               \
static inline rt name(t1 v1, t2 v2, t3 v3, t4 v4)                      \
{                                                                      \
	typedef fpt;                                                       \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	register rt _##name##_re __asm("d0");                              \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	register t3 _n3 __asm(#r3) = v3;                                   \
	register t4 _n4 __asm(#r4) = v4;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_##name##_re), "=r" (_d1), "=r" (_a0), "=r" (_a1)            \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2), "rf"(_n3),     \
		"rf"(_n4)                                                      \
	: "fp0", "fp1", "cc", "memory");                                   \
	return _##name##_re;                                               \
}

#define LP5(offs, rt, name, t1, v1, r1, t2, v2, r2, t3, v3, r3, t4,     \
	v4, r4, t5, v5, r5, bt, bn)                                        \
static inline rt name(t1 v1, t2 v2, t3 v3, t4 v4, t5 v5)                \
{                                                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	register rt _##name##_re __asm("d0");                              \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	register t3 _n3 __asm(#r3) = v3;                                   \
	register t4 _n4 __asm(#r4) = v4;                                   \
	register t5 _n5 __asm(#r5) = v5;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_##name##_re), "=r" (_d1), "=r" (_a0), "=r" (_a1)            \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2), "rf"(_n3),     \
		"rf"(_n4), "rf"(_n5)                                           \
	: "fp0", "fp1", "cc", "memory");                                   \
	return _##name##_re;                                               \
}

#define LP5NR(offs, name, t1, v1, r1, t2, v2, r2, t3, v3, r3, t4, v4,   \
	r4, t5, v5, r5, bt, bn)                                            \
static inline void name(t1 v1, t2 v2, t3 v3, t4 v4, t5 v5)              \
{                                                                      \
	register int _d0 __asm("d0");                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	register t3 _n3 __asm(#r3) = v3;                                   \
	register t4 _n4 __asm(#r4) = v4;                                   \
	register t5 _n5 __asm(#r5) = v5;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_d0), "=r" (_d1), "=r" (_a0), "=r" (_a1)                     \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2), "rf"(_n3),     \
		"rf"(_n4), "rf"(_n5)                                           \
	: "fp0", "fp1", "cc", "memory");                                   \
}

/* Only exec.library/MakeLibrary() */
#define LP5FP(offs, rt, name, t1, v1, r1, t2, v2, r2, t3, v3, r3, t4,   \
	v4, r4, t5, v5, r5, bt, bn, fpt)                                   \
static inline rt name(t1 v1, t2 v2, t3 v3, t4 v4, t5 v5)                \
{                                                                      \
	typedef fpt;                                                       \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	register rt _##name##_re __asm("d0");                              \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	register t3 _n3 __asm(#r3) = v3;                                   \
	register t4 _n4 __asm(#r4) = v4;                                   \
	register t5 _n5 __asm(#r5) = v5;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_##name##_re), "=r" (_d1), "=r" (_a0), "=r" (_a1)            \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2), "rf"(_n3),     \
		"rf"(_n4), "rf"(_n5)                                           \
	: "fp0", "fp1", "cc", "memory");                                   \
	return _##name##_re;                                               \
}

/* Only reqtools.library/XXX() */
#define LP5A4(offs, rt, name, t1, v1, r1, t2, v2, r2, t3, v3, r3, t4,   \
	v4, r4, t5, v5, r5, bt, bn)                                        \
static inline rt name(t1 v1, t2 v2, t3 v3, t4 v4, t5 v5)                \
{                                                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	register rt _##name##_re __asm("d0");                              \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	register t3 _n3 __asm(#r3) = v3;                                   \
	register t4 _n4 __asm(#r4) = v4;                                   \
	register t5 _n5 __asm(#r5) = v5;                                   \
	__asm volatile ("exg d7,a4\n\tmove.l %%a6,%%sp@-\n\t"                \
		"move.l %[libbase],%%a6\n\tjsr %%a6@(-"#offs":W)\n\t"            \
		"move.l %%sp@+,%%a6\n\texg d7,a4"                               \
	: "=r" (_##name##_re), "=r" (_d1), "=r" (_a0), "=r" (_a1)            \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2), "rf"(_n3),     \
		"rf"(_n4), "rf"(_n5)                                           \
	: "fp0", "fp1", "cc", "memory");                                   \
	return _##name##_re;                                               \
}

#define LP6(offs, rt, name, t1, v1, r1, t2, v2, r2, t3, v3, r3, t4,     \
	v4, r4, t5, v5, r5, t6, v6, r6, bt, bn)                            \
static inline rt name(t1 v1, t2 v2, t3 v3, t4 v4, t5 v5, t6 v6)         \
{                                                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	register rt _##name##_re __asm("d0");                              \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	register t3 _n3 __asm(#r3) = v3;                                   \
	register t4 _n4 __asm(#r4) = v4;                                   \
	register t5 _n5 __asm(#r5) = v5;                                   \
	register t6 _n6 __asm(#r6) = v6;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_##name##_re), "=r" (_d1), "=r" (_a0), "=r" (_a1)            \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2), "rf"(_n3),     \
		"rf"(_n4), "rf"(_n5), "rf"(_n6)                                \
	: "fp0", "fp1", "cc", "memory");                                   \
	return _##name##_re;                                               \
}

#define LP6NR(offs, name, t1, v1, r1, t2, v2, r2, t3, v3, r3, t4, v4,   \
	r4, t5, v5, r5, t6, v6, r6, bt, bn)                                \
static inline void name(t1 v1, t2 v2, t3 v3, t4 v4, t5 v5, t6 v6)       \
{                                                                      \
	register int _d0 __asm("d0");                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	register t3 _n3 __asm(#r3) = v3;                                   \
	register t4 _n4 __asm(#r4) = v4;                                   \
	register t5 _n5 __asm(#r5) = v5;                                   \
	register t6 _n6 __asm(#r6) = v6;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_d0), "=r" (_d1), "=r" (_a0), "=r" (_a1)                     \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2), "rf"(_n3),     \
		"rf"(_n4), "rf"(_n5), "rf"(_n6)                                \
	: "fp0", "fp1", "cc", "memory");                                   \
}

#define LP7(offs, rt, name, t1, v1, r1, t2, v2, r2, t3, v3, r3, t4,     \
	v4, r4, t5, v5, r5, t6, v6, r6, t7, v7, r7, bt, bn)                \
static inline rt name(t1 v1, t2 v2, t3 v3, t4 v4, t5 v5, t6 v6, t7 v7)  \
{                                                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	register rt _##name##_re __asm("d0");                              \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	register t3 _n3 __asm(#r3) = v3;                                   \
	register t4 _n4 __asm(#r4) = v4;                                   \
	register t5 _n5 __asm(#r5) = v5;                                   \
	register t6 _n6 __asm(#r6) = v6;                                   \
	register t7 _n7 __asm(#r7) = v7;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_##name##_re), "=r" (_d1), "=r" (_a0), "=r" (_a1)            \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2), "rf"(_n3),     \
		"rf"(_n4), "rf"(_n5), "rf"(_n6), "rf"(_n7)                      \
	: "fp0", "fp1", "cc", "memory");                                   \
	return _##name##_re;                                               \
}

#define LP7NR(offs, name, t1, v1, r1, t2, v2, r2, t3, v3, r3, t4, v4,   \
	r4, t5, v5, r5, t6, v6, r6, t7, v7, r7, bt, bn)                    \
static inline void name(t1 v1, t2 v2, t3 v3, t4 v4, t5 v5, t6 v6,       \
	t7 v7)                                                             \
{                                                                      \
	register int _d0 __asm("d0");                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	register t3 _n3 __asm(#r3) = v3;                                   \
	register t4 _n4 __asm(#r4) = v4;                                   \
	register t5 _n5 __asm(#r5) = v5;                                   \
	register t6 _n6 __asm(#r6) = v6;                                   \
	register t7 _n7 __asm(#r7) = v7;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_d0), "=r" (_d1), "=r" (_a0), "=r" (_a1)                     \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2), "rf"(_n3),     \
		"rf"(_n4), "rf"(_n5), "rf"(_n6), "rf"(_n7)                      \
	: "fp0", "fp1", "cc", "memory");                                   \
}

/* Only workbench.library/AddAppIconA() */
#define LP7A4(offs, rt, name, t1, v1, r1, t2, v2, r2, t3, v3, r3, t4,   \
	v4, r4, t5, v5, r5, t6, v6, r6, t7, v7, r7, bt, bn)                \
static inline rt name(t1 v1, t2 v2, t3 v3, t4 v4, t5 v5, t6 v6, t7 v7)  \
{                                                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	register rt _##name##_re __asm("d0");                              \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	register t3 _n3 __asm(#r3) = v3;                                   \
	register t4 _n4 __asm(#r4) = v4;                                   \
	register t5 _n5 __asm(#r5) = v5;                                   \
	register t6 _n6 __asm(#r6) = v6;                                   \
	register t7 _n7 __asm(#r7) = v7;                                   \
	__asm volatile ("exg d7,a4\n\tmove.l %%a6,%%sp@-\n\t"                \
		"move.l %[libbase],%%a6\n\tjsr %%a6@(-"#offs":W)\n\t"            \
		"move.l %%sp@+,%%a6\n\texg d7,a4"                               \
	: "=r" (_##name##_re), "=r" (_d1), "=r" (_a0), "=r" (_a1)            \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2), "rf"(_n3),     \
		"rf"(_n4), "rf"(_n5), "rf"(_n6), "rf"(_n7)                      \
	: "fp0", "fp1", "cc", "memory");                                   \
	return _##name##_re;                                               \
}

/* Would you believe that there really are beasts that need more than 7
   arguments? :-) */

/* For example intuition.library/AutoRequest() */
#define LP8(offs, rt, name, t1, v1, r1, t2, v2, r2, t3, v3, r3, t4,     \
	v4, r4, t5, v5, r5, t6, v6, r6, t7, v7, r7, t8, v8, r8, bt, bn)    \
static inline rt name(t1 v1, t2 v2, t3 v3, t4 v4, t5 v5, t6 v6, t7 v7,  \
	t8 v8)                                                             \
{                                                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	register rt _##name##_re __asm("d0");                              \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	register t3 _n3 __asm(#r3) = v3;                                   \
	register t4 _n4 __asm(#r4) = v4;                                   \
	register t5 _n5 __asm(#r5) = v5;                                   \
	register t6 _n6 __asm(#r6) = v6;                                   \
	register t7 _n7 __asm(#r7) = v7;                                   \
	register t8 _n8 __asm(#r8) = v8;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_##name##_re), "=r" (_d1), "=r" (_a0), "=r" (_a1)            \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2), "rf"(_n3),     \
		"rf"(_n4), "rf"(_n5), "rf"(_n6), "rf"(_n7), "rf"(_n8)           \
	: "fp0", "fp1", "cc", "memory");                                   \
	return _##name##_re;                                               \
}

/* For example intuition.library/ModifyProp() */
#define LP8NR(offs, name, t1, v1, r1, t2, v2, r2, t3, v3, r3, t4, v4,   \
	r4, t5, v5, r5, t6, v6, r6, t7, v7, r7, t8, v8, r8, bt, bn)        \
static inline void name(t1 v1, t2 v2, t3 v3, t4 v4, t5 v5, t6 v6,       \
	t7 v7, t8 v8)                                                      \
{                                                                      \
	register int _d0 __asm("d0");                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	register t3 _n3 __asm(#r3) = v3;                                   \
	register t4 _n4 __asm(#r4) = v4;                                   \
	register t5 _n5 __asm(#r5) = v5;                                   \
	register t6 _n6 __asm(#r6) = v6;                                   \
	register t7 _n7 __asm(#r7) = v7;                                   \
	register t8 _n8 __asm(#r8) = v8;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_d0), "=r" (_d1), "=r" (_a0), "=r" (_a1)                     \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2), "rf"(_n3),     \
		"rf"(_n4), "rf"(_n5), "rf"(_n6), "rf"(_n7), "rf"(_n8)           \
	: "fp0", "fp1", "cc", "memory");                                   \
}

/* For example layers.library/CreateUpfrontHookLayer() */
#define LP9(offs, rt, name, t1, v1, r1, t2, v2, r2, t3, v3, r3, t4,     \
	v4, r4, t5, v5, r5, t6, v6, r6, t7, v7, r7, t8, v8, r8, t9, v9,    \
	r9, bt, bn)                                                        \
static inline rt name(t1 v1, t2 v2, t3 v3, t4 v4, t5 v5, t6 v6, t7 v7,  \
	t8 v8, t9 v9)                                                      \
{                                                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	register rt _##name##_re __asm("d0");                              \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	register t3 _n3 __asm(#r3) = v3;                                   \
	register t4 _n4 __asm(#r4) = v4;                                   \
	register t5 _n5 __asm(#r5) = v5;                                   \
	register t6 _n6 __asm(#r6) = v6;                                   \
	register t7 _n7 __asm(#r7) = v7;                                   \
	register t8 _n8 __asm(#r8) = v8;                                   \
	register t9 _n9 __asm(#r9) = v9;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_##name##_re), "=r" (_d1), "=r" (_a0), "=r" (_a1)            \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2), "rf"(_n3),     \
		"rf"(_n4), "rf"(_n5), "rf"(_n6), "rf"(_n7), "rf"(_n8), "rf"(_n9)\
	: "fp0", "fp1", "cc", "memory");                                   \
	return _##name##_re;                                               \
}

/* For example intuition.library/NewModifyProp() */
#define LP9NR(offs, name, t1, v1, r1, t2, v2, r2, t3, v3, r3, t4, v4,   \
	r4, t5, v5, r5, t6, v6, r6, t7, v7, r7, t8, v8, r8, t9, v9, r9,    \
	bt, bn)                                                            \
static inline void name(t1 v1, t2 v2, t3 v3, t4 v4, t5 v5, t6 v6,       \
	t7 v7, t8 v8, t9 v9)                                               \
{                                                                      \
	register int _d0 __asm("d0");                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	register t3 _n3 __asm(#r3) = v3;                                   \
	register t4 _n4 __asm(#r4) = v4;                                   \
	register t5 _n5 __asm(#r5) = v5;                                   \
	register t6 _n6 __asm(#r6) = v6;                                   \
	register t7 _n7 __asm(#r7) = v7;                                   \
	register t8 _n8 __asm(#r8) = v8;                                   \
	register t9 _n9 __asm(#r9) = v9;                                   \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_d0), "=r" (_d1), "=r" (_a0), "=r" (_a1)                     \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2), "rf"(_n3),     \
		"rf"(_n4), "rf"(_n5), "rf"(_n6), "rf"(_n7), "rf"(_n8),          \
		"rf"(_n9)                                                      \
	: "fp0", "fp1", "cc", "memory");                                   \
}

/* Kriton Kyrimis <kyrimis@cti.gr> says CyberGraphics needs the following */
#define LP10(offs, rt, name, t1, v1, r1, t2, v2, r2, t3, v3, r3, t4,    \
	v4, r4, t5, v5, r5, t6, v6, r6, t7, v7, r7, t8, v8, r8, t9, v9,    \
	r9, t10, v10, r10, bt, bn)                                         \
static inline rt name(t1 v1, t2 v2, t3 v3, t4 v4, t5 v5, t6 v6, t7 v7,  \
	t8 v8, t9 v9, t10 v10)                                             \
{                                                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	register rt _##name##_re __asm("d0");                              \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	register t3 _n3 __asm(#r3) = v3;                                   \
	register t4 _n4 __asm(#r4) = v4;                                   \
	register t5 _n5 __asm(#r5) = v5;                                   \
	register t6 _n6 __asm(#r6) = v6;                                   \
	register t7 _n7 __asm(#r7) = v7;                                   \
	register t8 _n8 __asm(#r8) = v8;                                   \
	register t9 _n9 __asm(#r9) = v9;                                   \
	register t10 _n10 __asm(#r10) = v10;                               \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_##name##_re), "=r" (_d1), "=r" (_a0), "=r" (_a1)            \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2), "rf"(_n3),     \
		"rf"(_n4), "rf"(_n5), "rf"(_n6), "rf"(_n7), "rf"(_n8),          \
		"rf"(_n9), "rf"(_n10)                                          \
	: "fp0", "fp1", "cc", "memory");                                   \
	return _##name##_re;                                               \
}

/* Only graphics.library/BltMaskBitMapRastPort() */
#define LP10NR(offs, name, t1, v1, r1, t2, v2, r2, t3, v3, r3, t4, v4,  \
	r4, t5, v5, r5, t6, v6, r6, t7, v7, r7, t8, v8, r8, t9, v9, r9,    \
	t10, v10, r10, bt, bn)                                             \
static inline void name(t1 v1, t2 v2, t3 v3, t4 v4, t5 v5, t6 v6,       \
	t7 v7, t8 v8, t9 v9, t10 v10)                                      \
{                                                                      \
	register int _d0 __asm("d0");                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	register t3 _n3 __asm(#r3) = v3;                                   \
	register t4 _n4 __asm(#r4) = v4;                                   \
	register t5 _n5 __asm(#r5) = v5;                                   \
	register t6 _n6 __asm(#r6) = v6;                                   \
	register t7 _n7 __asm(#r7) = v7;                                   \
	register t8 _n8 __asm(#r8) = v8;                                   \
	register t9 _n9 __asm(#r9) = v9;                                   \
	register t10 _n10 __asm(#r10) = v10;                               \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_d0), "=r" (_d1), "=r" (_a0), "=r" (_a1)                     \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2), "rf"(_n3),     \
		"rf"(_n4), "rf"(_n5), "rf"(_n6), "rf"(_n7), "rf"(_n8),          \
		"rf"(_n9), "rf"(_n10)                                          \
	: "fp0", "fp1", "cc", "memory");                                   \
}

/* Only graphics.library/BltBitMap() */
#define LP11(offs, rt, name, t1, v1, r1, t2, v2, r2, t3, v3, r3, t4,    \
	v4, r4, t5, v5, r5, t6, v6, r6, t7, v7, r7, t8, v8, r8, t9, v9,    \
	r9, t10, v10, r10, t11, v11, r11, bt, bn)                          \
static inline rt name(t1 v1, t2 v2, t3 v3, t4 v4, t5 v5, t6 v6, t7 v7,  \
	t8 v8, t9 v9, t10 v10, t11 v11)                                    \
{                                                                      \
	register int _d1 __asm("d1");                                      \
	register int _a0 __asm("a0");                                      \
	register int _a1 __asm("a1");                                      \
	register rt _##name##_re __asm("d0");                              \
	void *const _##name##_bn = (bn);                                   \
	register t1 _n1 __asm(#r1) = v1;                                   \
	register t2 _n2 __asm(#r2) = v2;                                   \
	register t3 _n3 __asm(#r3) = v3;                                   \
	register t4 _n4 __asm(#r4) = v4;                                   \
	register t5 _n5 __asm(#r5) = v5;                                   \
	register t6 _n6 __asm(#r6) = v6;                                   \
	register t7 _n7 __asm(#r7) = v7;                                   \
	register t8 _n8 __asm(#r8) = v8;                                   \
	register t9 _n9 __asm(#r9) = v9;                                   \
	register t10 _n10 __asm(#r10) = v10;                               \
	register t11 _n11 __asm(#r11) = v11;                               \
	__asm volatile ("move.l %%a6,%%sp@-\n\tmove.l %[libbase],%%a6\n\t"   \
		"jsr %%a6@(-"#offs":W)\n\tmove.l %%sp@+,%%a6"                   \
	: "=r" (_##name##_re), "=r" (_d1), "=r" (_a0), "=r" (_a1)            \
	: [libbase] "a" (_##name##_bn), "rf"(_n1), "rf"(_n2), "rf"(_n3),     \
		"rf"(_n4), "rf"(_n5), "rf"(_n6), "rf"(_n7), "rf"(_n8),          \
		"rf"(_n9), "rf"(_n10), "rf"(_n11)                               \
	: "fp0", "fp1", "cc", "memory");                                   \
	return _##name##_re;                                               \
}

#define ColdReboot() \
	LP0NR(0x2d6, ColdReboot, \
	, EXEC_BASE_NAME)


#endif /* _AMICALLS_INLINE_H */
