/*
	Direct port of the cross platform 'sync' library:  https://github.com/cubiclesoft/cross-platform-cpp
	This source file is under the MIT, LGPL, or version 3.01 of the PHP license, your choice.
	(C) 2014 CubicleSoft.  All rights reserved.
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "zend_exceptions.h"
#include "ext/standard/info.h"
#if defined(PHP_WIN32) && PHP_MAJOR_VERSION == 5 && PHP_MINOR_VERSION <= 4
#include "win32/php_stdint.h"
#else
#include <stdint.h>
#endif
#include "php_sync.h"

/* {{{ sync_module_entry
 */
zend_module_entry sync_module_entry = {
	STANDARD_MODULE_HEADER,
	"sync",                     /* extension name */
	NULL,             			/* function list */
	PHP_MINIT(sync),            /* process startup */
	NULL,                       /* process shutdown */
	NULL,                       /* request startup */
	NULL,                       /* request shutdown */
	PHP_MINFO(sync),            /* extension info */
	PHP_SYNC_VERSION,           /* extension version */
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_SYNC
ZEND_GET_MODULE(sync)
#endif

#ifndef INFINITE
#	define INFINITE   0xFFFFFFFF
#endif

#ifdef __MACH__
typedef int clockid_t;
#define CLOCK_REALTIME 1

int clock_gettime(clockid_t clk_id, struct timespec *ts)
{
	clock_serv_t cclock;
	mach_timespec_t mts;
	host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
	clock_get_time(cclock, &mts);
	mach_port_deallocate(mach_task_self(), cclock);
	ts->tv_sec = mts.tv_sec;
	ts->tv_nsec = mts.tv_nsec;

	return 0;
}

int sem_timedwait(sem_t* sem, const struct timespec *abs_timeout) {
	int retval = 0;
	mach_timespec_t mts;

	if (abs_timeout->tv_sec >= 0 || abs_timeout->tv_nsec >= 0) {
		mts.tv_sec = abs_timeout->tv_sec;
		mts.tv_nsec = abs_timeout->tv_nsec;
	} else {
		// FIX: If we really wait forever, we cannot shut down VERMONT
		// this is mac os x specific and does not happen on linux
		// hence, we just add a small timeout instead of blocking
		// indefinately
		mts.tv_sec = 1;
		mts.tv_nsec = 0;
	}
	retval = semaphore_timedwait(*sem, mts);
	switch (retval) {
	case KERN_SUCCESS:
		return 0;
	case KERN_OPERATION_TIMED_OUT:
		errno = ETIMEDOUT;
		break;
	case KERN_ABORTED:
		errno = EINTR;
		break;
	default:
		errno = EINVAL;
		break;
	}
	return -1;
}
#endif

/* Define some generic functions used several places. */
#ifdef PHP_WIN32

/* Windows. */
sync_ThreadIDType sync_GetCurrentThreadID()
{
	return GetCurrentThreadId();
}

uint64_t sync_GetUnixMicrosecondTime()
{
	FILETIME TempTime;
	ULARGE_INTEGER TempTime2;
	uint64_t Result;

	GetSystemTimeAsFileTime(&TempTime);
	TempTime2.HighPart = TempTime.dwHighDateTime;
	TempTime2.LowPart = TempTime.dwLowDateTime;
	Result = TempTime2.QuadPart;

	Result = (Result / 10) - (uint64_t)11644473600000000ULL;

	return Result;
}

#else

/* POSIX pthreads. */
sync_ThreadIDType sync_GetCurrentThreadID()
{
	return pthread_self();
}

uint64_t sync_GetUnixMicrosecondTime()
{
	struct timeval TempTime;

	if (gettimeofday(&TempTime, NULL))  return 0;

	return (uint64_t)((uint64_t)TempTime.tv_sec * (uint64_t)1000000 + (uint64_t)TempTime.tv_usec);
}

/* Simplifies timer-based mutex/semaphore acquisition. */
int sync_WaitForSemaphore(sem_t *SemPtr, uint32_t Wait)
{
	if (SemPtr == SEM_FAILED)  return 0;

	if (Wait == INFINITE)
	{
		while (sem_wait(SemPtr) < 0)
		{
			if (errno != EINTR)  return 0;
		}
	}
	else if (Wait == 0)
	{
		while (sem_trywait(SemPtr) < 0)
		{
			if (errno != EINTR)  return 0;
		}
	}
	else
	{
		struct timespec TempTime;

		if (clock_gettime(CLOCK_REALTIME, &TempTime) == -1)  return 0;
		TempTime.tv_sec += Wait / 1000;
		TempTime.tv_nsec += (Wait % 1000) * 1000000;
		TempTime.tv_sec += TempTime.tv_nsec / 1000000000;
		TempTime.tv_nsec = TempTime.tv_nsec % 1000000000;

		while (sem_timedwait(SemPtr, &TempTime) < 0)
		{
			if (errno != EINTR)  return 0;
		}
	}

	return 1;
}

#endif


/* Mutex */
static zend_object_handlers sync_Mutex_object_handlers;

PHP_SYNC_API zend_class_entry *sync_Mutex_ce;

static void sync_Mutex_free_object(zend_object *object);

/* {{{ Initialize internal Mutex structure. */
zend_object * sync_Mutex_create_object(zend_class_entry *ce)
{
	sync_Mutex_object *obj;

	/* Create the object. */
	obj = ecalloc(1, sizeof(sync_Mutex_object) + zend_object_properties_size(ce));

	/* Initialize Zend. */
	zend_object_std_init(&obj->std, ce);
	object_properties_init(&obj->std, ce);

	/* set the object handlers */
	obj->std.handlers = &sync_Mutex_object_handlers;

	/* Initialize Mutex information. */
#ifdef PHP_WIN32
	InitializeCriticalSection(&obj->MxWinCritSection);
#else
	obj->MxSemMutex = SEM_FAILED;
	pthread_mutex_init(&obj->MxPthreadCritSection, NULL);
#endif

	return &obj->std;
}
/* }}} */

/* {{{ Unlocks a mutex. */
int sync_Mutex_unlock_internal(sync_Mutex_object *obj, int all)
{
#ifdef PHP_WIN32

	EnterCriticalSection(&obj->MxWinCritSection);

	/* Make sure the mutex exists and make sure it is owned by the calling thread. */
	if (obj->MxWinMutex == NULL || obj->MxOwnerID != sync_GetCurrentThreadID())
	{
		LeaveCriticalSection(&obj->MxWinCritSection);

		return 0;
	}

	if (all)  obj->MxCount = 1;
	obj->MxCount--;
	if (!obj->MxCount)
	{
		obj->MxOwnerID = 0;

		/* Release the mutex. */
		ReleaseMutex(obj->MxWinMutex);
	}

	LeaveCriticalSection(&obj->MxWinCritSection);

#else

	if (pthread_mutex_lock(&obj->MxPthreadCritSection) != 0)  return 0;

	/* Make sure the mutex exists and make sure it is owned by the calling thread. */
	if (obj->MxSemMutex == SEM_FAILED || obj->MxOwnerID != sync_GetCurrentThreadID())
	{
		pthread_mutex_unlock(&obj->MxPthreadCritSection);

		return 0;
	}

	if (all)  obj->MxCount = 1;
	obj->MxCount--;
	if (!obj->MxCount)
	{
		obj->MxOwnerID = 0;

		/* Release the mutex. */
		sem_post(obj->MxSemMutex);
	}

	pthread_mutex_unlock(&obj->MxPthreadCritSection);

#endif

	return 1;
}
/* }}} */

/* {{{ Free internal Mutex structure. */
void sync_Mutex_free_object(zend_object *object)
{
	sync_Mutex_object *obj = php_sync_Mutex_object_fetch_object(object);

	sync_Mutex_unlock_internal(obj, 1);

#ifdef PHP_WIN32
	if (obj->MxWinMutex != NULL)  CloseHandle(obj->MxWinMutex);
	DeleteCriticalSection(&obj->MxWinCritSection);
#else
	if (obj->MxSemMutex != SEM_FAILED)
	{
		if (obj->MxAllocated)  efree(obj->MxSemMutex);
		else  sem_close(obj->MxSemMutex);
	}

	pthread_mutex_destroy(&obj->MxPthreadCritSection);
#endif

	zend_object_std_dtor(&obj->std);
}
/* }}} */

/* {{{ proto void Sync_Mutex::__construct([string $name = null])
   Constructs a named or unnamed mutex object. */
PHP_METHOD(sync_Mutex, __construct)
{
	char *name = NULL;
	size_t name_len;
	sync_Mutex_object *obj = Z_SYNC_MUTEX_OBJ_P(getThis());
#ifdef PHP_WIN32
	SECURITY_ATTRIBUTES SecAttr;
#endif

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|s", &name, &name_len) == FAILURE)  return;

#ifdef PHP_WIN32

	SecAttr.nLength = sizeof(SecAttr);
	SecAttr.lpSecurityDescriptor = NULL;
	SecAttr.bInheritHandle = TRUE;

	obj->MxWinMutex = CreateMutexA(&SecAttr, FALSE, name);
	if (obj->MxWinMutex == NULL)
	{
		zend_throw_exception(zend_ce_exception, "Mutex could not be created", 0);
		return;
	}

#else

	if (name != NULL)
	{
		char *name2;

		spprintf(&name2, 0, "/Sync_Mutex_%s_0", name);
		obj->MxSemMutex = sem_open(name2, O_CREAT, 0666, 1);

		efree(name2);
	}
	else
	{
		obj->MxAllocated = 1;

		obj->MxSemMutex = (sem_t *)ecalloc(1, sizeof(sem_t));
		sem_init(obj->MxSemMutex, 0, 1);
	}

	if (obj->MxSemMutex == SEM_FAILED)
	{
		zend_throw_exception(zend_ce_exception, "Mutex could not be created", 0);
		return;
	}

#endif
}
/* }}} */

/* {{{ proto bool Sync_Mutex::lock([int $wait = -1])
   Locks a mutex object. */
PHP_METHOD(sync_Mutex, lock)
{
	zend_long wait = -1;
	sync_Mutex_object *obj = Z_SYNC_MUTEX_OBJ_P(getThis());
#ifdef PHP_WIN32
	DWORD Result;
#endif

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|l", &wait) == FAILURE)  return;

#ifdef PHP_WIN32

	EnterCriticalSection(&obj->MxWinCritSection);

	/* Check to see if this is already owned by the calling thread. */
	if (obj->MxOwnerID == sync_GetCurrentThreadID())
	{
		obj->MxCount++;
		LeaveCriticalSection(&obj->MxWinCritSection);

		RETURN_TRUE;
	}

	LeaveCriticalSection(&obj->MxWinCritSection);

	/* Acquire the mutex. */
	Result = WaitForSingleObject(obj->MxWinMutex, (DWORD)(wait > -1 ? wait : INFINITE));
	if (Result != WAIT_OBJECT_0)  RETURN_FALSE;

	EnterCriticalSection(&obj->MxWinCritSection);
	obj->MxOwnerID = sync_GetCurrentThreadID();
	obj->MxCount = 1;
	LeaveCriticalSection(&obj->MxWinCritSection);

#else

	if (pthread_mutex_lock(&obj->MxPthreadCritSection) != 0)
	{
		zend_throw_exception(zend_ce_exception, "Unable to acquire mutex critical section", 0);

		RETURN_FALSE;
	}

	/* Check to see if this mutex is already owned by the calling thread. */
	if (obj->MxOwnerID == sync_GetCurrentThreadID())
	{
		obj->MxCount++;
		pthread_mutex_unlock(&obj->MxPthreadCritSection);

		RETURN_TRUE;
	}

	pthread_mutex_unlock(&obj->MxPthreadCritSection);

	if (!sync_WaitForSemaphore(obj->MxSemMutex, (uint32_t)(wait > -1 ? wait : INFINITE)))  RETURN_FALSE;

	pthread_mutex_lock(&obj->MxPthreadCritSection);
	obj->MxOwnerID = sync_GetCurrentThreadID();
	obj->MxCount = 1;
	pthread_mutex_unlock(&obj->MxPthreadCritSection);

#endif

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool Sync_Mutex::unlock([bool $all = false])
   Unlocks a mutex object. */
PHP_METHOD(sync_Mutex, unlock)
{
	zend_long all = 0;
	sync_Mutex_object *obj = Z_SYNC_MUTEX_OBJ_P(getThis());

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|l", &all) == FAILURE)  return;

	if (!sync_Mutex_unlock_internal(obj, all))  RETURN_FALSE;

	RETURN_TRUE;
}
/* }}} */


ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_mutex___construct, 0, ZEND_RETURN_VALUE, 0)
	ZEND_ARG_INFO(0, name)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_mutex_lock, 0, ZEND_RETURN_VALUE, 0)
	ZEND_ARG_INFO(0, wait)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_mutex_unlock, 0, ZEND_RETURN_VALUE, 0)
	ZEND_ARG_INFO(0, all)
