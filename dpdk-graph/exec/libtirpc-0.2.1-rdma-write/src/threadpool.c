/**
 * threadpool.c
 *
 * This file will contain your implementation of a threadpool.
 */

#include <stdio.h>
#include <stdlib.h>
//#include <unistd.h>
//#include <sp_thread.h>
#include <string.h>

#include <threadpool.h>
#include <spthread.h>

typedef struct _thread_st {
	sp_thread_t id;
	sp_thread_mutex_t mutex;
	sp_thread_cond_t cond;
	dispatch_fn fn;
	void *arg;
	threadpool parent;
} _thread;

// _threadpool is the internal threadpool structure that is
// cast to type "threadpool" before it given out to callers
typedef struct _threadpool_st {
	// you should fill in this structure with whatever you need
	sp_thread_mutex_t tp_mutex;
	sp_thread_cond_t tp_idle;
	sp_thread_cond_t tp_full;
	sp_thread_cond_t tp_empty;
	_thread ** tp_list;
	int tp_index;
	int tp_max_index;
	int tp_stop;

	int tp_total;
} _threadpool;

threadpool create_threadpool(int num_threads_in_pool)
{
	_threadpool *pool;

	// sanity check the argument
	if ((num_threads_in_pool <= 0) || (num_threads_in_pool > MAXT_IN_POOL))
		return NULL;

	pool = (_threadpool *) malloc(sizeof(_threadpool));
	if (pool == NULL) {
		fprintf(stderr, "Out of memory creating a new threadpool!\n");
		return NULL;
	}

	// add your code here to initialize the newly created threadpool
	sp_thread_mutex_init( &pool->tp_mutex, NULL );
	sp_thread_cond_init( &pool->tp_idle, NULL );
	sp_thread_cond_init( &pool->tp_full, NULL );
	sp_thread_cond_init( &pool->tp_empty, NULL );
	pool->tp_max_index = num_threads_in_pool;
	pool->tp_index = 0;
	pool->tp_stop = 0;
	pool->tp_total = 0;
	pool->tp_list = ( _thread ** )malloc( sizeof( void * ) * MAXT_IN_POOL );
	memset( pool->tp_list, 0, sizeof( void * ) * MAXT_IN_POOL );

	return (threadpool) pool;
}

int save_thread( _threadpool * pool, _thread * thread )
{
	int ret = -1;

	sp_thread_mutex_lock( &pool->tp_mutex );

	if( pool->tp_index < pool->tp_max_index ) {
		pool->tp_list[ pool->tp_index ] = thread;
		pool->tp_index++;
		ret = 0;

		sp_thread_cond_signal( &pool->tp_idle );

		if( pool->tp_index >= pool->tp_total ) {
			sp_thread_cond_signal( &pool->tp_full );
		}
	}

	sp_thread_mutex_unlock( &pool->tp_mutex );

	return ret;
}

sp_thread_result_t SP_THREAD_CALL wrapper_fn( void * arg )
{
	_thread * thread = (_thread*)arg;
	_threadpool * pool = (_threadpool*)thread->parent;

	for( ; 0 == ((_threadpool*)thread->parent)->tp_stop; ) {
		thread->fn( thread->arg );

		if( 0 != ((_threadpool*)thread->parent)->tp_stop ) break;

		sp_thread_mutex_lock( &thread->mutex );
		if( 0 == save_thread( thread->parent, thread ) ) {
		//	printf("---save_thread successfully~~\n");
			sp_thread_cond_wait( &thread->cond, &thread->mutex );
			sp_thread_mutex_unlock( &thread->mutex );
		} else {
			sp_thread_mutex_unlock( &thread->mutex );
			sp_thread_cond_destroy( &thread->cond );
			sp_thread_mutex_destroy( &thread->mutex );

			free( thread );
			break;
		}
	}

	sp_thread_mutex_lock( &pool->tp_mutex );
	pool->tp_total--;
	if( pool->tp_total <= 0 ) sp_thread_cond_signal( &pool->tp_empty );
	sp_thread_mutex_unlock( &pool->tp_mutex );

	return 0;
}

