/*
	Direct port of the cross platform 'sync' library:  https://github.com/cubiclesoft/cross-platform-cpp
	This source file is under the MIT, LGPL, or version 3.01 of the PHP license, your choice.
	(C) 2014 CubicleSoft.  All rights reserved.
*/

/* $Id$ */

#ifndef PHP_SYNC_H
#define PHP_SYNC_H

extern zend_module_entry sync_module_entry;
#define phpext_sync_ptr &sync_module_entry

#define PHP_SYNC_VERSION   "1.1.0"

#ifdef PHP_WIN32
#	define PHP_SYNC_API __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
#	define PHP_SYNC_API __attribute__ ((visibility("default")))
#else
#	define PHP_SYNC_API
#endif

#ifdef ZTS
#include "TSRM.h"
#endif

#ifdef PHP_WIN32
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <limits.h>
#endif

#ifdef __MACH__
#include <mach/clock.h>
#include <mach/mach.h>
#include <mach/semaphore.h>
#endif

PHP_MINIT_FUNCTION(sync);
PHP_MSHUTDOWN_FUNCTION(sync);
PHP_MINFO_FUNCTION(sync);

typedef struct _sync_Mutex_object sync_Mutex_object;
typedef struct _sync_Semaphore_object sync_Semaphore_object;
typedef struct _sync_Event_object sync_Event_object;
typedef struct _sync_ReaderWriter_object sync_ReaderWriter_object;

#if defined(PHP_WIN32)
typedef DWORD sync_ThreadIDType;
#else
typedef pthread_t sync_ThreadIDType;
#endif

/* Mutex */
struct _sync_Mutex_object {
#if defined(PHP_WIN32)
	CRITICAL_SECTION MxWinCritSection;

	HANDLE MxWinMutex;
#else
	pthread_mutex_t MxPthreadCritSection;

	sem_t *MxSemMutex;
	int MxAllocated;
#endif

	volatile sync_ThreadIDType MxOwnerID;
	volatile unsigned int MxCount;

	zend_object std;
};

static inline sync_Mutex_object * php_sync_Mutex_object_fetch_object(zend_object * obj) {
	return (sync_Mutex_object *)((char *)obj - XtOffsetOf(sync_Mutex_object, std));
}

#define Z_SYNC_MUTEX_OBJ_P(zv) php_sync_Mutex_object_fetch_object(Z_OBJ_P(zv));

/* Semaphore */
struct _sync_Semaphore_object {
#if defined(PHP_WIN32)
	HANDLE MxWinSemaphore;
#else
	sem_t *MxSemSemaphore;
	int MxAllocated;
#endif

	int MxAutoUnlock;
	volatile unsigned int MxCount;

	zend_object std;
};

static inline sync_Semaphore_object * php_sync_Semaphore_object_fetch_object(zend_object * obj) {
	return (sync_Semaphore_object *)((char *)obj - XtOffsetOf(sync_Semaphore_object, std));
}

#define Z_SYNC_SEMAPHORE_OBJ_P(zv) php_sync_Semaphore_object_fetch_object(Z_OBJ_P(zv));

/* Event */
struct _sync_Event_object {
#if defined(PHP_WIN32)
	HANDLE MxWinWaitEvent;
#else
	sem_t *MxSemWaitMutex, *MxSemWaitEvent, *MxSemWaitCount, *MxSemWaitStatus;
	int MxAllocated;
	int MxManual;
#endif

	zend_object std;
};

static inline sync_Event_object * php_sync_Event_object_fetch_object(zend_object * obj) {
	return (sync_Event_object *)((char *)obj - XtOffsetOf(sync_Event_object, std));
}

#define Z_SYNC_EVENT_OBJ_P(zv) php_sync_Event_object_fetch_object(Z_OBJ_P(zv));


/* Reader-Writer */
struct _sync_ReaderWriter_object {
#if defined(PHP_WIN32)
	HANDLE MxWinRSemMutex, MxWinRSemaphore, MxWinRWaitEvent, MxWinWWaitMutex;
#else
	sem_t *MxSemRSemMutex, *MxSemRSemaphore, *MxSemRWaitEvent, *MxSemWWaitMutex;
	int MxAllocated;
#endif

	int MxAutoUnlock;
	volatile unsigned int MxReadLocks, MxWriteLock;

	zend_object std;
};

static inline sync_ReaderWriter_object * php_sync_ReaderWriter_object_fetch_object(zend_object * obj) {
	return (sync_ReaderWriter_object *)((char *)obj - XtOffsetOf(sync_ReaderWriter_object, std));
}

#define Z_SYNC_READERWRITER_OBJ_P(zv) php_sync_ReaderWriter_object_fetch_object(Z_OBJ_P(zv));

#endif	/* PHP_SYNC_H */


/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