ZEND_END_ARG_INFO()

static const zend_function_entry sync_Mutex_methods[] = {
	PHP_ME(sync_Mutex, __construct, arginfo_sync_mutex___construct, ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)
	PHP_ME(sync_Mutex, lock, arginfo_sync_mutex_lock, ZEND_ACC_PUBLIC)
	PHP_ME(sync_Mutex, unlock, arginfo_sync_mutex_unlock, ZEND_ACC_PUBLIC)
	PHP_FE_END
};



/* Semaphore */
PHP_SYNC_API zend_class_entry *sync_Semaphore_ce;

static zend_object_handlers sync_Semaphore_object_handlers;

static void sync_Semaphore_free_object(zend_object *object);

/* {{{ Initialize internal Semaphore structure. */
zend_object * sync_Semaphore_create_object(zend_class_entry *ce)
{
	sync_Semaphore_object *obj;

	/* Create the object. */
	obj = ecalloc(1, sizeof(sync_Semaphore_object) + zend_object_properties_size(ce));

	/* Initialize Zend. */
	zend_object_std_init(&obj->std, ce);
	object_properties_init(&obj->std, ce);

	/* Set object handlers */
	obj->std.handlers = &sync_Semaphore_object_handlers;

	/* Initialize Semaphore information. */
#ifndef PHP_WIN32
	obj->MxSemSemaphore = SEM_FAILED;
#endif

	return &obj->std;
}
/* }}} */

