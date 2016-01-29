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

#include <sys/cdefs.h>

/*
 * svc_vc.c, Server side for Connection Oriented based RPC. 
 *
 * Actually implements two flavors of transporter -
 * a tcp rendezvouser (a listner and connection establisher)
 * and a record/tcp stream.
 */
#include <pthread.h>
#include <reentrant.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/poll.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rpc/rpc.h>

#include "rpc_com.h"

#include <getpeereid.h>

/* MODIFIED by dwyane @ 2010-12-21 */

#include <rpc/list.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <infiniband/arch.h>
#include <rdma/rdma_cma.h>
#include <rpc/rpc_rdma.h>
/* END */

/* MODIFIED by dwyane @ 2011-01-12 */
#include "dwyane.h"
/* END */

/* MODIFIED by dwyane @ 2010-12-26 */
#define	SVC_RDMA_SUCCESS 0
#define	SVC_RDMA_FAIL -1
uint32_t rdma_bufs_granted = RDMA_BUFS_GRANT;
/* END */
struct rdma_conn {  /* kept in xprt->xp_p1 for actual connection */
	enum xprt_stat strm_stat;
	u_int32_t x_id;
	XDR xdrs;
	char verf_body[MAX_AUTH_BYTES];
	u_int sendsize;
	u_int recvsize;
	int maxrec;
	bool_t nonblock;
	struct timeval last_recv_time;
};

/* MODIFIED by dwyane @ 2010-12-23 */
struct clone_rdma_data {
	bool_t		cloned;		/* xprt cloned for thread processing */
	CONN		*conn;		/* RDMA connection */
	rdma_buf_t	rpcbuf;		/* RPC req/resp buffer */
	struct clist	*cl_reply;	/* reply chunk buffer info */
	struct clist	*cl_wlist;		/* write list clist */
};


typedef struct __rdma_rpc_svcxprt{
	SVCXPRT 				xprt;
	struct rdma_cm_id 		*cm_id;
	struct ibv_pd 			*pd;
	struct ibv_comp_channel 	*comp_chan;
	bool_t 					type;
	dw_bufpool_t				*send_pool;
	dw_bufpool_t				*recv_pool;
        //dw_bufpool_t                            *longrpc_pool;
	CONN					*conn;
	/* MODIFIED by dwyane @ 2011-01-11 */
	XDR					dw_outxdr;	/* xdr stream for output */
	u_int32_t				dw_outsz;
	XDR					dw_inxdr;	/* xdr stream for input */
	u_int32_t				dw_xid;		/* current XID */
	/* END */
	/* MODIFIED by dwyane @ 2011-01-12 */
	rwlock_t				close_lock;
	xdrproc_t xdr_results;
	caddr_t   xdr_location;
	/* END */
}RDMA_SVCXPRT;
/* END */

/* MODIFIED by dwyane @ 2010-12-21 */
int prepare_rdma_xprt(RDMA_SVCXPRT* rxprt);
void* pending_handler(void *arg);
void* event_handler(void *arg);
void fill_xprt_rdma(SVCXPRT *xprt);
void svc_rdma_ops(SVCXPRT *xprt);
void svc_rdma_listen_ops(SVCXPRT *xprt);
int svc_setup_rlist(CONN *conn, XDR *xdrs, XDR *call_xdrp, int *flag);

static bool_t svc_rdma_recv(SVCXPRT *, struct rpc_msg *);
static bool_t svc_rdma_accept(SVCXPRT *, struct rpc_msg *);
static bool_t svc_rdma_getargs(SVCXPRT *, xdrproc_t, void *);
static bool_t svc_rdma_freeargs(SVCXPRT *, xdrproc_t, caddr_t);
static bool_t svc_rdma_freeres(RDMA_SVCXPRT *, xdrproc_t, caddr_t);
static bool_t svc_rdma_reply(SVCXPRT *, struct rpc_msg *);
static bool_t svc_rdma_control(SVCXPRT *xprt, const u_int32_t rq, void *in);
static void svc_rdma_destroy(SVCXPRT *);
static enum xprt_stat svc_rdma_stat(SVCXPRT *);
static int	svc_compose_rpcmsg(SVCXPRT *, CONN *, xdrproc_t,
			caddr_t, rdma_buf_t *, XDR **, struct rpc_msg *,
			bool_t, u_int32_t *,struct clist *wcl);

struct build_dw{
	struct ibv_pd 			*pd;
	struct ibv_comp_channel 	*comp_chan;
};
/* END */

/* MODIFIED by dwyane @ 2010-12-21 */
SVCXPRT *
svc_rdma_create()
{
	RDMA_SVCXPRT 	*rxprt;
	PRINTF_INFO("%s\n", __func__);

	rxprt = mem_alloc(sizeof(RDMA_SVCXPRT));

	if(prepare_rdma_xprt(rxprt)){
		PRINTF_ERR("err while prepare_rdma_xprt\n");
		mem_free(rxprt, sizeof(RDMA_SVCXPRT));
		return NULL;
	}

	rxprt->recv_pool = dw_rbufpool_create(rxprt->pd, RECV_BUFFER, 64);
	rxprt->send_pool = dw_rbufpool_create(rxprt->pd, SEND_BUFFER, 64);
    //rxprt->longrpc_pool = dw_rbufpool_create(rxprt->pd, RDMA_LONG_BUFFER, 32);

	rxprt->conn = NULL;
	
	return &rxprt->xprt;
}