int dispatch_threadpool(threadpool from_me, dispatch_fn dispatch_to_here, void *arg)
{
	int ret = 0;

	_threadpool *pool = (_threadpool *) from_me;
	sp_thread_attr_t attr;
	_thread * thread = NULL;

	// add your code here to dispatch a thread
	sp_thread_mutex_lock( &pool->tp_mutex );
//	printf("==========dispatch_threadpool\n");
	while( pool->tp_index <= 0 && pool->tp_total >= pool->tp_max_index ) {
		//printf("all thread is busy!~~\n");
		sp_thread_cond_wait( &pool->tp_idle, &pool->tp_mutex );
	}

	if( pool->tp_index <= 0 ) {
		_thread * thread = ( _thread * )malloc( sizeof( _thread ) );
		memset( &( thread->id ), 0, sizeof( thread->id ) );
		sp_thread_mutex_init( &thread->mutex, NULL );
		sp_thread_cond_init( &thread->cond, NULL );
		thread->fn = dispatch_to_here;
		thread->arg = arg;
		thread->parent = pool;

		sp_thread_attr_init( &attr );
		sp_thread_attr_setdetachstate( &attr, SP_THREAD_CREATE_DETACHED );

		if( 0 == sp_thread_create( &thread->id, &attr, wrapper_fn, thread ) ) {
			pool->tp_total++;
		//	printf( "create thread#%ld\n", thread->id );
		} else {
			ret = -1;
			//printf( "cannot create thread\n" );
			sp_thread_mutex_destroy( &thread->mutex );
			sp_thread_cond_destroy( &thread->cond );
			free( thread );
		}
	} else {
		pool->tp_index--;
		thread = pool->tp_list[ pool->tp_index ];
		pool->tp_list[ pool->tp_index ] = NULL;

		thread->fn = dispatch_to_here;
		thread->arg = arg;
		thread->parent = pool;

		sp_thread_mutex_lock( &thread->mutex );
		sp_thread_cond_signal( &thread->cond ) ;
		sp_thread_mutex_unlock ( &thread->mutex );
	}

	sp_thread_mutex_unlock( &pool->tp_mutex );

	return ret;
}

void destroy_threadpool(threadpool destroyme)
{
	_threadpool *pool = (_threadpool *) destroyme;

	// add your code here to kill a threadpool
	int i = 0;

	sp_thread_mutex_lock( &pool->tp_mutex );

	if( pool->tp_index < pool->tp_total ) {
		//printf( "waiting for %d thread(s) to finish\n", pool->tp_total - pool->tp_index );
		sp_thread_cond_wait( &pool->tp_full, &pool->tp_mutex );
	}

	pool->tp_stop = 1;

	for( i = 0; i < pool->tp_index; i++ ) {
		_thread * thread = pool->tp_list[ i ];

		sp_thread_mutex_lock( &thread->mutex );
		sp_thread_cond_signal( &thread->cond ) ;
		sp_thread_mutex_unlock ( &thread->mutex );
	}

	if( pool->tp_total > 0 ) {
		//printf( "waiting for %d thread(s) to exit\n", pool->tp_total );
		sp_thread_cond_wait( &pool->tp_empty, &pool->tp_mutex );
	}

	for( i = 0; i < pool->tp_index; i++ ) {
		free( pool->tp_list[ i ] );
		pool->tp_list[ i ] = NULL;
	}

	sp_thread_mutex_unlock( &pool->tp_mutex );

	pool->tp_index = 0;

	sp_thread_mutex_destroy( &pool->tp_mutex );
	sp_thread_cond_destroy( &pool->tp_idle );
	sp_thread_cond_destroy( &pool->tp_full );
	sp_thread_cond_destroy( &pool->tp_empty );

	free( pool->tp_list );
	free( pool );
}