/* {{{ Free internal Semaphore structure. */
void sync_Semaphore_free_object(zend_object *object)
{
	sync_Semaphore_object *obj = php_sync_Semaphore_object_fetch_object(object);

	if (obj->MxAutoUnlock)
	{
		while (obj->MxCount)
		{
#ifdef PHP_WIN32
			ReleaseSemaphore(obj->MxWinSemaphore, 1, NULL);
#else
			sem_post(obj->MxSemSemaphore);
#endif

			obj->MxCount--;
		}
	}

#ifdef PHP_WIN32
	if (obj->MxWinSemaphore != NULL)  CloseHandle(obj->MxWinSemaphore);
#else
	if (obj->MxSemSemaphore != SEM_FAILED)
	{
		if (obj->MxAllocated)  efree(obj->MxSemSemaphore);
		else  sem_close(obj->MxSemSemaphore);
	}
#endif

	zend_object_std_dtor(&obj->std);
}
/* }}} */

/* {{{ proto void Sync_Semaphore::__construct([string $name = null, [int $initialval = 1, [bool $autounlock = true]]])
   Constructs a named or unnamed semaphore object.  Don't set $autounlock to false unless you really know what you are doing. */
PHP_METHOD(sync_Semaphore, __construct)
{
	char *name = NULL;
	size_t name_len;
	zend_long initialval = 1;
	zend_long autounlock = 1;
	sync_Semaphore_object *obj = Z_SYNC_SEMAPHORE_OBJ_P(getThis());
#ifdef PHP_WIN32
	SECURITY_ATTRIBUTES SecAttr;
#endif

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|sll", &name, &name_len, &initialval, &autounlock) == FAILURE)  return;

	obj->MxAutoUnlock = (autounlock ? 1 : 0);

#ifdef PHP_WIN32

	SecAttr.nLength = sizeof(SecAttr);
	SecAttr.lpSecurityDescriptor = NULL;
	SecAttr.bInheritHandle = TRUE;

	// TODO test use of initialval and autounlock -- they are zend_long now instead of long

	obj->MxWinSemaphore = CreateSemaphoreA(&SecAttr, (LONG)initialval, (LONG)initialval, name);
	if (obj->MxWinSemaphore == NULL)
	{
		zend_throw_exception(zend_ce_exception, "Semaphore could not be created", 0);
		return;
	}

#else

	if (name != NULL)
	{
		char *name2;

		spprintf(&name2, 0, "/Sync_Semaphore_%s_0", name);
		obj->MxSemSemaphore = sem_open(name2, O_CREAT, 0666, (unsigned int)initialval);

		efree(name2);
	}
	else
	{
		obj->MxAllocated = 1;

		obj->MxSemSemaphore = (sem_t *)ecalloc(1, sizeof(sem_t));
		sem_init(obj->MxSemSemaphore, 0, (unsigned int)initialval);
	}

	if (obj->MxSemSemaphore == SEM_FAILED)
	{
		zend_throw_exception(zend_ce_exception, "Semaphore could not be created", 0);
		return;
	}

#endif
}
/* }}} */

/* {{{ proto bool Sync_Semaphore::lock([int $wait = -1])
   Locks a semaphore object. */
PHP_METHOD(sync_Semaphore, lock)
{
	zend_long wait = -1;
	sync_Semaphore_object *obj = Z_SYNC_SEMAPHORE_OBJ_P(getThis());
#ifdef PHP_WIN32
	DWORD Result;
#endif

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|l", &wait) == FAILURE)  return;

#ifdef PHP_WIN32

	Result = WaitForSingleObject(obj->MxWinSemaphore, (DWORD)(wait > -1 ? wait : INFINITE));
	if (Result != WAIT_OBJECT_0)  RETURN_FALSE;

#else

	if (!sync_WaitForSemaphore(obj->MxSemSemaphore, (uint32_t)(wait > -1 ? wait : INFINITE)))  RETURN_FALSE;

#endif

	if (obj->MxAutoUnlock)  obj->MxCount++;

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool Sync_Semaphore::unlock([int &$prevcount])
   Unlocks a semaphore object. */
PHP_METHOD(sync_Semaphore, unlock)
{
	zval *zprevcount = NULL;
	sync_Semaphore_object *obj = Z_SYNC_SEMAPHORE_OBJ_P(getThis());
	int count;
	int argc = ZEND_NUM_ARGS();
#ifdef PHP_WIN32
	LONG PrevCount;
#endif

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|z", &zprevcount) == FAILURE)  return;

#ifdef PHP_WIN32

	if (!ReleaseSemaphore(obj->MxWinSemaphore, 1, &PrevCount))  RETURN_FALSE;
	count = (int)PrevCount;

#else

	/* Get the current value first. */
	sem_getvalue(obj->MxSemSemaphore, &count);

	/* Release the semaphore. */
	sem_post(obj->MxSemSemaphore);

#endif

	if (argc > 0)
	{
		zval_dtor(zprevcount);
		ZVAL_LONG(zprevcount, count);
	}

	if (obj->MxAutoUnlock)  obj->MxCount--;

	RETURN_TRUE;
}
/* }}} */


ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_semaphore___construct, 0, ZEND_RETURN_VALUE, 0)
	ZEND_ARG_INFO(0, name)
	ZEND_ARG_INFO(0, initialval)
	ZEND_ARG_INFO(0, autounlock)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_semaphore_lock, 0, ZEND_RETURN_VALUE, 0)
	ZEND_ARG_INFO(0, wait)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_semaphore_unlock, 0, ZEND_RETURN_VALUE, 0)
	ZEND_ARG_INFO(1, prevcount)
ZEND_END_ARG_INFO()