int 
prepare_rdma_xprt(RDMA_SVCXPRT *rxprt)
{
	struct rdma_event_channel      *cm_channel;
	struct rdma_cm_id	       	*listen_id;
	
	struct ibv_pd		       		*pd;
	struct ibv_comp_channel	       *comp_chan;

	struct sockaddr_in				sin;

	int							err;

	//static struct build_dw				build_dw;//2014-7这里有疑问，如果不行就改回来
	struct build_dw		*build_dw_ptr = malloc(sizeof(struct build_dw));
	pthread_t 					pending_thread;
	pthread_t						event_thread;

	struct ibv_device 				**dev_list = NULL;
	struct ibv_context				*ib_ctx = NULL;

	int							dev_num;
	
	dev_list = ibv_get_device_list(&dev_num);
	PRINTF_INFO("the dev_num is %d\n", dev_num);
	ib_ctx = ibv_open_device(dev_list[0]);
	ibv_free_device_list(dev_list);
	ibv_close_device(ib_ctx);

	cm_channel = rdma_create_event_channel();
	if (!cm_channel)
		return 1;

	err = rdma_create_id(cm_channel, &listen_id, NULL, RDMA_PS_TCP);
	if (err)
		return err;

	listen_id->verbs = ib_ctx;

	PRINTF_INFO("the context is %p\n", listen_id->verbs);//0x604570
	pd = ibv_alloc_pd(listen_id->verbs);
	if (!pd)
		return 1;

	comp_chan = ibv_create_comp_channel(listen_id->verbs);
	if (!comp_chan)
		return 1;
	//build_dw.pd = pd;
	//build_dw.comp_chan = comp_chan;
	build_dw_ptr->pd = pd;
	build_dw_ptr->comp_chan = comp_chan;

	pthread_create(&pending_thread, NULL, pending_handler, build_dw_ptr);

	sin.sin_family	    = AF_INET;
	sin.sin_port	    = htons(37549);
	sin.sin_addr.s_addr = INADDR_ANY;

	err = rdma_bind_addr(listen_id, (struct sockaddr *) &sin);
	if (err){
		PRINTF_ERR("err occur while rdma_bind_addr\n");
		return 1;
	}

	rxprt->cm_id = listen_id;
	rxprt->pd = pd;
	rxprt->comp_chan = comp_chan;
	rxprt->type = 0;
	fill_xprt_rdma(&rxprt->xprt);

	listen_id->context = rxprt;

	pthread_create(&event_thread, NULL, event_handler, cm_channel);

	err = rdma_listen(listen_id, 1);
	if (err){
		PRINTF_ERR("err occur while rdma_listen\n");
		return 1;
	}
	//printf("rdma listen success~\n");
	
	return 0;
	
}

void *
pending_handler(void *arg)
{
	struct ibv_comp_channel 	*comp_chan;
	struct ibv_cq				*evt_cq;
	struct rdma_cm_id 		*cm_id;
	void						*cq_context;
	struct ibv_wc				wc;

	int						err;
	RDMA_SVCXPRT			*rxprt;
	
	//struct timeval tim1,tim2;
	//unsigned long used_time;
	comp_chan = ((struct build_dw *)arg)->comp_chan;
	while(1){

		PRINTF_INFO("wait here err=%d\n",err);
		
		if(ibv_get_cq_event(comp_chan, &evt_cq, &cq_context))
		{
			PRINTF_ERR("err occur while get cq event\n");
			return NULL;
		}
		//gettimeofday(&tim1,NULL);
		ibv_ack_cq_events(evt_cq, 1);

		cm_id = (struct rdma_cm_id*)evt_cq->cq_context;
		rxprt = (RDMA_SVCXPRT *)cm_id->context;
		PRINTF_INFO("now we get data\n");

		if (ibv_req_notify_cq(evt_cq, 0))
		{
			PRINTF_ERR("err occur while notify cq\n");
			return  NULL;
		}
	int i = 0;
		/* MODIFIED by dwyane @ 2010-12-16 */
		while((err = ibv_poll_cq(cm_id->qp->recv_cq, 1, &wc)) > 0){
			i++;
			PRINTF_INFO("the data is from recv_cq,len is %d wr_id:%d err=%d\n", wc.byte_len,wc.wr_id,err);
			if (wc.status != IBV_WC_SUCCESS){
				printf("pending_handler: wc.status != IBV_WC_SUCCESS.wc.wc_flags is %d,wc.wr_id is %ld\n",wc.wc_flags,wc.wr_id);
				rwlock_wrlock(&rxprt->close_lock);
				if(cm_id->qp->state == IBV_QPS_ERR){
					PRINTF_INFO("qp state is %d\n", cm_id->qp->state);
				
					/* MODIFIED by yh @ 2014-04-22 */
					if(rxprt->conn->to_be_done1->data){
						printf("big mistake1\n");
						dw_rbuf_free(rxprt->conn, RECV_BUFFER, &rxprt->conn->to_be_done1->data);
						rxprt->conn->to_be_done1->data_prt = NULL;
						break;
					}
					#if 0
					if(rxprt->conn->to_be_done2->data){
						printf("big mistake2\n");
						dw_rbuf_free(rxprt->conn, RECV_BUFFER, &rxprt->conn->to_be_done2->data);
						rxprt->conn->to_be_done2->data_prt = NULL;
					}
					#endif
					PRINTF_INFO("bfree is %d\n", rxprt->conn->recv_pool->bpool->buffree);
					mem_free(rxprt->conn->to_be_done1, sizeof(struct vec));
					mem_free(rxprt->conn, sizeof(CONN));
					mem_free(rxprt, sizeof(RDMA_SVCXPRT));
				}else{
					PRINTF_ERR("request return err, wc.status is %d, wc.opcode is %d\n", wc.status, wc.opcode);
				}
				rwlock_unlock(&rxprt->close_lock);
				/* END */
				break;
			}
			//ibv_ack_cq_events(cm_id->qp->recv_cq, 1);
			rxprt->conn->to_be_done1->length = wc.byte_len;
			PRINTF_INFO("data length is %d\n", wc.byte_len);			
			/*if(wc.wc_flags==IBV_WC_WITH_IMM)
			{
				//printf("get IMM in pending_handler\n");
				dw_rbuf_free(rxprt->conn, RECV_BUFFER, &rxprt->conn->to_be_done1->data);
				rxprt->conn->to_be_done1->data_prt = NULL;
				svc_rdma_freeres(rxprt,rxprt->xdr_results,rxprt->xdr_location);
				gettimeofday(&tim2,NULL);
				used_time = (tim2.tv_sec-tim1.tv_sec)*1000000.0 + (tim2.tv_usec-tim1.tv_usec);
				printf("ack used_time is %ul,In %s: %d\n",used_time,__func__,__LINE__);
            		}
			else {*/
			dwyane_enqueue(&rxprt->xprt);
			//gettimeofday(&tim2,NULL);				
			//used_time = (tim2.tv_sec-tim1.tv_sec)*1000000.0 + (tim2.tv_usec-tim1.tv_usec);
			//printf("nomal used_time is %ul,In %s: %d\n",used_time,__func__,__LINE__);
			//}
		}
		
		/* END */
	}

	PRINTF_INFO("exit %s\n", __func__);
	pthread_exit(NULL);
	return(NULL);
}

