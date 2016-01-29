
#ifndef __spthread_hpp__
#define __spthread_hpp__

#ifndef WIN32

/// pthread

#include <pthread.h>
#include <unistd.h>

typedef void * sp_thread_result_t;
typedef pthread_mutex_t sp_thread_mutex_t;
typedef pthread_cond_t  sp_thread_cond_t;
typedef pthread_t       sp_thread_t;
typedef pthread_attr_t  sp_thread_attr_t;

#define sp_thread_mutex_init(m,a)   pthread_mutex_init(m,a)
#define sp_thread_mutex_destroy(m)  pthread_mutex_destroy(m)
#define sp_thread_mutex_lock(m)     pthread_mutex_lock(m)
#define sp_thread_mutex_unlock(m)   pthread_mutex_unlock(m)

#define sp_thread_cond_init(c,a)    pthread_cond_init(c,a)
#define sp_thread_cond_destroy(c)   pthread_cond_destroy(c)
#define sp_thread_cond_wait(c,m)    pthread_cond_wait(c,m)
#define sp_thread_cond_signal(c)    pthread_cond_signal(c)

#define sp_thread_attr_init(a)        pthread_attr_init(a)
#define sp_thread_attr_setdetachstate pthread_attr_setdetachstate
#define SP_THREAD_CREATE_DETACHED     PTHREAD_CREATE_DETACHED

#define sp_thread_self    pthread_self
#define sp_thread_create  pthread_create

#define SP_THREAD_CALL
typedef sp_thread_result_t ( * sp_thread_func_t )( void * args );

#define sp_sleep(x) sleep(x)

#else ///////////////////////////////////////////////////////////////////////

// win32 thread

#include <winsock2.h>
#include <process.h>

typedef unsigned sp_thread_t;

typedef unsigned sp_thread_result_t;
#define SP_THREAD_CALL __stdcall
typedef sp_thread_result_t ( __stdcall * sp_thread_func_t )( void * args );

typedef HANDLE  sp_thread_mutex_t;
typedef HANDLE  sp_thread_cond_t;
typedef DWORD   sp_thread_attr_t;

#define SP_THREAD_CREATE_DETACHED 1
#define sp_sleep(x) Sleep(1000*x)

int sp_thread_mutex_init( sp_thread_mutex_t * mutex, void * attr )
{
	*mutex = CreateMutex( NULL, FALSE, NULL );
	return NULL == * mutex ? GetLastError() : 0;
}

int sp_thread_mutex_destroy( sp_thread_mutex_t * mutex )
{
	int ret = CloseHandle( *mutex );

	return 0 == ret ? GetLastError() : 0;
}

int sp_thread_mutex_lock( sp_thread_mutex_t * mutex )
{
	int ret = WaitForSingleObject( *mutex, INFINITE );
	return WAIT_OBJECT_0 == ret ? 0 : GetLastError();
}

int sp_thread_mutex_unlock( sp_thread_mutex_t * mutex )
{
	int ret = ReleaseMutex( *mutex );
	return 0 != ret ? 0 : GetLastError();
}

int sp_thread_cond_init( sp_thread_cond_t * cond, void * attr )
{
	*cond = CreateEvent( NULL, FALSE, FALSE, NULL );
	return NULL == *cond ? GetLastError() : 0;
}

int sp_thread_cond_destroy( sp_thread_cond_t * cond )
{
	int ret = CloseHandle( *cond );
	return 0 == ret ? GetLastError() : 0;
}

/*
Caller MUST be holding the mutex lock; the
lock is released and the caller is blocked waiting
on 'cond'. When 'cond' is signaled, the mutex
is re-acquired before returning to the caller.
*/
int sp_thread_cond_wait( sp_thread_cond_t * cond, sp_thread_mutex_t * mutex )
{
	int ret = 0;

	sp_thread_mutex_unlock( mutex );

	ret = WaitForSingleObject( *cond, INFINITE );

	sp_thread_mutex_lock( mutex );

	return WAIT_OBJECT_0 == ret ? 0 : GetLastError();
}

int sp_thread_cond_signal( sp_thread_cond_t * cond )
{
	int ret = SetEvent( *cond );
	return 0 == ret ? GetLastError() : 0;
}

sp_thread_t sp_thread_self()
{
	return GetCurrentThreadId();
}

int sp_thread_attr_init( sp_thread_attr_t * attr )
{
	*attr = 0;
	return 0;
}

int sp_thread_attr_setdetachstate( sp_thread_attr_t * attr, int detachstate )
{
	*attr |= detachstate;
	return 0;
}

int sp_thread_create( sp_thread_t * thread, sp_thread_attr_t * attr,
		sp_thread_func_t myfunc, void * args )
{
	// _beginthreadex returns 0 on an error
	HANDLE h = (HANDLE)_beginthreadex( NULL, 0, myfunc, args, 0, thread );
	return h > 0 ? 0 : GetLastError();
}

#endif

#endif