static const zend_function_entry sync_Semaphore_methods[] = {
	PHP_ME(sync_Semaphore, __construct, arginfo_sync_semaphore___construct, ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)
	PHP_ME(sync_Semaphore, lock, arginfo_sync_semaphore_lock, ZEND_ACC_PUBLIC)
	PHP_ME(sync_Semaphore, unlock, arginfo_sync_semaphore_unlock, ZEND_ACC_PUBLIC)
	PHP_FE_END
};



/* Event */
PHP_SYNC_API zend_class_entry *sync_Event_ce;

static zend_object_handlers sync_Event_object_handlers;

static void sync_Event_free_object(zend_object *object);

/* {{{ Initialize internal Event structure. */
zend_object * sync_Event_create_object(zend_class_entry *ce)
{
	sync_Event_object *obj;

	/* Create the object. */
	obj = ecalloc(1, sizeof(sync_Event_object) + zend_object_properties_size(ce));

	/* Initialize Zend. */
	zend_object_std_init(&obj->std, ce);
	object_properties_init(&obj->std, ce);

	/* Set object handlers */
	obj->std.handlers = &sync_Event_object_handlers;

	/* Initialize Event information. */
#ifndef PHP_WIN32
	obj->MxSemWaitMutex = SEM_FAILED;
	obj->MxSemWaitEvent = SEM_FAILED;
	obj->MxSemWaitCount = SEM_FAILED;
	obj->MxSemWaitStatus = SEM_FAILED;
#endif

	return &obj->std;
}
/* }}} */

/* {{{ Free internal Event structure. */
static void sync_Event_free_object(zend_object *object)
{
	sync_Event_object *obj = php_sync_Event_object_fetch_object(object);

#ifdef PHP_WIN32
	if (obj->MxWinWaitEvent != NULL)  CloseHandle(obj->MxWinWaitEvent);
#else
	if (obj->MxAllocated)
	{
		if (obj->MxSemWaitStatus != SEM_FAILED)  efree(obj->MxSemWaitStatus);
		if (obj->MxSemWaitCount != SEM_FAILED)  efree(obj->MxSemWaitCount);
		if (obj->MxSemWaitEvent != SEM_FAILED)  efree(obj->MxSemWaitEvent);
		if (obj->MxSemWaitMutex != SEM_FAILED)  efree(obj->MxSemWaitMutex);
	}
	else
	{
		if (obj->MxSemWaitStatus != SEM_FAILED)  sem_close(obj->MxSemWaitStatus);
		if (obj->MxSemWaitCount != SEM_FAILED)  sem_close(obj->MxSemWaitCount);
		if (obj->MxSemWaitEvent != SEM_FAILED)  sem_close(obj->MxSemWaitEvent);
		if (obj->MxSemWaitMutex != SEM_FAILED)  sem_close(obj->MxSemWaitMutex);
	}
#endif

	zend_object_std_dtor(&obj->std);
}
/* }}} */

/* {{{ proto void Sync_Event::__construct([string $name = null, [bool $manual = false]])
   Constructs a named or unnamed event object. */
PHP_METHOD(sync_Event, __construct)
{
	char *name = NULL;
	int name_len;
	zend_long manual = 0;
	sync_Event_object *obj = Z_SYNC_EVENT_OBJ_P(getThis());
#ifdef PHP_WIN32
	SECURITY_ATTRIBUTES SecAttr;
#endif

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|sl", &name, &name_len, &manual) == FAILURE)  return;

#ifdef PHP_WIN32

	SecAttr.nLength = sizeof(SecAttr);
	SecAttr.lpSecurityDescriptor = NULL;
	SecAttr.bInheritHandle = TRUE;

	obj->MxWinWaitEvent = CreateEventA(&SecAttr, (BOOL)manual, FALSE, name);
	if (obj->MxWinWaitEvent == NULL)
	{
		zend_throw_exception(zend_ce_exception, "Event object could not be created", 0);
		return;
	}

#else

	obj->MxManual = (manual ? 1 : 0);

	if (name != NULL)
	{
		char *name2;
		int name2_len;

		name2_len = spprintf(&name2, 0, "/Sync_Event_%s_0", name);
		obj->MxSemWaitMutex = sem_open(name2, O_CREAT, 0666, 1);
        name2[name2_len-1] = '1'; /* "/Sync_Event_%s_1" */
		obj->MxSemWaitEvent = sem_open(name2, O_CREAT, 0666, 0);

		if (manual)
		{
			sprintf(name2, "/Sync_Event_%s_2", name);
			name2[name2_len-1] = '2'; /* "/Sync_Event_%s_2" */
			obj->MxSemWaitCount = sem_open(name2, O_CREAT, 0666, 0);

			name2[name2_len-1] = '3'; /* "/Sync_Event_%s_3" */
			obj->MxSemWaitStatus = sem_open(name2, O_CREAT, 0666, 0);
		}

		efree(name2);
	}
	else
	{
		obj->MxAllocated = 1;

		obj->MxSemWaitMutex = (sem_t *)ecalloc(1, sizeof(sem_t));
		sem_init(obj->MxSemWaitMutex, 0, 1);

		obj->MxSemWaitEvent = (sem_t *)ecalloc(1, sizeof(sem_t));
		sem_init(obj->MxSemWaitEvent, 0, 0);

		if (manual)
		{
			obj->MxSemWaitCount = (sem_t *)ecalloc(1, sizeof(sem_t));
			sem_init(obj->MxSemWaitCount, 0, 0);

			obj->MxSemWaitStatus = (sem_t *)ecalloc(1, sizeof(sem_t));
			sem_init(obj->MxSemWaitStatus, 0, 0);
		}
	}

	if (obj->MxSemWaitMutex == SEM_FAILED || obj->MxSemWaitEvent == SEM_FAILED || (manual && (obj->MxSemWaitCount == SEM_FAILED || obj->MxSemWaitStatus == SEM_FAILED)))
	{
		zend_throw_exception(zend_ce_exception, "Event object could not be created", 0);
		return;
	}

#endif
}
/* }}} */

/* {{{ proto bool Sync_Event::wait([int $wait = -1])
   Waits for an event object to fire. */