void *
event_handler(void *arg)
{
	struct rdma_event_channel      	*cm_channel = (struct rdma_event_channel *)arg;
	struct rdma_cm_event	       	*event;
	int 							err;
	RDMA_SVCXPRT				*rxprt;

	while(1){
		err = rdma_get_cm_event(cm_channel, &event);//blocking
		PRINTF_INFO("%s:get cm event\n",__func__);

		if (err){
			PRINTF_ERR("errer occur while get event\n");
			return NULL;
		}

		switch(event->event){
			case RDMA_CM_EVENT_CONNECT_REQUEST:
				PRINTF_INFO("get connect\n");
				rxprt = (RDMA_SVCXPRT *)event->listen_id->context;
				rxprt->xprt.xp_p1 = event->id;
				dwyane_enqueue(&rxprt->xprt);
				break;
			case RDMA_CM_EVENT_ESTABLISHED:
				PRINTF_INFO("get established\n");
				break;
			case RDMA_CM_EVENT_DISCONNECTED:
				//printf("RDMA_CM_EVENT_DISCONNECTED\n");
				rxprt = (RDMA_SVCXPRT *)event->id->context;
				/* MODIFIED by dwyane @ 2011-01-12 */
				rwlock_wrlock(&rxprt->close_lock);
				rdma_disconnect(event->id);
				PRINTF_INFO("get disconnected\n\n\n\n\n\n\n\n\n\n\n");
				/*if (rxprt->dw_outxdr.x_ops != NULL) {
                XDR_DESTROY(&(rxprt->dw_outxdr));
                }*/
				rwlock_unlock(&rxprt->close_lock);
				/* END */
				break;
			default:
				break;
		}

		rdma_ack_cm_event(event);
	}
	
	return NULL;
}

void fill_xprt_rdma(SVCXPRT *xprt){
	RDMA_SVCXPRT *rxprt;

	rxprt = list_entry(xprt, RDMA_SVCXPRT, xprt);
	xprt->xp_fd = -1;
	xprt->xp_port = 37549;
	if(rxprt->type)
		svc_rdma_ops(xprt);
	else
		svc_rdma_listen_ops(xprt);
	xprt->xp_netid = strdup("rdma");
	xprt->busy = 0;
	pthread_cond_init(&xprt->busy_cond,NULL);
	pthread_mutex_init(&xprt->busy_lock,NULL);
	xprt->busy_flag = 0;
}

void
svc_rdma_ops(SVCXPRT *xprt)
{
	static struct xp_ops ops;
	static struct xp_ops2 ops2;
	extern mutex_t ops_lock;

/* VARIABLES PROTECTED BY ops_lock: ops, ops2 */

	mutex_lock(&ops_lock);
	if (ops.xp_recv == NULL) {
		ops.xp_recv = svc_rdma_recv;
		ops.xp_stat = svc_rdma_stat;
		ops.xp_getargs = svc_rdma_getargs;
		ops.xp_reply = svc_rdma_reply;
		ops.xp_freeargs = svc_rdma_freeargs;
		ops.xp_destroy = svc_rdma_destroy;
		ops2.xp_control = svc_rdma_control;
	}
	xprt->xp_ops = &ops;
	xprt->xp_ops2 = &ops2;
	mutex_unlock(&ops_lock);
}

void
svc_rdma_listen_ops(SVCXPRT *xprt)
{
	static struct xp_ops ops;
	static struct xp_ops2 ops2;
	extern mutex_t ops_lock;

	mutex_lock(&ops_lock);
	if (ops.xp_recv == NULL) {
		ops.xp_recv = svc_rdma_accept;
		ops.xp_stat = svc_rdma_stat;
		ops.xp_getargs =
		    (bool_t (*)(SVCXPRT *, xdrproc_t, void *))abort;
		ops.xp_reply =
		    (bool_t (*)(SVCXPRT *, struct rpc_msg *))abort;
		ops.xp_freeargs =
		    (bool_t (*)(SVCXPRT *, xdrproc_t, void *))abort,
		ops.xp_destroy = svc_rdma_destroy;
		ops2.xp_control = svc_rdma_control;
	}
	xprt->xp_ops = &ops;
	xprt->xp_ops2 = &ops2;
	mutex_unlock(&ops_lock);
}

