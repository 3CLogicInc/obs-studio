/* Minimal pthread shim for building obs-qsv11 with MSVC without OBS's
 * w32-pthreads dep. Covers only what util/threading.h and obs-qsv11.c
 * reference: static-init mutex + lock/unlock (recursion unused). */
#pragma once
#ifdef _WIN32
#include <windows.h>

typedef SRWLOCK pthread_mutex_t;
typedef int pthread_mutexattr_t;

#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT
#define PTHREAD_MUTEX_RECURSIVE 1

static __inline int pthread_mutexattr_init(pthread_mutexattr_t *attr)
{
	*attr = 0;
	return 0;
}

static __inline int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type)
{
	*attr = type;
	return type == PTHREAD_MUTEX_RECURSIVE ? 38 /* ENOSYS: SRWLOCK is non-recursive */ : 0;
}

static __inline int pthread_mutexattr_destroy(pthread_mutexattr_t *attr)
{
	(void)attr;
	return 0;
}

static __inline int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr)
{
	(void)attr;
	InitializeSRWLock(m);
	return 0;
}

static __inline int pthread_mutex_lock(pthread_mutex_t *m)
{
	AcquireSRWLockExclusive(m);
	return 0;
}

static __inline int pthread_mutex_unlock(pthread_mutex_t *m)
{
	ReleaseSRWLockExclusive(m);
	return 0;
}

static __inline int pthread_mutex_destroy(pthread_mutex_t *m)
{
	(void)m;
	return 0;
}
#endif