PHP_METHOD(sync_Event, wait)
{
	zend_long wait = -1;
	sync_Event_object *obj = Z_SYNC_EVENT_OBJ_P(getThis());
#ifdef PHP_WIN32
	DWORD Result;
#else
	uint32_t WaitAmt;
	uint64_t StartTime, CurrTime;
	int Val, Result;
#endif

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|l", &wait) == FAILURE)  return;

#ifdef PHP_WIN32

	Result = WaitForSingleObject(obj->MxWinWaitEvent, (DWORD)(wait > -1 ? wait : INFINITE));
	if (Result != WAIT_OBJECT_0)  RETURN_FALSE;

#else

	WaitAmt = (uint32_t)(wait > -1 ? wait : INFINITE);

	/* Get current time in milliseconds. */
	StartTime = (WaitAmt == INFINITE ? 0 : sync_GetUnixMicrosecondTime() / 1000000);

	if (!obj->MxManual)  CurrTime = StartTime;
	else
	{
		/* Lock the mutex. */
		if (!sync_WaitForSemaphore(obj->MxSemWaitMutex, WaitAmt))  RETURN_FALSE;

		/* Get the status.  If it is 1, then the event has been fired. */
		sem_getvalue(obj->MxSemWaitStatus, &Val);
		if (Val == 1)
		{
			sem_post(obj->MxSemWaitMutex);

			RETURN_TRUE;
		}

		/* Increment the wait count. */
		CurrTime = (WaitAmt == INFINITE ? 0 : sync_GetUnixMicrosecondTime() / 1000000);
		if (WaitAmt < CurrTime - StartTime)
		{
			sem_post(obj->MxSemWaitMutex);

			RETURN_FALSE;
		}
		sem_post(obj->MxSemWaitCount);

		/* Release the mutex. */
		sem_post(obj->MxSemWaitMutex);
	}

	/* Wait for the semaphore. */
	Result = sync_WaitForSemaphore(obj->MxSemWaitEvent, WaitAmt - (CurrTime - StartTime));

	if (obj->MxManual)
	{
		/* Lock the mutex. */
		sync_WaitForSemaphore(obj->MxSemWaitMutex, INFINITE);

		/* Decrease the wait count. */
		sync_WaitForSemaphore(obj->MxSemWaitCount, INFINITE);

		/* Release the mutex. */
		sem_post(obj->MxSemWaitMutex);
	}

	if (!Result)  RETURN_FALSE;

#endif

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool Sync_Event::fire()
   Lets a thread through that is waiting.  Lets multiple threads through that are waiting if the event object is 'manual'. */
PHP_METHOD(sync_Event, fire)
{
	sync_Event_object *obj = Z_SYNC_EVENT_OBJ_P(getThis());
#ifndef PHP_WIN32
	int x, Val;
#endif

#ifdef PHP_WIN32

	if (!SetEvent(obj->MxWinWaitEvent))  RETURN_FALSE;

#else

	if (obj->MxManual)
	{
		/* Lock the mutex. */
		if (!sync_WaitForSemaphore(obj->MxSemWaitMutex, INFINITE))  RETURN_FALSE;

		/* Update the status.  No wait. */
		sem_getvalue(obj->MxSemWaitStatus, &Val);
		if (Val == 0)  sem_post(obj->MxSemWaitStatus);

		/* Release the mutex. */
		sem_post(obj->MxSemWaitMutex);

		/* Release all waiting threads.  Might do too many sem_post() calls. */
		sem_getvalue(obj->MxSemWaitCount, &Val);
		for (x = 0; x < Val; x++)  sem_post(obj->MxSemWaitEvent);
	}
	else
	{
		/* Release one thread. */
		sem_getvalue(obj->MxSemWaitEvent, &Val);
		if (Val == 0)  sem_post(obj->MxSemWaitEvent);
	}

#endif

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool Sync_Event::reset()
   Resets the event object state.  Only use when the event object is 'manual'. */
PHP_METHOD(sync_Event, reset)
{
	sync_Event_object *obj = Z_SYNC_EVENT_OBJ_P(getThis());

#ifdef PHP_WIN32

	if (!ResetEvent(obj->MxWinWaitEvent))  RETURN_FALSE;

#else
	int Val;

	if (!obj->MxManual)  RETURN_FALSE;

	/* Lock the mutex. */
	if (!sync_WaitForSemaphore(obj->MxSemWaitMutex, INFINITE))  RETURN_FALSE;

	/* Restrict the semaphore.  Fixes the too many sem_post() calls in Fire(). */
	while (sync_WaitForSemaphore(obj->MxSemWaitEvent, 0))  {}

	/* Update the status.  Start waiting. */
	sem_getvalue(obj->MxSemWaitStatus, &Val);
	if (Val == 1)  sync_WaitForSemaphore(obj->MxSemWaitStatus, INFINITE);

	/* Release the mutex. */
	sem_post(obj->MxSemWaitMutex);

#endif

	RETURN_TRUE;
}
/* }}} */


ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_event___construct, 0, 0, 0)
	ZEND_ARG_INFO(0, name)
	ZEND_ARG_INFO(0, manual)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_event_wait, 0, 0, 0)
	ZEND_ARG_INFO(0, wait)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_event_fire, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_event_reset, 0, 0, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry sync_Event_methods[] = {
	PHP_ME(sync_Event, __construct, arginfo_sync_event___construct, ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)
	PHP_ME(sync_Event, wait, arginfo_sync_event_wait, ZEND_ACC_PUBLIC)
	PHP_ME(sync_Event, fire, arginfo_sync_event_fire, ZEND_ACC_PUBLIC)
	PHP_ME(sync_Event, reset, arginfo_sync_event_reset, ZEND_ACC_PUBLIC)
	PHP_FE_END
};



/* Reader-Writer */
PHP_SYNC_API zend_class_entry *sync_ReaderWriter_ce;

static zend_object_handlers sync_ReaderWriter_object_handlers;

static void sync_ReaderWriter_free_object(zend_object *object);

/* {{{ Initialize internal Reader-Writer structure. */
zend_object * sync_ReaderWriter_create_object(zend_class_entry *ce)
{
	sync_ReaderWriter_object *obj;

	/* Create the object. */
	obj = ecalloc(1, sizeof(sync_ReaderWriter_object) + zend_object_properties_size(ce));

	/* Initialize Zend. */
	zend_object_std_init(&obj->std, ce);
	object_properties_init(&obj->std, ce);

	/* Set the object handlers */
	obj->std.handlers = &sync_ReaderWriter_object_handlers;

	/* Initialize Reader-Writer information. */
#ifndef PHP_WIN32
	obj->MxSemRSemMutex = SEM_FAILED;
	obj->MxSemRSemaphore = SEM_FAILED;
	obj->MxSemRWaitEvent = SEM_FAILED;
	obj->MxSemWWaitMutex = SEM_FAILED;
#endif
	obj->MxAutoUnlock = 1;

	return &obj->std;
}
/* }}} */