static bool_t svc_rdma_recv(SVCXPRT *xprt, struct rpc_msg *msg)
{
	PRINTF_INFO("%s\n", __func__);
	RDMA_SVCXPRT *rxprt;
	CONN *conn;
	struct vec *recv_data;
	XDR *xdrs;
	struct rdma_conn *cd;
	cd = (struct rdma_conn *)(xprt->xp_p1);

	/* MODIFIED by dwyane @ 2011-01-06 */
	struct clist	*cl_reply;
        struct clone_rdma_data *crdp;
	/* END */
        crdp = (struct clone_rdma_data *)xprt->xp_p2buf;

	rxprt = list_entry(xprt, RDMA_SVCXPRT, xprt);
	conn = rxprt->conn;
	/* MODIFIED by dwyane @ 2011-01-12 */
	recv_data = mem_alloc(sizeof(struct vec));
#if 0
	if(conn->to_be_done1->data==NULL){
		recv_data->data = conn->to_be_done2->data;
		recv_data->length = conn->to_be_done2->length;
	}else{
		recv_data->data = conn->to_be_done1->data;
		recv_data->length = conn->to_be_done1->length;
	}
#else
	recv_data->data = conn->to_be_done1->data;
	recv_data->length = conn->to_be_done1->length;
#endif
	/* END */
	

	PRINTF_INFO("the data recv is %p:%10s, length is %d\n", recv_data->data, recv_data->data,recv_data->length);
	
#if 1
	
	struct clist	*cl = NULL;
	struct clist	*wcl = NULL;
	struct clist	*cllong = NULL;

	rdma_stat	status;
	u_int32_t vers, op, pos, xid;
	u_int32_t rdma_credit;
	u_int32_t wcl_total_length = 0;
	bool_t	wwl = FALSE;
	bool_t		have_rchunk = FALSE;
        
	//ming why post everyrecv?
	status = rdma_svc_postrecv(conn);
	if (status != RDMA_SUCCESS) {
		goto badrpc_call;
	}

	xdrs = &rxprt->dw_inxdr;
	xdrmem_create(xdrs, recv_data->data, recv_data->length, XDR_DECODE);  

	xid = *(uint32_t *)recv_data->data;
	rxprt->dw_xid = xid;
	
	cd->x_id = xid;

	//printf("xid is %d,in %s\n",xid,__func__);
	XDR_SETPOS(xdrs, sizeof (u_int32_t));
        
	if (! xdr_u_int(xdrs, &vers) ||
	    ! xdr_u_int(xdrs, &rdma_credit) ||
	    ! xdr_u_int(xdrs, &op)) {
		goto xdr_err;
	}
    PRINTF_DEBUG("vers:%d,rdma_credit:%d,op:%d\n",vers,rdma_credit,op);

	/* Checking if the status of the recv operation was normal */
	/*

	/* MODIFIED by dwyane @ 2011-01-06 */
	pos = XDR_GETPOS(xdrs);
	PRINTF_INFO("here before xdr_do_clist, pos is %d\n", pos);
	/* END */
	
	if (! xdr_do_clist(xdrs, &cl)) {
		goto xdr_err;
	}

	/* MODIFIED by dwyane @ 2011-01-06 */
	pos = XDR_GETPOS(xdrs);
	PRINTF_INFO("here after xdr_do_clist, pos is %d\n", pos);
	/* END */
	
	if (!xdr_decode_wlist_svc(xdrs, &wcl, &wwl, &wcl_total_length, conn)) {
		/* MODIFIED by dwyane @ 2011-01-06 */
		PRINTF_ERR("xdr_decode_wlist_svc return err\n");
		/* END */
		if (cl){
			clist_free(cl);
			cl = NULL;
		}
		goto xdr_err;
	}
	
	crdp->cl_wlist = wcl;

	crdp->cl_reply = NULL;
	(void) xdr_decode_reply_wchunk(xdrs, &crdp->cl_reply);

	/*
	 * A chunk at 0 offset indicates that the RPC call message
	 * is in a chunk. Get the RPC call message chunk.
	 */
	if (cl != NULL && op == RDMA_NOMSG) {
		PRINTF_ERR("should not happen in %s:%d",__func__,__LINE__);
#if 0
		/* Remove RPC call message chunk from chunklist */
		cllong = cl;
		cl = cl->c_next;
		cllong->c_next = NULL;

        PRINTF_DEBUG("%s,%d cllong->c_len:%ld\n",__func__,__LINE__,cllong->c_len);
		/* Allocate and register memory for the RPC call msg chunk */
		cllong->rb_longbuf.type = RDMA_LONG_BUFFER;
		cllong->rb_longbuf.len = cllong->c_len > LONG_REPLY_LEN ?
		    cllong->c_len : LONG_REPLY_LEN;

		if (rdma_buf_alloc(conn, &cllong->rb_longbuf)) {
			clist_free(cllong);
			cllong = NULL;
			goto cll_malloc_err;
		}
		cllong->u.c_daddr3 = cllong->rb_longbuf.addr;

		if (cllong->u.c_daddr == NULL) {
			rdma_buf_free(conn, &cllong->rb_longbuf);
			clist_free(cllong);
			cllong = NULL;
			goto cll_malloc_err;
		}
		status = clist_register(conn, cllong, CLIST_REG_DST);
		if (status) {
			rdma_buf_free(conn, &cllong->rb_longbuf);
			clist_free(cllong);
			cllong = NULL;
			goto cll_malloc_err;
		}

		/*
		 * Now read the RPC call message in
		 */
		status = dw_read(conn, cllong);
		if (status) {
			(void) clist_deregister(conn, cllong);
			rdma_buf_free(conn, &cllong->rb_longbuf);
			clist_free(cllong);
			cllong = NULL;
			goto cll_malloc_err;
		}
		(void) clist_deregister(conn, cllong);
		xdrrdma_create_svc(xdrs, (caddr_t)(uintptr_t)cllong->u.c_daddr3,
		    cllong->c_len, 0, cl, NULL, XDR_DECODE, conn);
		crdp->rpcbuf = cllong->rb_longbuf;
		crdp->rpcbuf.len = cllong->c_len;
		clist_free(cllong);
		cllong = NULL;
#endif
	} else {
		pos = XDR_GETPOS(xdrs);
		PRINTF_INFO("here before xdrrdma_create, pos is %d\n", pos);
		xdrrdma_create_svc(xdrs, recv_data->data+ pos,
		    recv_data->length - pos, 0, cl, NULL, XDR_DECODE, conn);

#if 0

		/* Use xdrrdmablk_ops to indicate there is a read chunk list */
		if (cl != NULL) {
			int32_t flg = XDR_RDMA_RLIST_REG;

			XDR_CONTROL(xdrs, XDR_RDMA_SET_FLAGS, &flg);
			xdrs->x_ops = &xdrrdmablk_ops;
		}
	}
	
	if (crdp->cl_wlist) {
		int32_t flg = XDR_RDMA_WLIST_REG;

		XDR_CONTROL(xdrs, XDR_RDMA_SET_WLIST, crdp->cl_wlist);
		XDR_CONTROL(xdrs, XDR_RDMA_SET_FLAGS, &flg);
	}
#endif
        }
	if (! xdr_callmsg(xdrs, msg)) {
		goto callmsg_err;
	}

	/* MODIFIED by dwyane @ 2011-01-12 */
	dw_rbuf_free(conn, RECV_BUFFER, &recv_data->data);
#if 1
	mem_free(recv_data, sizeof(struct vec));
#endif
	/* END */
        crdp->conn = conn;
	
    PRINTF_DEBUG("exit of %s\n", __func__);
	
	return (TRUE);

callmsg_err:
	//rdma_buf_free(conn, &crdp->rpcbuf);

cll_malloc_err:
	//yh:4-25
	//if (cl){
		//clist_free(cl);
		//cl=NULL;
	//}
xdr_err:
	XDR_DESTROY(xdrs);

badrpc_call:
	//RDMA_BUF_FREE(conn, &rdp->rpcmsg);
	//RDMA_REL_CONN(conn);
	//freeb(mp);
	//RSSTAT_INCR(rsbadcalls);
	//printf("%s,%d\n",__func__,__LINE__);
#endif
	return (FALSE);
}

