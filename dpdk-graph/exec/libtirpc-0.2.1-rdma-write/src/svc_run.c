/*
 * Copyright (c) 2009, Sun Microsystems, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of Sun Microsystems, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived
 *   from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * This is the rpc server side idle loop
 * Wait for input, call server program.
 */
#include <pthread.h>
#include <reentrant.h>
#include <err.h>
#include <errno.h>
#include <rpc/rpc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <rpc/rpc.h>
#include "rpc_com.h"
#include <sys/select.h>

/* MODIFIED by dwyane @ 2010-12-21 */
#include <rpc/list.h>
#include <rpc/svc.h>
#include <sys/epoll.h>
#include "threadpool.h"

/* END */

/* MODIFIED by dwyane @ 2011-01-12 */
#include "dwyane.h"
/* END */
#define RPC_THREAD_NUM 8

threadpool tp;

void
svc_run_rdma()
{
#if 0
	svc_run_epoll();
#else
	/* MODIFIED by dwyane @ 2010-12-21 */
	extern rwlock_t pending_lock;

	tp = create_threadpool(RPC_THREAD_NUM);
	printf("%s:create_threadpool\n",__func__);
	///*
/*	pthread_t run_for_ip;
	if(pthread_create(&run_for_ip, NULL, svc_run_epoll, NULL)){
		PRINTF_ERR("thread create err in %s\n", __func__);
		return;
	}*
	//*/

	INIT_LIST_HEAD(&pending_xprt);

	//what the mean of these code?? ming 2011-9-20
	while(1){
		SVCXPRT *xprt_pending;

		rwlock_wrlock(&pending_lock);

		if(list_empty(&pending_xprt)){
			rwlock_unlock(&pending_lock);
			continue;
		}
		else{
			xprt_pending = list_entry(pending_xprt.next, SVCXPRT, pending_flag);
			PRINTF_INFO("process with a xprt\n");
			list_del(&xprt_pending->pending_flag);
			//xprt_pending->busy = 0;
			rwlock_unlock(&pending_lock);
			
			//do_sth_with_xprt(xprt_pending);
			dispatch_threadpool(tp, do_sth_with_xprt, xprt_pending);
			//rwlock_unlock(&pending_lock);
		}
	}
	
	/* END */
#endif
}

void
svc_run()
{
	fd_set readfds, cleanfds;
	struct timeval timeout;
	extern rwlock_t svc_fd_lock;


	for (;;) {
		rwlock_rdlock(&svc_fd_lock);
		readfds = svc_fdset;
		cleanfds = svc_fdset;
		rwlock_unlock(&svc_fd_lock);
		timeout.tv_sec = 30;
		timeout.tv_usec = 0;
		switch (select(svc_maxfd+1, &readfds, NULL, NULL, &timeout)) {
		case -1:
			FD_ZERO(&readfds);
			if (errno == EINTR) {
				continue;
			}
			warn("svc_run: - select failed");
			return;
		case 0:
			__svc_clean_idle(&cleanfds, 30, FALSE);
			continue;
		default:
			svc_getreqset(&readfds);
		}
	}
}


/*
 *      This function causes svc_run() to exit by telling it that it has no
 *      more work to do.
 */
void
svc_exit()
{
	extern rwlock_t svc_fd_lock;

	rwlock_wrlock(&svc_fd_lock);
	FD_ZERO(&svc_fdset);
	rwlock_unlock(&svc_fd_lock);
}

void *
svc_run_epoll(void *arg){
	int i, sockfd, nfds;
	struct epoll_event events[20];



	for ( ; ; ) {

		nfds=epoll_wait(epfd, events, 20, 500);

        	for(i = 0; i < nfds; ++i)
        	{
            		if(events[i].events & EPOLLIN)
            		{
				//PRINTF_INFO("reading!\n");                
                    		if ( (sockfd = events[i].data.fd) < 0) 
					continue;
				svc_getreq_common_cap(sockfd);	

             		 }	
               }
                             
	}
         
}