/* {{{ Unlocks a read lock. */
int sync_ReaderWriter_readunlock_internal(sync_ReaderWriter_object *obj)
{
#ifdef PHP_WIN32

	DWORD Result;
	LONG Val;

	if (obj->MxWinRSemMutex == NULL || obj->MxWinRSemaphore == NULL || obj->MxWinRWaitEvent == NULL)  return 0;

	/* Acquire the semaphore mutex. */
	Result = WaitForSingleObject(obj->MxWinRSemMutex, INFINITE);
	if (Result != WAIT_OBJECT_0)  return 0;

	if (obj->MxReadLocks)  obj->MxReadLocks--;

	/* Release the semaphore. */
	if (!ReleaseSemaphore(obj->MxWinRSemaphore, 1, &Val))
	{
		ReleaseSemaphore(obj->MxWinRSemMutex, 1, NULL);

		return 0;
	}

	/* Update the event state. */
	if (Val == LONG_MAX - 1)
	{
		if (!SetEvent(obj->MxWinRWaitEvent))
		{
			ReleaseSemaphore(obj->MxWinRSemMutex, 1, NULL);

			return 0;
		}
	}

	/* Release the semaphore mutex. */
	ReleaseSemaphore(obj->MxWinRSemMutex, 1, NULL);

	return 1;

#else

	int Result, Val;

	if (obj->MxSemRSemMutex == NULL || obj->MxSemRSemaphore == NULL || obj->MxSemRWaitEvent == NULL)  return 0;

	/* Acquire the semaphore mutex. */
	if (!sync_WaitForSemaphore(obj->MxSemRSemMutex, INFINITE))  return 0;

	if (obj->MxReadLocks)  obj->MxReadLocks--;

	/* Release the semaphore. */
	Result = sem_post(obj->MxSemRSemaphore);
	if (Result != 0)
	{
		sem_post(obj->MxSemRSemMutex);

		return 0;
	}

	/* Update the event state. */
	sem_getvalue(obj->MxSemRSemaphore, &Val);
	if (Val == SEM_VALUE_MAX)
	{
		if (sem_post(obj->MxSemRWaitEvent) != 0)
		{
			sem_post(obj->MxSemRSemMutex);

			return 0;
		}
	}

	/* Release the semaphore mutex. */
	sem_post(obj->MxSemRSemMutex);

	return (Result == 0);

#endif
}
/* }}} */

/* {{{ Unlocks a write lock. */
int sync_ReaderWriter_writeunlock_internal(sync_ReaderWriter_object *obj)
{
#ifdef PHP_WIN32

	if (obj->MxWinWWaitMutex == NULL)  return 0;

	obj->MxWriteLock = 0;

	/* Release the write lock. */
	ReleaseSemaphore(obj->MxWinWWaitMutex, 1, NULL);

#else

	if (obj->MxSemWWaitMutex == NULL)  return 0;

	obj->MxWriteLock = 0;

	/* Release the write lock. */
	sem_post(obj->MxSemWWaitMutex);

#endif

	return 1;
}
/* }}} */

/* {{{ Free internal Reader-Writer structure. */
void sync_ReaderWriter_free_object(zend_object *object)
{
	sync_ReaderWriter_object *obj = php_sync_ReaderWriter_object_fetch_object(object);

	if (obj->MxAutoUnlock)
	{
		while (obj->MxReadLocks)  sync_ReaderWriter_readunlock_internal(obj);

		if (obj->MxWriteLock)  sync_ReaderWriter_writeunlock_internal(obj);
	}

#ifdef PHP_WIN32
	if (obj->MxWinWWaitMutex != NULL)  CloseHandle(obj->MxWinWWaitMutex);
	if (obj->MxWinRWaitEvent != NULL)  CloseHandle(obj->MxWinRWaitEvent);
	if (obj->MxWinRSemaphore != NULL)  CloseHandle(obj->MxWinRSemaphore);
	if (obj->MxWinRSemMutex != NULL)  CloseHandle(obj->MxWinRSemMutex);
#else
	if (obj->MxAllocated)
	{
		if (obj->MxSemWWaitMutex != SEM_FAILED)  efree(obj->MxSemWWaitMutex);
		if (obj->MxSemRWaitEvent != SEM_FAILED)  efree(obj->MxSemRWaitEvent);
		if (obj->MxSemRSemaphore != SEM_FAILED)  efree(obj->MxSemRSemaphore);
		if (obj->MxSemRSemMutex != SEM_FAILED)  efree(obj->MxSemRSemMutex);
	}
	else
	{
		if (obj->MxSemWWaitMutex != SEM_FAILED)  sem_close(obj->MxSemWWaitMutex);
		if (obj->MxSemRWaitEvent != SEM_FAILED)  sem_close(obj->MxSemRWaitEvent);
		if (obj->MxSemRSemaphore != SEM_FAILED)  sem_close(obj->MxSemRSemaphore);
		if (obj->MxSemRSemMutex != SEM_FAILED)  sem_close(obj->MxSemRSemMutex);
	}
#endif

	zend_object_std_dtor(&obj->std);
}
/* }}} */

/* {{{ proto void Sync_ReaderWriter::__construct([string $name = null, [bool $autounlock = true]])
   Constructs a named or unnamed reader-writer object.  Don't set $autounlock to false unless you really know what you are doing. */
PHP_METHOD(sync_ReaderWriter, __construct)
{
	char *name = NULL;
	int name_len;
	zend_long autounlock = 1;
	sync_ReaderWriter_object *obj = Z_SYNC_READERWRITER_OBJ_P(getThis());
	char *name2;
#ifdef PHP_WIN32
	SECURITY_ATTRIBUTES SecAttr;
#endif

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|sl", &name, &name_len, &autounlock) == FAILURE)  return;

	obj->MxAutoUnlock = (autounlock ? 1 : 0);

	if (name == NULL)  name2 = NULL;
	else  name2 = emalloc(name_len + 20);

#ifdef PHP_WIN32

	SecAttr.nLength = sizeof(SecAttr);
	SecAttr.lpSecurityDescriptor = NULL;
	SecAttr.bInheritHandle = TRUE;

	/* Create the mutexes, semaphore, and event objects. */
	if (name2 != NULL)  sprintf(name2, "Sync_ReadWrite|%s|0", name);
	obj->MxWinRSemMutex = CreateSemaphoreA(&SecAttr, 1, 1, name2);
	if (name2 != NULL)  sprintf(name2, "Sync_ReadWrite|%s|1", name);
	obj->MxWinRSemaphore = CreateSemaphoreA(&SecAttr, LONG_MAX, LONG_MAX, name2);
	if (name2 != NULL)  sprintf(name2, "Sync_ReadWrite|%s|2", name);
	obj->MxWinRWaitEvent = CreateEventA(&SecAttr, TRUE, TRUE, name2);
	if (name2 != NULL)  sprintf(name2, "Sync_ReadWrite|%s|3", name);
	obj->MxWinWWaitMutex = CreateSemaphoreA(&SecAttr, 1, 1, name2);

#else

	if (name2 != NULL)
	{
		sprintf(name2, "/Sync_ReadWrite_%s_0", name);
		obj->MxSemRSemMutex = sem_open(name2, O_CREAT, 0666, 1);
		sprintf(name2, "/Sync_ReadWrite_%s_1", name);
		obj->MxSemRSemaphore = sem_open(name2, O_CREAT, 0666, SEM_VALUE_MAX);
		sprintf(name2, "/Sync_ReadWrite_%s_2", name);
		obj->MxSemRWaitEvent = sem_open(name2, O_CREAT, 0666, 1);
		sprintf(name2, "/Sync_ReadWrite_%s_3", name);
		obj->MxSemWWaitMutex = sem_open(name2, O_CREAT, 0666, 1);
	}
	else
	{
		obj->MxAllocated = 1;

		obj->MxSemRSemMutex = (sem_t *)ecalloc(1, sizeof(sem_t));
		sem_init(obj->MxSemRSemMutex, 0, 1);

		obj->MxSemRSemaphore = (sem_t *)ecalloc(1, sizeof(sem_t));
		sem_init(obj->MxSemRSemaphore, 0, SEM_VALUE_MAX);

		obj->MxSemRWaitEvent = (sem_t *)ecalloc(1, sizeof(sem_t));
		sem_init(obj->MxSemRWaitEvent, 0, 1);

		obj->MxSemWWaitMutex = (sem_t *)ecalloc(1, sizeof(sem_t));
		sem_init(obj->MxSemWWaitMutex, 0, 1);
	}

#endif

	if (name2 != NULL)  efree(name2);

#ifdef PHP_WIN32

	if (obj->MxWinRSemMutex == NULL || obj->MxWinRSemaphore == NULL || obj->MxWinRWaitEvent == NULL || obj->MxWinWWaitMutex == NULL)
	{
		zend_throw_exception(zend_ce_exception, "Reader-Writer object could not be created", 0);
		return;
	}

#else

	if (obj->MxSemRSemMutex == SEM_FAILED || obj->MxSemRSemaphore == SEM_FAILED || obj->MxSemRWaitEvent == SEM_FAILED || obj->MxSemWWaitMutex == SEM_FAILED)
	{
		zend_throw_exception(zend_ce_exception, "Reader-Writer object could not be created", 0);
		return;
	}

#endif
}
/* }}} */