static bool_t svc_rdma_accept(SVCXPRT *xprt, struct rpc_msg *msg)
{
	PRINTF_INFO("enter %s\n", __func__);
	
	RDMA_SVCXPRT 			*new_rxprt, *listen_xprt;
	struct rdma_cm_id		*cm_id;
	struct rdma_conn_param	conn_param = { };
	struct ibv_cq		       	*send_cq, *recv_cq;
	struct ibv_qp_init_attr		qp_attr = { };
	int						err;
	struct rdma_conn *cd;
	cd = mem_alloc(sizeof(struct rdma_conn));

	listen_xprt = list_entry(xprt, RDMA_SVCXPRT, xprt);
	new_rxprt = mem_alloc(sizeof(RDMA_SVCXPRT));
	
	new_rxprt->type = 1;
	new_rxprt->cm_id = (struct rdma_cm_id *)xprt->xp_p1;
	new_rxprt->pd = listen_xprt->pd;
	new_rxprt->comp_chan = listen_xprt->comp_chan;
	fill_xprt_rdma(&new_rxprt->xprt);
	new_rxprt->cm_id->context = new_rxprt;

	cm_id = new_rxprt->cm_id;

	send_cq = ibv_create_cq(cm_id->verbs, 2, NULL, new_rxprt->comp_chan, 0);
	if (!send_cq)
		return -1;

	if (ibv_req_notify_cq(send_cq, 0))
		return -1;

	send_cq->cq_context = cm_id;

	recv_cq = ibv_create_cq(cm_id->verbs, 2, NULL, new_rxprt->comp_chan, 0);
	if (!recv_cq)
		return -1;

	if (ibv_req_notify_cq(recv_cq, 0))
		return -1;

	recv_cq->cq_context = cm_id;

	/* establish qp */

	qp_attr.cap.max_send_wr	 	= SVC_MAX_SEND_WR;
	qp_attr.cap.max_send_sge 		= SVC_MAX_SEND_SGE;
	qp_attr.cap.max_recv_wr	 	= SVC_MAX_RECV_WR;
	qp_attr.cap.max_recv_sge 		= SVC_MAX_RECV_SGE;

	qp_attr.send_cq		 		= send_cq;
	qp_attr.recv_cq		 		= recv_cq;

	qp_attr.qp_type		 		= IBV_QPT_RC;

	err = rdma_create_qp(cm_id, new_rxprt->pd, &qp_attr);
	if (err){
		PRINTF_ERR("rdma_create_qp failure,err is %d in %s: %d",err,__func__,__LINE__);
		return -1;
	}

	new_rxprt->conn = alloc_conn(new_rxprt->cm_id, new_rxprt->pd, 
								new_rxprt->comp_chan, 
								listen_xprt->send_pool, listen_xprt->recv_pool);

	/* MODIFIED by dwyane @ 2011-01-12 */
	if(!new_rxprt->conn)
		PRINTF_ERR("alloc_conn err\n");
	else
		PRINTF_INFO("new_rxprt is %p\n", new_rxprt);

	rwlock_init(&new_rxprt->close_lock, NULL);
	//rwlock_wrlock(&new_rxprt->close_lock);
	/* END */

	rdma_svc_postrecv(new_rxprt->conn);
	/* rdma_accept */
	
	conn_param.initiator_depth 	   		= 1;
	conn_param.responder_resources 	= 1;

	err = rdma_accept(cm_id, &conn_param);
	if (err){
		PRINTF_ERR("rdma_accept failure in %s: %d",__func__,__LINE__);
		return -1;
	}
    new_rxprt->xprt.xp_rtaddr.buf=(void *)&cm_id->route.addr.src_addr;
	new_rxprt->xprt.xp_p1=(void *)cd;

	PRINTF_INFO("exit %s\n", __func__);
	return 0;
	
}

static enum xprt_stat svc_rdma_stat(SVCXPRT *xprt)
{
}

static bool_t svc_rdma_getargs(SVCXPRT *xprt, xdrproc_t xdr_args, void *xdr_ptr)
{
	RDMA_SVCXPRT *rxprt;
	//CONN *conn;

	rxprt = list_entry(xprt, RDMA_SVCXPRT, xprt);
	//conn = rxprt->conn;
	//printf("%s\n",__func__);
	return (*xdr_args)(&rxprt->dw_inxdr, xdr_ptr);

}


static int
svc_process_long_reply(SVCXPRT* clone_xprt,
    xdrproc_t xdr_results, caddr_t xdr_location,
    struct rpc_msg *msg, bool_t has_args, int *msglen,
    int *freelen, int *numchunks, unsigned int *final_len)
{
	int status;
	XDR xdrslong;
	struct clist *wcl = NULL;
	int count = 0;
	int alloc_len;
	char  *memp;
	rdma_buf_t long_rpc = {0};
	struct clone_rdma_data *crdp;
	RDMA_SVCXPRT *rxprt;

    PRINTF_DEBUG("func:%s line:%d\n",__func__,__LINE__);
	crdp = (struct clone_rdma_data *)clone_xprt->xp_p2buf;
        rxprt = list_entry(clone_xprt, RDMA_SVCXPRT, xprt);

	bzero(&xdrslong, sizeof (xdrslong));
	/* Choose a size for the long rpc response */
	alloc_len = RNDUP(MAX_AUTH_BYTES + *msglen);
    PRINTF_DEBUG("func:%s line:%d      msglen:%d alloc_len:%d\n",__func__,__LINE__,*msglen,alloc_len);

	if (alloc_len <= 64 * 1024) {
		if (alloc_len > 32 * 1024) {
			alloc_len = 64 * 1024;
		} else {
			if (alloc_len > 16 * 1024) {
				alloc_len = 32 * 1024;
			} else {
				alloc_len = 16 * 1024;
			}
		}
	}

    PRINTF_DEBUG("func:%s line:%d        wcl->c_len:%d\n",__func__,__LINE__,crdp->cl_reply->c_len);
	long_rpc.type = RDMA_LONG_BUFFER;
	long_rpc.len = alloc_len;
	if (rdma_buf_alloc(crdp->conn, &long_rpc)) {
                printf("SVC_RDMA_FAIL   func:%s line:%d\n",__func__,__LINE__);
		return (SVC_RDMA_FAIL);
	}
	memp = long_rpc.addr;
	xdrmem_create(&xdrslong, memp, alloc_len, XDR_ENCODE);

    msg->rm_xid = rxprt->dw_xid;

	if(has_args){
           if(xdr_replymsg(&xdrslong, msg)){
              printf("begin encode return results       func:%s line:%d\n",__func__,__LINE__);
	     if(!(*xdr_results)(&xdrslong, xdr_location)) {
                printf("SVC_RDMA_FAIL   func:%s line:%d\n",__func__,__LINE__);
		rdma_buf_free(crdp->conn, &long_rpc);
		printf("SVC_RDMA_FAIL   func:%s line:%d\n",__func__,__LINE__);
		return (SVC_RDMA_FAIL);
                }
            }
	}
	*final_len = XDR_GETPOS(&xdrslong);
	*numchunks = 0;
	*freelen = 0;
	wcl = crdp->cl_reply;
	wcl->rb_longbuf = long_rpc;
	count = *final_len;
    PRINTF_DEBUG("func:%s line:%d       count=%d              wcl->c_len:%d\n",__func__,__LINE__,count,wcl->c_len);
	while ((wcl != NULL) && (count > 0)) {

		if (wcl->c_dmemhandle.mrc_rmr == 0)
			break;

		if (wcl->c_len > count) {
			count=wcl->c_len;
		}
		wcl->w.c_saddr3 = (caddr_t)memp;

		count -= wcl->c_len;
        PRINTF_DEBUG("func:%s line:%d       count=%d\n",__func__,__LINE__,count);
		*numchunks +=  1;
		memp += wcl->c_len;
		wcl = wcl->c_next;
	}

	/*
 * 	 * Make rest of the chunks 0-len
 * 	 	 */
	while (wcl != NULL) {
		if (wcl->c_dmemhandle.mrc_rmr == 0)
			break;
		wcl->c_len = 0;
		wcl = wcl->c_next;
	}
	wcl = crdp->cl_reply;

	/*
 * 	 * MUST fail if there are still more data
 * 	 	 */
	if (count > 0) {
		rdma_buf_free(crdp->conn, &long_rpc);
                printf("SVC_RDMA_FAIL   func:%s line:%d\n",__func__,__LINE__);
		return (SVC_RDMA_FAIL);
	}

	if (clist_register(crdp->conn, wcl, CLIST_REG_SOURCE) != RDMA_SUCCESS) {
                printf("SVC_RDMA_FAIL   func:%s line:%d\n",__func__,__LINE__);
		rdma_buf_free(crdp->conn, &long_rpc);
		return (SVC_RDMA_FAIL);
	}


	status = dw_write(crdp->conn, wcl);

	(void) clist_deregister(wcl);
	rdma_buf_free(crdp->conn, &wcl->rb_longbuf);

	if (status != RDMA_SUCCESS) {
		printf("SVC_RDMA_FAIL   func:%s line:%d\n",__func__,__LINE__);
		return (SVC_RDMA_FAIL);
	}
	return (SVC_RDMA_SUCCESS);
}