/* {{{ proto bool Sync_ReaderWriter::readlock([int $wait = -1])
   Read locks a reader-writer object. */
PHP_METHOD(sync_ReaderWriter, readlock)
{
	zend_long wait = -1;
	sync_ReaderWriter_object *obj = Z_SYNC_READERWRITER_OBJ_P(getThis());
	uint32_t WaitAmt;
	uint64_t StartTime, CurrTime;
#ifdef PHP_WIN32
	DWORD Result;
#else
	int Val;
#endif

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|l", &wait) == FAILURE)  return;

	WaitAmt = (uint32_t)(wait > -1 ? wait : INFINITE);

	/* Get current time in milliseconds. */
	StartTime = (WaitAmt == INFINITE ? 0 : sync_GetUnixMicrosecondTime() / 1000000);

#ifdef PHP_WIN32

	/* Acquire the write lock mutex.  Guarantees that readers can't starve the writer. */
	Result = WaitForSingleObject(obj->MxWinWWaitMutex, WaitAmt);
	if (Result != WAIT_OBJECT_0)  RETURN_FALSE;

	/* Acquire the semaphore mutex. */
	CurrTime = (WaitAmt == INFINITE ? 0 : sync_GetUnixMicrosecondTime() / 1000000);
	if (WaitAmt < CurrTime - StartTime)
	{
		ReleaseSemaphore(obj->MxWinWWaitMutex, 1, NULL);

		RETURN_FALSE;
	}
	Result = WaitForSingleObject(obj->MxWinRSemMutex, WaitAmt - (DWORD)(CurrTime - StartTime));
	if (Result != WAIT_OBJECT_0)
	{
		ReleaseSemaphore(obj->MxWinWWaitMutex, 1, NULL);

		RETURN_FALSE;
	}

	/* Acquire the semaphore. */
	CurrTime = (WaitAmt == INFINITE ? 0 : sync_GetUnixMicrosecondTime() / 1000000);
	if (WaitAmt < CurrTime - StartTime)
	{
		ReleaseSemaphore(obj->MxWinRSemMutex, 1, NULL);
		ReleaseSemaphore(obj->MxWinWWaitMutex, 1, NULL);

		RETURN_FALSE;
	}
	Result = WaitForSingleObject(obj->MxWinRSemaphore, WaitAmt - (DWORD)(CurrTime - StartTime));
	if (Result != WAIT_OBJECT_0)
	{
		ReleaseSemaphore(obj->MxWinRSemMutex, 1, NULL);
		ReleaseSemaphore(obj->MxWinWWaitMutex, 1, NULL);

		RETURN_FALSE;
	}

	/* Update the event state. */
	if (!ResetEvent(obj->MxWinRWaitEvent))
	{
		ReleaseSemaphore(obj->MxWinRSemaphore, 1, NULL);
		ReleaseSemaphore(obj->MxWinRSemMutex, 1, NULL);
		ReleaseSemaphore(obj->MxWinWWaitMutex, 1, NULL);

		RETURN_FALSE;
	}

	obj->MxReadLocks++;

	/* Release the mutexes. */
	ReleaseSemaphore(obj->MxWinRSemMutex, 1, NULL);
	ReleaseSemaphore(obj->MxWinWWaitMutex, 1, NULL);

#else

	/* Acquire the write lock mutex.  Guarantees that readers can't starve the writer. */
	if (!sync_WaitForSemaphore(obj->MxSemWWaitMutex, WaitAmt))  RETURN_FALSE;

	/* Acquire the semaphore mutex. */
	CurrTime = (WaitAmt == INFINITE ? 0 : sync_GetUnixMicrosecondTime() / 1000000);
	if (WaitAmt < CurrTime - StartTime)
	{
		sem_post(obj->MxSemWWaitMutex);

		RETURN_FALSE;
	}
	if (!sync_WaitForSemaphore(obj->MxSemRSemMutex, WaitAmt - (CurrTime - StartTime)))
	{
		sem_post(obj->MxSemWWaitMutex);

		RETURN_FALSE;
	}

	/* Acquire the semaphore. */
	CurrTime = (WaitAmt == INFINITE ? 0 : sync_GetUnixMicrosecondTime() / 1000000);
	if (WaitAmt < CurrTime - StartTime)
	{
		sem_post(obj->MxSemRSemMutex);
		sem_post(obj->MxSemWWaitMutex);

		RETURN_FALSE;
	}
	if (!sync_WaitForSemaphore(obj->MxSemRSemaphore, WaitAmt - (CurrTime - StartTime)))
	{
		sem_post(obj->MxSemRSemMutex);
		sem_post(obj->MxSemWWaitMutex);

		RETURN_FALSE;
	}

	/* Update the event state. */
	sem_getvalue(obj->MxSemRSemaphore, &Val);
	if (Val == SEM_VALUE_MAX - 1)
	{
		if (!sync_WaitForSemaphore(obj->MxSemRWaitEvent, INFINITE))
		{
			sem_post(obj->MxSemRSemaphore);
			sem_post(obj->MxSemRSemMutex);
			sem_post(obj->MxSemWWaitMutex);

			RETURN_FALSE;
		}
	}

	obj->MxReadLocks++;

	/* Release the mutexes. */
	sem_post(obj->MxSemRSemMutex);
	sem_post(obj->MxSemWWaitMutex);

#endif

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool Sync_ReaderWriter::writelock([int $wait = -1])
   Write locks a reader-writer object. */
PHP_METHOD(sync_ReaderWriter, writelock)
{
	zend_long wait = -1;
	sync_ReaderWriter_object *obj = Z_SYNC_READERWRITER_OBJ_P(getThis());
	uint32_t WaitAmt;
	uint64_t StartTime, CurrTime;
#ifdef PHP_WIN32
	DWORD Result;
#endif

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "|l", &wait) == FAILURE)  return;

	WaitAmt = (uint32_t)(wait > -1 ? wait : INFINITE);

	/* Get current time in milliseconds. */
	StartTime = (WaitAmt == INFINITE ? 0 : sync_GetUnixMicrosecondTime() / 1000000);

#ifdef PHP_WIN32

	/* Acquire the write lock mutex. */
	Result = WaitForSingleObject(obj->MxWinWWaitMutex, WaitAmt);
	if (Result != WAIT_OBJECT_0)  RETURN_FALSE;

	/* Wait for readers to reach zero. */
	CurrTime = (WaitAmt == INFINITE ? 0 : sync_GetUnixMicrosecondTime() / 1000000);
	Result = WaitForSingleObject(obj->MxWinRWaitEvent, WaitAmt - (DWORD)(CurrTime - StartTime));
	if (Result != WAIT_OBJECT_0)
	{
		ReleaseSemaphore(obj->MxWinWWaitMutex, 1, NULL);

		RETURN_FALSE;
	}

#else

	/* Acquire the write lock mutex. */
	if (!sync_WaitForSemaphore(obj->MxSemWWaitMutex, WaitAmt))  RETURN_FALSE;

	/* Wait for readers to reach zero. */
	CurrTime = (WaitAmt == INFINITE ? 0 : sync_GetUnixMicrosecondTime() / 1000000);
	if (!sync_WaitForSemaphore(obj->MxSemRWaitEvent, WaitAmt - (CurrTime - StartTime)))
	{
		sem_post(obj->MxSemWWaitMutex);

		RETURN_FALSE;
	}

	/* Release the semaphore to avoid a later deadlock. */
	sem_post(obj->MxSemRWaitEvent);

#endif

	obj->MxWriteLock = 1;

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool Sync_ReaderWriter::readunlock()
   Read unlocks a reader-writer object. */
PHP_METHOD(sync_ReaderWriter, readunlock)
{
	sync_ReaderWriter_object *obj = Z_SYNC_READERWRITER_OBJ_P(getThis());

	if (!sync_ReaderWriter_readunlock_internal(obj))  RETURN_FALSE;

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool Sync_ReaderWriter::writeunlock()
   Write unlocks a reader-writer object. */
PHP_METHOD(sync_ReaderWriter, writeunlock)
{
	sync_ReaderWriter_object *obj = Z_SYNC_READERWRITER_OBJ_P(getThis());

	if (!sync_ReaderWriter_writeunlock_internal(obj))  RETURN_FALSE;

	RETURN_TRUE;
}
/* }}} */


ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_readerwriter___construct, 0, ZEND_RETURN_VALUE, 0)
	ZEND_ARG_INFO(0, name)
	ZEND_ARG_INFO(0, autounlock)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_readerwriter_readlock, 0, ZEND_RETURN_VALUE, 0)
	ZEND_ARG_INFO(0, wait)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_sync_readerwriter_writelock, 0, ZEND_RETURN_VALUE, 0)
	ZEND_ARG_INFO(0, wait)
ZEND_END_ARG_INFO()

static const zend_function_entry sync_ReaderWriter_methods[] = {
	PHP_ME(sync_ReaderWriter, __construct, arginfo_sync_readerwriter___construct, ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)
	PHP_ME(sync_ReaderWriter, readlock, arginfo_sync_readerwriter_readlock, ZEND_ACC_PUBLIC)
	PHP_ME(sync_ReaderWriter, writelock, arginfo_sync_readerwriter_writelock, ZEND_ACC_PUBLIC)
	PHP_ME(sync_ReaderWriter, readunlock, NULL, ZEND_ACC_PUBLIC)
	PHP_ME(sync_ReaderWriter, writeunlock, NULL, ZEND_ACC_PUBLIC)
	PHP_FE_END
};



/* {{{ PHP_MINIT_FUNCTION(sync)
 */
PHP_MINIT_FUNCTION(sync)
{
	zend_class_entry ce;

	INIT_CLASS_ENTRY(ce, "SyncMutex", sync_Mutex_methods);
	ce.create_object = sync_Mutex_create_object;
	sync_Mutex_ce = zend_register_internal_class(&ce);
	memcpy(&sync_Mutex_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	sync_Mutex_object_handlers.offset = XtOffsetOf(sync_Mutex_object, std);
	sync_Mutex_object_handlers.free_obj = sync_Mutex_free_object;

	INIT_CLASS_ENTRY(ce, "SyncSemaphore", sync_Semaphore_methods);
	ce.create_object = sync_Semaphore_create_object;
	sync_Semaphore_ce = zend_register_internal_class(&ce);
	memcpy(&sync_Semaphore_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	sync_Semaphore_object_handlers.offset = XtOffsetOf(sync_Semaphore_object, std);
	sync_Semaphore_object_handlers.free_obj = sync_Semaphore_free_object;

	INIT_CLASS_ENTRY(ce, "SyncEvent", sync_Event_methods);
	ce.create_object = sync_Event_create_object;
	sync_Event_ce = zend_register_internal_class(&ce);
	memcpy(&sync_Event_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	sync_Event_object_handlers.offset = XtOffsetOf(sync_Event_object, std);
	sync_Event_object_handlers.free_obj = sync_Event_free_object;

	INIT_CLASS_ENTRY(ce, "SyncReaderWriter", sync_ReaderWriter_methods);
	ce.create_object = sync_ReaderWriter_create_object;
	sync_ReaderWriter_ce = zend_register_internal_class(&ce);
	memcpy(&sync_ReaderWriter_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	sync_ReaderWriter_object_handlers.offset = XtOffsetOf(sync_ReaderWriter_object, std);
	sync_ReaderWriter_object_handlers.free_obj = sync_ReaderWriter_free_object;

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(sync)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "sync support", "enabled");
	php_info_print_table_end();
}
/* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