static bool_t svc_rdma_reply(SVCXPRT *xprt, struct rpc_msg *msg)
{
	PRINTF_INFO("%s\n", __func__);
	
	int ack_flag = 0;
	RDMA_SVCXPRT *rxprt;
	CONN *conn;

	XDR *xdrs;
        struct clone_rdma_data *crdp; 

    crdp = (struct clone_rdma_data *)xprt->xp_p2buf;
	rxprt = list_entry(xprt, RDMA_SVCXPRT, xprt);
	conn = rxprt->conn;
	
	XDR *xdrs_rpc = &(rxprt->dw_outxdr);
	XDR xdrs_rhdr;
	rdma_buf_t rbuf_resp = {0}, rbuf_rpc_resp = {0};

	struct clist *cl_read = NULL;
	struct clist *cl_send = NULL;
	struct clist *cl_write = NULL;
	xdrproc_t xdr_results;		/* results XDR encoding function */
	caddr_t xdr_location;		/* response results pointer */

	int retval = FALSE;
	int status, msglen, num_wreply_segments = 0;
	u_int32_t rdma_credit = 0;
	int freelen = 0;
	bool_t has_args;
	u_int32_t  final_resp_len, rdma_response_op, vers;
	struct timeval tim1,tim2;
	unsigned long used_time;

	bzero(&xdrs_rhdr, sizeof (XDR));
	
	cl_write = crdp->cl_wlist;

	/*
	 * If there is a result procedure specified in the reply message,
	 * it will be processed in the xdr_replymsg and SVCAUTH_WRAP.
	 * We need to make sure it won't be processed twice, so we null
	 * it for xdr_replymsg here.
	 */
	has_args = FALSE;
    PRINTF_DEBUG("msg->rm_reply.rp_stat:%d msg->rm_reply.rp_acpt.ar_stat:%d\n",msg->rm_reply.rp_stat,msg->rm_reply.rp_acpt.ar_stat);
	if (msg->rm_reply.rp_stat == MSG_ACCEPTED &&
	    msg->rm_reply.rp_acpt.ar_stat == SUCCESS) {
		if ((xdr_results = msg->acpted_rply.ar_results.proc) != NULL) {
			has_args = TRUE;
			xdr_location = msg->acpted_rply.ar_results.where;
			PRINTF_INFO("%s:result value is %p:%d\n",__func__,
				xdr_location,*(int *)xdr_location);
			PRINTF_INFO("%s:xdr_proc %p:%p\n",__func__,
				xdr_results,xdr_int);
			msg->acpted_rply.ar_results.proc = xdr_void;
			msg->acpted_rply.ar_results.where = NULL;
		}
	}
	rxprt->xdr_results=xdr_results;
    rxprt->xdr_location=xdr_location;
	/*
	 * Given the limit on the inline response size (RPC_MSG_SZ),
	 * there is a need to make a guess as to the overall size of
	 * the response.  If the resultant size is beyond the inline
	 * size, then the server needs to use the "reply chunk list"
	 * provided by the client (if the client provided one).  An
	 * example of this type of response would be a READDIR
	 * response (e.g. a small directory read would fit in RPC_MSG_SZ
	 * and that is the preference but it may not fit)
	 *
	 * Combine the encoded size and the size of the true results
	 * and then make the decision about where to encode and send results.
	 *
	 * One important note, this calculation is ignoring the size
	 * of the encoding of the authentication overhead.  The reason
	 * for this is rooted in the complexities of access to the
	 * encoded size of RPCSEC_GSS related authentiation,
	 * integrity, and privacy.
	 *
	 * If it turns out that the encoded authentication bumps the
	 * response over the RPC_MSG_SZ limit, then it may need to
	 * attempt to encode for the reply chunk list.
	 */

	/*
	 * Calculating the "sizeof" the RPC response header and the
	 * encoded results.
	 */
#if 0
		msglen= xdr_sizeof(xdr_replymsg, msg);
        PRINTF_DEBUG("msglen:%d\n",msglen);
        if (has_args)
		msglen += xdrrdma_sizeof(xdr_results,xdr_location,1024, NULL,NULL);
        PRINTF_DEBUG("msglen:%d\n",msglen);
        PRINTF_DEBUG("has_args:%d add_msglen:\n",has_args,xdrrdma_sizeof(xdr_results, xdr_location,1024, NULL, NULL));
#endif
    status = SVC_RDMA_SUCCESS;
	msglen = 0;

	if (msglen < RPC_MSG_SZ) {
		/*
		 * Looks like the response will fit in the inline
		 * response; let's try
		 */

		rdma_response_op = RDMA_MSG;

		status = svc_compose_rpcmsg(xprt, conn, xdr_results,
		    xdr_location, &rbuf_rpc_resp, &xdrs_rpc, msg,
		    has_args, &final_resp_len,cl_write);
		if(cl_write!=NULL){
			int32_t xdr_flag = XDR_RDMA_WLIST_REG;
			XDR_CONTROL(xdrs_rpc, XDR_RDMA_SET_FLAGS, &xdr_flag);
		}
		status = clist_register(conn, cl_write, CLIST_REG_SOURCE);
		if(status!=RDMA_SUCCESS)
			PRINTF_ERR("clist_register err in %s:%d\n",__func__,__LINE__);
	}

	/*
	 * If the encode failed (size?) or the message really is
	 * larger than what is allowed, try the response chunk list.
	 */
	 else{
		PRINTF_ERR("should not happen! in %s:%d\n",__func__,__LINE__);
#if 0
		if (crdp->cl_reply == NULL) {
                     printf("func:%s line:%d   crdp->cl_reply==NULL\n",__func__,__LINE__);
                     crdp->cl_reply=mem_alloc(sizeof(struct clist));
                }

		msglen = xdr_sizeof(xdr_replymsg, msg);
        PRINTF_DEBUG("func:%s line:%d      msglen:%d\n",__func__,__LINE__,msglen);
		msglen += xdrrdma_sizeof(xdr_results, xdr_location, 0,
		    NULL, NULL);
        PRINTF_DEBUG("func:%s line:%d      msglen:%d\n",__func__,__LINE__,msglen);
        PRINTF_DEBUG("func:%s line:%d        wcl->c_len:%d\n",__func__,__LINE__,crdp->cl_reply->c_len);
		status = svc_process_long_reply(xprt, xdr_results,
		    xdr_location, msg, has_args, &msglen, &freelen,
		    &num_wreply_segments, &final_resp_len);

		if (status != SVC_RDMA_SUCCESS) {
			goto out;
		}

		rdma_response_op = RDMA_NOMSG;
#endif
	}
    PRINTF_DEBUG("msg->rm_reply.rp_acpt.ar_stat:%d\n",msg->rm_reply.rp_acpt.ar_stat);

	rbuf_resp.type = SEND_BUFFER;
	if (rdma_buf_alloc(conn, &rbuf_resp)) {
		rdma_buf_free(conn, &rbuf_rpc_resp);
		goto out;
	}

 	rdma_credit = rdma_bufs_granted;
	vers = 2;
	xdrmem_create(&xdrs_rhdr, rbuf_resp.addr, rbuf_resp.len, XDR_ENCODE);
	(*(uint32_t *)rbuf_resp.addr) = msg->rm_xid;
	/* Skip xid and set the xdr position accordingly. */
	XDR_SETPOS(&xdrs_rhdr, sizeof (uint32_t));
	if (!xdr_u_int(&xdrs_rhdr, &vers) ||
	    !xdr_u_int(&xdrs_rhdr, &rdma_credit) ||
	    !xdr_u_int(&xdrs_rhdr, &rdma_response_op)) {
		rdma_buf_free(conn, &rbuf_rpc_resp);
		rdma_buf_free(conn, &rbuf_resp);
		goto out;
	}
    PRINTF_DEBUG("reply vers:%d,rdma_credit:%d,rdma_response_op:%d\n",vers,rdma_credit,rdma_response_op);
	PRINTF_INFO("%s:xid:vers:credit:res_op~%d:%d:%d:%d\n",__func__,
		msg->rm_xid,vers,rdma_credit,rdma_response_op);
	PRINTF_INFO("rhdr len is %d\n",XDR_GETPOS(&xdrs_rhdr));
	/*
	 * Now XDR the read chunk list, actually always NULL
	 */
	//(void) xdr_encode_rlist_svc(&xdrs_rhdr, cl_read);
	
	svc_setup_rlist(conn,&xdrs_rhdr,xdrs_rpc,&ack_flag);
	/*
	 * encode write list -- we already drove RDMA_WRITEs
	 */
	//if (!xdr_encode_wlist(conn,&xdrs_rhdr, cl_write,CLIST_REG_SOURCE)) {
	if (!xdr_encode_wlist(conn,&xdrs_rhdr, NULL,CLIST_REG_SOURCE)) {
		rdma_buf_free(conn, &rbuf_rpc_resp);
		rdma_buf_free(conn, &rbuf_resp);
		goto out;
	}

	/*
	 * XDR encode the RDMA_REPLY write chunk
	 */
#if 1
	if (!xdr_encode_reply_wchunk(&xdrs_rhdr, crdp->cl_reply,
	    num_wreply_segments)){
                printf("NULL\n\n");
		rdma_buf_free(conn, &rbuf_rpc_resp);
		rdma_buf_free(conn, &rbuf_resp);
		goto out;
	}
#endif
	PRINTF_INFO("rhdr len is %d\n",XDR_GETPOS(&xdrs_rhdr));
	clist_add(&cl_send, 0, XDR_GETPOS(&xdrs_rhdr), &rbuf_resp.handle,
	    rbuf_resp.addr, NULL, NULL);

	if (rdma_response_op == RDMA_MSG) {
		clist_add(&cl_send, 0, final_resp_len, &rbuf_rpc_resp.handle,
		    rbuf_rpc_resp.addr, NULL, NULL);
	}

				
	//gettimeofday(&tim1,NULL);	
	status = dw_write(conn, cl_write);
	//gettimeofday(&tim2,NULL);	
	//used_time = (tim2.tv_sec-tim1.tv_sec)*1000000.0 + (tim2.tv_usec-tim1.tv_usec);
	//printf("dw_write used_time is %ul,In %s: %d\n",used_time,__func__,__LINE__);
	if (status == RDMA_SUCCESS) {
        PRINTF_DEBUG("func:%s line:%d                retval==TRUE\n",__func__,__LINE__);
		retval = TRUE;
	}
	
	

	status = dw_send(conn, cl_send, msg->rm_xid);
	//gettimeofday(&tim1,NULL);	
	//used_time = (tim1.tv_sec-tim2.tv_sec)*1000000.0 + (tim1.tv_usec-tim2.tv_usec);
	//printf("dw_send used_time is %ul,In %s: %d\n",used_time,__func__,__LINE__);
	if (status == RDMA_SUCCESS) {
		PRINTF_DEBUG("func:%s line:%d				 retval==TRUE\n",__func__,__LINE__);
		retval = TRUE;
	}
		

out:
	/*
	 * Free up sendlist chunks
	 */
	if (cl_send != NULL){
		clist_free(cl_send);
		cl_send = NULL;
	}

	if(!ack_flag){
		svc_rdma_freeres(rxprt, xdr_results,xdr_location);
	}
	/*
	 * Destroy private data for xdr rdma
	 */

	
	if (crdp->cl_reply) {
		
		clist_free(crdp->cl_reply);
		crdp->cl_reply = NULL;
	}
     
	/*
	 * This is completely disgusting.  If public is set it is
	 * a pointer to a structure whose first field is the address
	 * of the function to free that structure and any related
	 * stuff.  (see rrokfree in nfs_xdr.c).
	 */
	if (xdrs_rpc->x_public) {
		/* LINTED pointer alignment */
		(**((int (**)()) xdrs_rpc->x_public)) (xdrs_rpc->x_public);
	}
	if (xdrs_rhdr.x_ops != NULL) {
		XDR_DESTROY(&xdrs_rhdr);
	}

	/* MODIFIED by dwyane @ 2011-01-12 */
	rdma_buf_free(conn, &rbuf_rpc_resp);
	rdma_buf_free(conn, &rbuf_resp);
	/* END */
	return (retval);
}

static bool_t svc_rdma_freeargs(SVCXPRT *clone_xprt, xdrproc_t xdr_args, caddr_t args_ptr)
{
    struct clone_rdma_data *crdp;
	bool_t retval;
    RDMA_SVCXPRT *rxprt;

    PRINTF_DEBUG("\nfunc:%s line:%d file:%s   enter freeargs\n",__func__,__LINE__,__FILE__);
	crdp = (struct clone_rdma_data *)clone_xprt->xp_p2buf;

	rxprt = list_entry(clone_xprt, RDMA_SVCXPRT,xprt);

	if (args_ptr) {
		XDR	*xdrs = &rxprt->dw_inxdr;

		xdrs->x_op = XDR_FREE;
		retval = (*xdr_args)(xdrs, args_ptr);
	}

	XDR_DESTROY(&(rxprt->dw_inxdr));
	rdma_buf_free(crdp->conn, &crdp->rpcbuf);
	if (crdp->cl_reply) {
		clist_free(crdp->cl_reply);
		crdp->cl_reply = NULL;
	}

	return (retval);
}


static bool_t svc_rdma_freeres(RDMA_SVCXPRT *rxprt, xdrproc_t xdr_results, caddr_t results_ptr)
{
	bool_t retval;
	
	if (results_ptr) {
		XDR	*xdrs = &rxprt->dw_outxdr;
		xdrs->x_op = XDR_FREE;
		retval = (*xdr_results)(xdrs, results_ptr);
	}

	XDR_DESTROY(&(rxprt->dw_outxdr));	
	return (retval);
}


static void svc_rdma_destroy(SVCXPRT *xprt)
{
#if 0
	/* MODIFIED by dwyane @ 2011-01-11 */
	RDMA_SVCXPRT *rxprt;

	rxprt = list_entry(xprt, RDMA_SVCXPRT, xprt);

	free_conn(rxprt->conn);
#if 0
	if(rxprt->type){
		}
#endif
	mem_free(rxprt, sizeof(RDMA_SVCXPRT));
	/* END */
#endif
}

static bool_t svc_rdma_control(SVCXPRT *xprt, const u_int32_t rq, void *in)
{
}

static int
svc_compose_rpcmsg(SVCXPRT * xprt, CONN * conn, xdrproc_t xdr_results,
    caddr_t xdr_location, rdma_buf_t *rpcreply, XDR ** xdrs,
    struct rpc_msg *msg, bool_t has_args, u_int32_t *len,struct clist *wcl)
{
	PRINTF_INFO("begin %s~\n",__func__);
	/* MODIFIED by dwyane @ 2011-01-11 */
	RDMA_SVCXPRT *rxprt;

	rxprt = list_entry(xprt, RDMA_SVCXPRT, xprt);
	/* END */
	
	/*
	 * Get a pre-allocated buffer for rpc reply
	 */
	rpcreply->type = SEND_BUFFER;
	if (rdma_buf_alloc(conn, rpcreply)) {
		//DTRACE_PROBE(krpc__e__svcrdma__rpcmsg__reply__nofreebufs);
		return (SVC_RDMA_FAIL);
	}

	//xdrrdma_create(*xdrs, rpcreply->addr, rpcreply->len,0, NULL, XDR_ENCODE, conn);
	xdrrdma_create_svc(*xdrs, rpcreply->addr, rpcreply->len,
	    RDMA_MINCHUNK, NULL,wcl, XDR_ENCODE, conn);
	printf("x_handy is %d,in %s:%d\n",(*xdrs)->x_handy,__func__,__LINE__);
	msg->rm_xid = rxprt->dw_xid;
	PRINTF_INFO("xid is %d~\n",msg->rm_xid);
	
	if (has_args) {
		//PRINTF_INFO("has_args\n");
		if (xdr_replymsg(*xdrs, msg)){ //ming 
			printf("x_handy is %d,in %s:%d\n",(*xdrs)->x_handy,__func__,__LINE__);
			PRINTF_INFO("begin encode return result\n");
			if( 
#if 1		
			!(*xdr_results)(*xdrs, xdr_location))
#else
		    SVCAUTH_WRAP(&clone_xprt->xp_auth, *xdrs,
		    xdr_results, xdr_location))))
#endif
		    {
				rdma_buf_free(conn, rpcreply);
				//DTRACE_PROBE(
				    //krpc__e__svcrdma__rpcmsg__reply__authwrap1);
				return (SVC_RDMA_FAIL);
			}
		}	
	} else {
		if (!xdr_replymsg(*xdrs, msg)) {
			rdma_buf_free(conn, rpcreply);
			//DTRACE_PROBE(
			    //krpc__e__svcrdma__rpcmsg__reply__authwrap2);
			return (SVC_RDMA_FAIL);
		}
	}

	*len = XDR_GETPOS(*xdrs);

	PRINTF_INFO("end %s~len is %d\n",__func__,*len);
	return (SVC_RDMA_SUCCESS);
}

/* END */

int
svc_setup_rlist(CONN *conn, XDR *xdrs, XDR *call_xdrp,int* flag)
{
	int status;
	struct clist *rclp;
	int32_t xdr_flag = XDR_RDMA_RLIST_REG;

	XDR_CONTROL(call_xdrp, XDR_RDMA_GET_RLIST, &rclp);
    PRINTF_INFO("in %s, pointer of clist is %p\n", __func__, rclp);

	if (rclp != NULL) {
		*flag = 1;
		status = clist_register(conn, rclp, CLIST_REG_SOURCE);

		if (status != RDMA_SUCCESS) {
                        PRINTF_ERR("in %s, return CLNT_RDMA_FAIL\n", __func__);
                        return (CLNT_RDMA_FAIL);
                }
                XDR_CONTROL(call_xdrp, XDR_RDMA_SET_FLAGS, &xdr_flag);
        }else
			*flag = 0;
        (void) xdr_do_clist(xdrs, &rclp);
        return (CLNT_RDMA_SUCCESS);
}


