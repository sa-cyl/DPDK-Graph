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
 * clnt_tcp.c, Implements a TCP/IP based, client side RPC.
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 *
 * TCP based RPC supports 'batched calls'.
 * A sequence of calls may be batched-up in a send buffer.  The rpc call
 * return immediately to the client even though the call was not necessarily
 * sent.  The batching occurs if the results' xdr routine is NULL (0) AND
 * the rpc timeout value is zero (see clnt.h, rpc).
 *
 * Clients should NOT casually batch calls that in fact return results; that is,
 * the server side should be aware that a call is batched and not produce any
 * return message.  Batched calls that produce many result messages can
 * deadlock (netlock) the client and the server....
 *
 * Now go hang yourself.
 */
#include <pthread.h>

#include <reentrant.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <sys/syslog.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <rpc/rpc.h>
#include "rpc_com.h"
#include <rpc/rpc_rdma.h>

/* MODIFIED by dwyane @ 2011-01-12 */
#include "dwyane.h"
/* END */

extern mutex_t  clnt_rdma_lock;

/* MODIFIED by dwyane @ 2010-12-21 */
#define DATA_READ 2
enum {
	RESOLVE_TIMEOUT_MS	= 5000,
};


struct data_req {
        char c;
        u_long len;
};

#define	CKU_HDRSIZE	20

typedef struct __rpc_rdma_client {
	CLIENT 						clnt;
	int		busy_flag;
	pthread_cond_t busy_cond;
	struct rdma_cm_id 			*cm_id;
	struct ibv_pd 				*pd;
	struct ibv_comp_channel	       *comp_chan;
	XDR							dw_outxdr;	/* xdr stream for output */
	u_int32_t						dw_outsz;
	XDR							dw_inxdr;	/* xdr stream for input */
        //XDR                                                     dw_replyxdr;
	char							dw_rpchdr[DW_HDRSIZE+4]; /* rpc header */
	u_int32_t						dw_xid;	/* current XID */
	struct rpc_err				dw_err;	/* error status */
	dw_bufpool_t					*send_pool;
	dw_bufpool_t  				*recv_pool;
    //dw_bufpool_t                            *longrpc_pool;
}RDMA_CLIENT;
/* END */

void release_clnt_lock(RDMA_CLIENT * rclnt){
	pthread_mutex_lock(&clnt_rdma_lock);
	rclnt->busy_flag= 0;
	pthread_mutex_unlock(&clnt_rdma_lock);
	pthread_cond_signal(&rclnt->busy_cond);
}


/* MODIFIED by dwyane @ 2010-12-21 */
int prepare_rdma_clnt(const char*, RDMA_CLIENT*);
static enum clnt_stat clnt_rdma_call(CLIENT *, rpcproc_t, xdrproc_t, void *,
    xdrproc_t, void *, struct timeval);
static void clnt_rdma_geterr(CLIENT *, struct rpc_err *);
static bool_t clnt_rdma_freeres(CLIENT *, xdrproc_t, void *);
static void clnt_rdma_abort(CLIENT *);
static bool_t clnt_rdma_control(CLIENT *, u_int, void *);
static void clnt_rdma_destroy(CLIENT *);
static struct clnt_ops *clnt_rdma_ops(void);
int
clnt_compose_rpcmsg(CLIENT *clnt, rpcproc_t procnum,
    rdma_buf_t *rpcmsg, XDR *xdrs,
    xdrproc_t xdr_args, void* argsp);
int
clnt_compose_rdma_header(CONN *conn, CLIENT *clnt, rdma_buf_t *clmsg,
    XDR **xdrs, u_int32_t *op);
static void clnt_decode_long_reply(CONN *, struct clist *,
		struct clist *, XDR *, XDR **, struct clist *,struct clist *,
		struct clist *, u_int32_t, u_int32_t);

int
clnt_setup_rlist(CONN *conn, XDR *xdrs, XDR *call_xdrp);


/* END */

/* MODIFIED by dwyane @ 2010-12-30 */
u_int32_t
calc_length(u_int32_t len)
{
	len = RNDUP(len);

	if (len <= 64 * 1024) {
		if (len > 32 * 1024) {
			len = 64 * 1024;
		} else {
			if (len > 16 * 1024) {
				len = 32 * 1024;
			} else {
				if (len > 8 * 1024) {
					len = 16 * 1024;
				} else {
					len = 8 * 1024;
				}
			}
		}
	}
	return (len);
}
/* END */

/* MODIFIED by dwyane @ 2010-12-21 */
CLIENT *
clnt_rdma_create(hostname, prog, vers)
	const char *hostname;
	const rpcprog_t prog;			/* program number */
	const rpcvers_t vers;			/* version number */
	
{
	PRINTF_INFO("%s\n", __func__);
	
	RDMA_CLIENT *rclnt;
	struct rpc_msg call_msg;
        int32_t *buf;

	int 	status;
	CONN *conn;
	
	rclnt = mem_alloc(sizeof(RDMA_CLIENT));
	if(prepare_rdma_clnt(hostname, rclnt)){
		PRINTF_DEBUG("some err happen while prepare_rdma_clnt\n");
		mem_free(rclnt, sizeof(RDMA_CLIENT));
		return NULL;
	}

	/* call message, just used to pre-serialize below */
	call_msg.rm_xid = 0;
	call_msg.rm_direction = CALL;
	call_msg.rm_call.cb_rpcvers = RPC_MSG_VERSION;
	call_msg.rm_call.cb_prog = prog;
	call_msg.rm_call.cb_vers = vers;
    //buf=(int32_t *)&call_msg;
    PRINTF_DEBUG("%s:%d buf[0]:%ld buf[1]:%ld buf[2]:%ld buf[3]:%ld buf[4]:%ld buf[5]:%ld\n",__func__,__LINE__,buf[0],buf[1],buf[2],buf[3],buf[4],buf[5]);
	xdrmem_create(&rclnt->dw_outxdr, rclnt->dw_rpchdr, CKU_HDRSIZE, XDR_ENCODE);
	/* pre-serialize call message header */
	if (!xdr_callhdr(&rclnt->dw_outxdr, &call_msg)) {
		XDR_DESTROY(&rclnt->dw_outxdr);
		auth_destroy(rclnt->clnt.cl_auth);
		mem_free(rclnt, sizeof (RDMA_CLIENT));
		return (NULL);
	}

	rclnt->recv_pool = dw_rbufpool_create(rclnt->pd, RECV_BUFFER, 32);
	rclnt->send_pool = dw_rbufpool_create(rclnt->pd, SEND_BUFFER, 32);
	//rclnt->longrpc_pool = dw_rbufpool_create(rclnt->pd, RDMA_LONG_BUFFER, 32);
	
	if(rclnt->dw_xid == 0)
		rclnt->dw_xid = 39438;
	#if 0 //ming 2011-9-26
	conn = alloc_conn(rclnt->cm_id, rclnt->pd, rclnt->comp_chan, rclnt->send_pool, rclnt->recv_pool);

	
	status = rdma_clnt_postrecv(conn, rclnt->dw_xid);//rib_clnt_post
	if (status != RDMA_SUCCESS) {
		//rdma_buf_free(conn, &clmsg);
		rclnt->dw_err.re_status = RPC_CANTSEND;
		rclnt->dw_err.re_errno = EIO;
		//goto done;
		PRINTF_INFO("%s:rdma_clnt_postrecv\n",__func__);
		return status;
	}
	#endif
	pthread_cond_init(&rclnt->busy_cond,NULL);
	return &rclnt->clnt;
}

int
prepare_rdma_clnt(const char *hostname, RDMA_CLIENT *rclnt)	
{
	struct rdma_event_channel      	*cm_channel;
	struct rdma_cm_id	       	*cm_id;
	struct rdma_cm_event	       	*event;
	struct rdma_conn_param		conn_param = { };

	struct ibv_comp_channel	       *comp_chan;

	struct ibv_pd		       		*pd;
	struct ibv_cq		       		*send_cq;
	struct ibv_cq		       		*recv_cq;

	struct ibv_qp_init_attr			qp_attr = { };

	struct addrinfo		       	*res, *t;
	struct addrinfo				hints = {
		.ai_family   = AF_INET,
		.ai_socktype = SOCK_STREAM
	};
	int							n, err;

	/* Set up RDMA CM structures */

	cm_channel = rdma_create_event_channel();
	if (!cm_channel)
		return 1;

	err = rdma_create_id(cm_channel, &cm_id, NULL, RDMA_PS_TCP);
	if (err)
		return err;

	n = getaddrinfo(hostname, "37549", &hints, &res);
	if (n < 0)
		return 1;

	/* Resolve server address and route */

	for (t = res; t; t = t->ai_next) {
		err = rdma_resolve_addr(cm_id, NULL, t->ai_addr,
					RESOLVE_TIMEOUT_MS);
		if (!err)
			break;
	}
	if (err)
		return err; 

	err = rdma_get_cm_event(cm_channel, &event);
	if (err)
		return err;

	if (event->event != RDMA_CM_EVENT_ADDR_RESOLVED)
		return 1;

	rdma_ack_cm_event(event);

	err = rdma_resolve_route(cm_id, RESOLVE_TIMEOUT_MS);
	if (err)
		return err; 

	err = rdma_get_cm_event(cm_channel, &event);
	if (err)
		return err;

	if (event->event != RDMA_CM_EVENT_ROUTE_RESOLVED)
		return 1;

	rdma_ack_cm_event(event);

	/* Create verbs objects now that we know which device to use */

	pd = ibv_alloc_pd(cm_id->verbs);
	if (!pd)
		return 1;

	comp_chan = ibv_create_comp_channel(cm_id->verbs);
	if (!comp_chan)
		return 1;

	send_cq = ibv_create_cq(cm_id->verbs, 2, NULL, comp_chan, 0);
	if (!send_cq)
		return 1;

	if (ibv_req_notify_cq(send_cq, 0))
		return 1;

	recv_cq = ibv_create_cq(cm_id->verbs, 2, NULL, comp_chan, 0);
	if (!recv_cq)
		return 1;

	if (ibv_req_notify_cq(recv_cq, 0))
		return 1;

	qp_attr.cap.max_send_wr	 = CLNT_MAX_SEND_WR;
	qp_attr.cap.max_send_sge = CLNT_MAX_SEND_SGE;
	qp_attr.cap.max_recv_wr	 = CLNT_MAX_RECV_WR;
	qp_attr.cap.max_recv_sge = CLNT_MAX_RECV_SGE;

	qp_attr.send_cq		 = send_cq;
	qp_attr.recv_cq		 = recv_cq;

	qp_attr.qp_type		 = IBV_QPT_RC;

	err = rdma_create_qp(cm_id, pd, &qp_attr);
	if (err)
		return err;

	conn_param.initiator_depth 	   		= 1;
	conn_param.responder_resources 	= 1;
	conn_param.retry_count	   		= 7;

	/* Connect to server */

	err = rdma_connect(cm_id, &conn_param);
	if (err)
		return err;
	//printf("%s:connect success~\n",__func__);
	
	err = rdma_get_cm_event(cm_channel, &event);
	if (err)
		return err;

	if (event->event != RDMA_CM_EVENT_ESTABLISHED)
		return 1;

	PRINTF_INFO("get established\n");
	rdma_ack_cm_event(event);

	rclnt->cm_id = cm_id;
	rclnt->pd = pd;
	rclnt->comp_chan = comp_chan;

	rclnt->clnt.cl_auth = authnone_create();
	rclnt->clnt.cl_netid = strdup("rdma");
	rclnt->clnt.cl_ops = clnt_rdma_ops();
	rclnt->clnt.cl_tp = NULL;
	rclnt->clnt.cl_private = rclnt;

	return 0;
}

static int
clnt_setup_long_reply(CONN *conn, struct clist **clpp, u_int32_t length)
{
	if (length == 0) {
		*clpp = NULL;
		return (CLNT_RDMA_SUCCESS);
	}
#if 0
	*clpp = clist_alloc();

	(*clpp)->rb_longbuf.len = calc_length(length);
	(*clpp)->rb_longbuf.type = RDMA_LONG_BUFFER;

	if (rdma_buf_alloc(conn, &((*clpp)->rb_longbuf))) {
		clist_free(*clpp);
		*clpp = NULL;
		return (CLNT_RDMA_FAIL);
	}

	(*clpp)->u.c_daddr3 = (*clpp)->rb_longbuf.addr;
	(*clpp)->c_len = (*clpp)->rb_longbuf.len;
	(*clpp)->c_next = NULL;
	(*clpp)->c_dmemhandle = (*clpp)->rb_longbuf.handle;
         //printf("func:%s   line:%d     daddr3:%ld c_len:%d\n",__func__,__LINE__,(*clpp)->u.c_daddr3,(*clpp)->c_len);
        //output_detail((*clpp)->c_dmemhandle,1024);

	if (clist_register(conn, *clpp, CLIST_REG_DST)) {
		rdma_buf_free(conn, &((*clpp)->rb_longbuf));
		clist_free(*clpp);
		*clpp = NULL;
		return (CLNT_RDMA_FAIL);
	}

	return (CLNT_RDMA_SUCCESS);
#endif
}



static enum clnt_stat
clnt_rdma_call(clnt, proc, xdr_args, args_ptr, xdr_results, results_ptr, timeout)
	CLIENT *clnt;
	rpcproc_t proc;
	xdrproc_t xdr_args;
	void *args_ptr;
	xdrproc_t xdr_results;
	void *results_ptr;
	struct timeval timeout;
{

	int 	try_call_again;
	int 	status;
	int 	msglen;
	int 	ack_flag = 0;
	
	XDR	*call_xdrp, callxdr; /* for xdrrdma encoding the RPC call */
	XDR	*reply_xdrp, replyxdr; /* for xdrrdma decoding the RPC reply */
	XDR 	*rdmahdr_o_xdrs, *rdmahdr_i_xdrs;

	struct rpc_msg 	reply_msg;

	struct clist *cl_sendlist=NULL;
	struct clist *cl_recvlist=NULL;
	struct clist *cl=NULL;
	struct clist *cl_rpcmsg=NULL;
	struct clist *cl_rdma_reply=NULL;
	struct clist *cl_rpcreply_wlist=NULL;
    struct clist *cl_long_reply=NULL;	
    struct clist *cl_wcl=NULL;

	//unsigned long used_time;
	//struct timeval tim1, tim2, tim3, tim4;
	
	u_int32_t vers;
	u_int32_t op;
	u_int32_t off;
	u_int32_t seg_array_len;
	u_int32_t long_reply_len;
	u_int32_t	rdma_header_len;
	rdma_buf_t clmsg;
	rdma_buf_t rpcmsg;


	rdma_chunkinfo_lengths_t rcil;

	bool_t wlist_exists_reply;

	u_int32_t rdma_credit = 43;

	RDMA_CLIENT *rclnt;
	CONN *conn;
	rclnt = (RDMA_CLIENT *)clnt->cl_private;
	{
		/*************************************
		*section 1
		*************************************/
		//gettimeofday(&tim1,NULL);

		pthread_mutex_lock(&clnt_rdma_lock);
		while (rclnt->busy_flag)
			pthread_cond_wait(&rclnt->busy_cond, &clnt_rdma_lock);
		rclnt->busy_flag = 1;
		pthread_mutex_unlock(&clnt_rdma_lock);
			
		conn = alloc_conn(rclnt->cm_id, rclnt->pd, rclnt->comp_chan, rclnt->send_pool, rclnt->recv_pool);

		//gettimeofday(&tim2,NULL);
		//used_time = (tim2.tv_sec-tim1.tv_sec)*1000000.0 + (tim2.tv_usec-tim1.tv_usec); 
		//printf("used_time1 is %ul,In %s: %d\n",used_time,__func__,__LINE__);
		(void) xdrrdma_prepare_wlist(xdr_results, results_ptr,&cl_wcl);
	}
call_again:
	{
		/*************************************
	 	*section 2
	 	*************************************/
		bzero(&clmsg, sizeof (clmsg));
		bzero(&rpcmsg, sizeof (rpcmsg));

		try_call_again = 0;
		cl_sendlist = NULL;
		cl_recvlist = NULL;
		cl = NULL;
		cl_rpcmsg = NULL;
		cl_rdma_reply = NULL;
		call_xdrp = NULL;
		reply_xdrp = NULL;
		wlist_exists_reply  = FALSE;
		cl_rpcreply_wlist = NULL;
		rcil.rcil_len = 0;
		rcil.rcil_len_alt = 0;
	        
		msglen = 0;
		if(rdma_buf_alloc(conn, &rpcmsg)){
			goto done;
		}

		/* First try to encode into regular send buffer */
		op = RDMA_MSG;

		call_xdrp = &callxdr;

		xdrrdma_create(call_xdrp, rpcmsg.addr, rpcmsg.len,
			    RDMA_MINCHUNK, NULL, NULL, XDR_ENCODE, conn
			    );


		status = clnt_compose_rpcmsg(clnt, proc, &rpcmsg, call_xdrp,
			    xdr_args, args_ptr);

		if (status != CLNT_RDMA_SUCCESS) {
			rdma_buf_free(conn, &rpcmsg);
			XDR_DESTROY(call_xdrp);
		} else {
			XDR_CONTROL(call_xdrp, XDR_RDMA_GET_CHUNK_LEN, &rcil);//
		}
	    PRINTF_DEBUG("func:%s  msglen:%d\n",__func__,xdr_sizeof(xdr_args, args_ptr));
#if 0
		if (status != CLNT_RDMA_SUCCESS) {
	                msglen +=DW_HDRSIZE + BYTES_PER_XDR_UNIT + MAX_AUTH_BYTES;                

			msglen = calc_length(msglen);

			/* MODIFIED by dwyane @ 2010-12-30 */
			PRINTF_INFO("msglen is %d\n", msglen);
			/* END */
	        PRINTF_DEBUG("func:%s line:%d             msglen:%d\n",__func__,__LINE__,msglen);
			/* pick up the lengths for the reply buffer needed */
			(void) xdrrdma_sizeof(xdr_args, args_ptr, 0,
			    &rcil.rcil_len, &rcil.rcil_len_alt);
	        PRINTF_DEBUG("func:%s line:%d             rcil.rcil_len:%d\n",__func__,__LINE__,rcil.rcil_len);
			/*
			 * Construct a clist to describe the CHUNK_BUFFER
			 * for the rpcmsg.
			 */
			cl_rpcmsg = clist_alloc();
			cl_rpcmsg->c_len = msglen;
			cl_rpcmsg->rb_longbuf.type = RDMA_LONG_BUFFER;
			cl_rpcmsg->rb_longbuf.len = msglen;
			if (rdma_buf_alloc(conn, &cl_rpcmsg->rb_longbuf)) {
				clist_free(cl_rpcmsg);
				cl_rpcmsg=NULL;
				goto done;
			}
			cl_rpcmsg->w.c_saddr3 = cl_rpcmsg->rb_longbuf.addr;

			op = RDMA_NOMSG;
			call_xdrp = &callxdr;

			xdrrdma_create(call_xdrp, cl_rpcmsg->rb_longbuf.addr,
			    cl_rpcmsg->rb_longbuf.len, 0,
			    cl_rpcmsg,NULL, XDR_ENCODE, conn);
			status = clnt_compose_rpcmsg(clnt, proc, &cl_rpcmsg->rb_longbuf, call_xdrp,
			    xdr_args, args_ptr);
	                rcil.rcil_len=msglen;
			if (status != CLNT_RDMA_SUCCESS) {
				PRINTF_ERR("%s:clnt_compose_rpcmsg status err\n",__func__);
				rclnt->dw_err.re_status = RPC_CANTENCODEARGS;
				rclnt->dw_err.re_errno = EIO;
				goto done;
			}
		}      
#endif
		/*
		 * Prepare the RDMA header. On success xdrs will hold the result
		 * of xdrmem_create() for a SEND_BUFFER.
		 */
		status = clnt_compose_rdma_header(conn, clnt, &clmsg,
		    &rdmahdr_o_xdrs, &op);
		if (status != CLNT_RDMA_SUCCESS) {
			PRINTF_ERR("!!%s:clnt_compose_rdma_header status err\n",__func__);
			rclnt->dw_err.re_status = RPC_CANTSEND;
			rclnt->dw_err.re_errno = EIO;
			goto done;
		}
		/*
		 * Now insert the RDMA READ list iff present
		 */
		status = clnt_setup_rlist(conn, rdmahdr_o_xdrs, call_xdrp);
		if (status != CLNT_RDMA_SUCCESS) {
			PRINTF_ERR("!!%s:clnt_setup_rlist status err\n",__func__);
			rclnt->dw_err.re_status = RPC_CANTSEND;
			rclnt->dw_err.re_errno = EIO;
			goto done;
		}

		/*
		 * Setup RDMA WRITE chunk list for nfs read operation
		 * other operations will have a NULL which will result
		 * as a NULL list in the XDR stream.
		 */
		xdr_encode_wlist(conn,rdmahdr_o_xdrs, cl_wcl,CLIST_REG_DST);
		/*if (status != CLNT_RDMA_SUCCESS) {
			PRINTF_ERR("!!%s:xdr_encode_wlist status err\n",__func__);
			rdma_buf_free(conn, &clmsg);
			rclnt->dw_err.re_status = RPC_CANTSEND;
			rclnt->dw_err.re_errno = EIO;
			goto done;
		}*/
		long_reply_len=rcil.rcil_len;
	    PRINTF_DEBUG("func:%s line:%d     xdr pos is %d   long_reply_len:%d\n",__func__,__LINE__, XDR_GETPOS(rdmahdr_o_xdrs),long_reply_len);	
		/*
		 * XDR encode the RDMA_REPLY write chunk
		 */
	    status = clnt_setup_long_reply(conn,&cl_long_reply,long_reply_len);
	        if (status != CLNT_RDMA_SUCCESS) {
			rdma_buf_free(conn, &clmsg);

			goto done;
		}
	    seg_array_len = (cl_long_reply ? 1 : 0);
		(void) xdr_encode_reply_wchunk(rdmahdr_o_xdrs, cl_long_reply,seg_array_len);

		/*
		 * Construct a clist in "sendlist" that represents what we
		 * will push over the wire.
		 *
		 * Start with the RDMA header and clist (if any)
		 */
		 /* MODIFIED by dwyane @ 2011-01-05 */

		rdma_header_len = XDR_GETPOS(rdmahdr_o_xdrs);
		clist_add(&cl_sendlist, 0, rdma_header_len, &clmsg.handle,
		    clmsg.addr, NULL, NULL);

		/*
		 * Put the RPC call message in  sendlist if small RPC
		 */
		if (op == RDMA_MSG) {
			clist_add(&cl_sendlist, 0, rclnt->dw_outsz, &rpcmsg.handle,
			    rpcmsg.addr, NULL, NULL);
		} else {//ming /*what about long rpc?*/
		}
		//gettimeofday(&tim1,NULL);
		//used_time = (tim1.tv_sec-tim2.tv_sec)*1000000.0 + (tim1.tv_usec-tim2.tv_usec); 
		//printf("used_time2 is %ul,In %s: %d\n",used_time,__func__,__LINE__);
	}
	/*
	 * Set up a reply buffer ready for the reply
	 */
	{
		/*************************************
		 *section 3
		 *************************************/
		status = rdma_clnt_postrecv(conn, rclnt->dw_xid);//rib_clnt_post
		//gettimeofday(&tim2,NULL);
		//used_time = (tim2.tv_sec-tim1.tv_sec)*1000000.0 + (tim2.tv_usec-tim1.tv_usec); 
		//printf("used_time3 is %ul,In %s: %d\n",used_time,__func__,__LINE__);
	}
	{
		/*************************************
		 *section 4
		 **************************************/
		if (status != RDMA_SUCCESS) {
			PRINTF_ERR("!!%s:rdma_clnt_postrecv status err\n",__func__);
			rdma_buf_free(conn, &clmsg);
			rclnt->dw_err.re_status = RPC_CANTSEND;
			rclnt->dw_err.re_errno = EIO;
			goto done;
		}
		/*
		 * Send the RDMA Header and RPC call message to the server
		 */
		status = dw_send(conn, cl_sendlist, rclnt->dw_xid);//rib_send_and_wait(conn, cl, msgid, 1, 1, &wd);
		if (status != RDMA_SUCCESS) {
			PRINTF_ERR("!!%s:dw_send status err\n",__func__);
			rclnt->dw_err.re_status = RPC_CANTSEND;
			rclnt->dw_err.re_errno = EIO;
			goto done;
		}
		//gettimeofday(&tim1,NULL);
		//used_time = (tim1.tv_sec-tim2.tv_sec)*1000000.0 + (tim1.tv_usec-tim2.tv_usec); 
		//printf("used_time4 is %ul,In %s: %d\n",used_time,__func__,__LINE__);
	}
	/*
	 * Recv rpc reply
	 */
	{
		/*************************************
		 *section 5
		 **************************************/
		status = dw_recv(conn, rclnt->dw_xid);
		//gettimeofday(&tim2,NULL);
		//used_time = (tim2.tv_sec-tim1.tv_sec)*1000000.0 + (tim2.tv_usec-tim1.tv_usec); 
		//printf("used_time5 is %ul,In %s: %d\n",used_time,__func__,__LINE__);
	}
	{
		/*************************************
		 *section 6
		 **************************************/
		/*
		 * Now check recv status
		 */
		if (status != 0) {
			PRINTF_ERR("!!%s:dw_recv status err\n",__func__);
			if (status == RDMA_INTR) {
				rclnt->dw_err.re_status = RPC_INTR;
				rclnt->dw_err.re_errno = EINTR;
			} else if (status == RPC_TIMEDOUT) {
				rclnt->dw_err.re_status = RPC_TIMEDOUT;
				rclnt->dw_err.re_errno = ETIMEDOUT;
			} else {
				rclnt->dw_err.re_status = RPC_CANTRECV;
				rclnt->dw_err.re_errno = EIO;
			}
			goto done;
		}
		/*
		 * Process the reply message.
		 *
		 * First the chunk list (if any)
		 */
		
		rdmahdr_i_xdrs = &(rclnt->dw_inxdr);
		xdrmem_create(rdmahdr_i_xdrs,
		    (uintptr_t)conn->to_be_done1->data,
		    conn->to_be_done1->length, XDR_DECODE);
		/*
		 * Treat xid as opaque (xid is the first entity
		 * in the rpc rdma message).
		 * Skip xid and set the xdr position accordingly.
		 */
		u_int32_t xid = *(uint32_t *)rdmahdr_i_xdrs->x_base;
		XDR_SETPOS(rdmahdr_i_xdrs, sizeof (u_int32_t));
		(void) xdr_u_int(rdmahdr_i_xdrs, &vers);
		(void) xdr_u_int(rdmahdr_i_xdrs, &rdma_credit);
		(void) xdr_u_int(rdmahdr_i_xdrs, &op);
		(void) xdr_do_clist(rdmahdr_i_xdrs, &cl);
		if(cl)
			ack_flag = 1;
		
		PRINTF_DEBUG("%s:xid:vers:credit:res_op~%d:%d:%d:%d\n",__func__,
				xid,vers,rdma_credit,op);


		wlist_exists_reply = FALSE;
		if (! xdr_decode_wlist(rdmahdr_i_xdrs, &cl_rpcreply_wlist,
		    &wlist_exists_reply)) {
			PRINTF_ERR("!!%s:xdr_decode_wlist status err\n",__func__);
			rclnt->dw_err.re_status = RPC_CANTDECODERES;
			rclnt->dw_err.re_errno = EIO;
			goto done;
		}
		/*
		 * The server shouldn't have sent a RDMA_SEND that
		 * the client needs to RDMA_WRITE a reply back to
		 * the server.  So silently ignoring what the
		 * server returns in the rdma_reply section of the
		 * header.
		 */
		(void) xdr_decode_reply_wchunk(rdmahdr_i_xdrs, &cl_rdma_reply);
		off = xdr_getpos(rdmahdr_i_xdrs);
		PRINTF_INFO("after rdma header, off is %d\n", off);
	    PRINTF_DEBUG("func:%s line:%d       cl_rdma_reply->c_len:%d\n",__func__,__LINE__,cl_rdma_reply->c_len);
		clnt_decode_long_reply(conn,cl_long_reply,
		    cl_rdma_reply, &replyxdr, &reply_xdrp,
		    cl, NULL, cl_recvlist, op, off);
		if (reply_xdrp == NULL){
			PRINTF_ERR("reply_xdrp is NULL, return\n");
			goto done;
		}

		if (wlist_exists_reply) {
			XDR_CONTROL(reply_xdrp, XDR_RDMA_SET_WLIST, cl_rpcreply_wlist);
		}

		reply_msg.rm_direction = REPLY;
		reply_msg.rm_reply.rp_stat = MSG_ACCEPTED;
		reply_msg.acpted_rply.ar_stat = SUCCESS;
		reply_msg.acpted_rply.ar_verf = _null_auth;

		/*
		 *  xdr_results will be done in AUTH_UNWRAP.
		 */
		reply_msg.acpted_rply.ar_results.where = NULL;
		reply_msg.acpted_rply.ar_results.proc = xdr_void;
		/*
		 * Decode and validate the response.
		 */
		PRINTF_INFO("%s: rm_direction is %d\n",__func__,reply_msg.rm_direction);
		if (xdr_replymsg(reply_xdrp, &reply_msg)) {
			
			PRINTF_INFO("%s:xdr_replymsg success~\n",__func__);
			enum clnt_stat re_status;

			_seterr_reply(&reply_msg, &(rclnt->dw_err));

			re_status = rclnt->dw_err.re_status;
			if (re_status == RPC_SUCCESS) {
				(*xdr_results)(reply_xdrp, results_ptr);
			}
			else {
			}
		} else {
			PRINTF_ERR("!!%s:xdr_replymsg err\n",__func__);
			rclnt->dw_err.re_status = RPC_CANTDECODERES;
			rclnt->dw_err.re_errno = EIO;
		}

	done:
		rclnt->dw_xid--;//yh:4-21
		clist_deregister(cl_wcl);
		clist_free(cl_wcl);
		if (cl_sendlist != NULL)
		{
			/* MODIFIED by dwyane @ 2011-01-13 */

			/* END */
			clist_free(cl_sendlist);
			cl_sendlist=NULL;
		}
		rdma_buf_free(conn, &rpcmsg);
		rdma_buf_free(conn, &clmsg);

		if (cl_long_reply) {//should not happen
			(void) clist_deregister(cl_long_reply);
			rdma_buf_free(conn, &cl_long_reply->rb_longbuf);
			clist_free(cl_long_reply);
			cl_long_reply =NULL;
		}

		if (call_xdrp)
			XDR_DESTROY(call_xdrp);

		if (cl_rdma_reply) {
			clist_free(cl_rdma_reply);
			cl_rdma_reply = NULL;
		}

		if (cl_recvlist) {
			rdma_buf_t	recvmsg = {0};
			recvmsg.addr = (caddr_t)(uintptr_t)cl_recvlist->w.c_saddr3;
			recvmsg.type = RECV_BUFFER;
			rdma_buf_free(conn, &recvmsg);
			clist_free(cl_recvlist);
			cl_recvlist = NULL;
		}

		if (try_call_again)
			goto call_again;

		/* MODIFIED by dwyane @ 2011-01-13 */
		PRINTF_INFO("before ,bfree is %d\n", conn->recv_pool->bpool->buffree);
		dw_rbuf_free(conn, RECV_BUFFER, &conn->to_be_done1->data);
		PRINTF_INFO("after ,bfree is %d\n", conn->recv_pool->bpool->buffree);
		/* END */
		release_clnt_lock((RDMA_CLIENT *)clnt->cl_private);

		if (rclnt->dw_err.re_status != RPC_SUCCESS) {
			PRINTF_ERR("return err in %s:%d\n",__func__,__LINE__);
		}
		//gettimeofday(&tim1,NULL);
		//used_time = (tim1.tv_sec-tim2.tv_sec)*1000000.0 + (tim1.tv_usec-tim2.tv_usec); 
		//printf("used_time6 is %ul,In %s: %d\n",used_time,__func__,__LINE__);
	}
	return (rclnt->dw_err.re_status);
}

static void
clnt_rdma_geterr(cl, errp)
	CLIENT *cl;
	struct rpc_err *errp;
{
}

static bool_t
clnt_rdma_freeres(cl, xdr_res, res_ptr)
	CLIENT *cl;
	xdrproc_t xdr_res;
	void *res_ptr;
{
}

static void
clnt_rdma_abort(cl)
	CLIENT *cl;
{
}

static bool_t
clnt_rdma_control(cl, request, info)
	CLIENT *cl;
	u_int request;
	void *info;
{
	
}

static void
clnt_rdma_destroy(cl)
	CLIENT *cl;
{
	RDMA_CLIENT *rclnt = (RDMA_CLIENT*) cl->cl_private;
	sigset_t mask;
	sigset_t newmask;

	PRINTF_INFO("%s\n", __func__);

	sigfillset(&newmask);
	thr_sigsetmask(SIG_SETMASK, &newmask, &mask);

	mem_free(rclnt, sizeof(RDMA_CLIENT));
	if (cl->cl_netid && cl->cl_netid[0])
		mem_free(cl->cl_netid, strlen(cl->cl_netid) +1);
	//mem_free(cl, sizeof(CLIENT));
	thr_sigsetmask(SIG_SETMASK, &(mask), NULL);

}


static struct clnt_ops *
clnt_rdma_ops()
{
	static struct clnt_ops ops;
	extern mutex_t  ops_lock;
	sigset_t mask, newmask;

	/* VARIABLES PROTECTED BY ops_lock: ops */

	sigfillset(&newmask);
	thr_sigsetmask(SIG_SETMASK, &newmask, &mask);
	mutex_lock(&ops_lock);
	if (ops.cl_call == NULL) {
		ops.cl_call = clnt_rdma_call;
		ops.cl_abort = clnt_rdma_abort;
		ops.cl_geterr = clnt_rdma_geterr;
		ops.cl_freeres = clnt_rdma_freeres;
		ops.cl_destroy = clnt_rdma_destroy;
		ops.cl_control = clnt_rdma_control;
	}
	mutex_unlock(&ops_lock);
	thr_sigsetmask(SIG_SETMASK, &(mask), NULL);
	return (&ops);
}


int
clnt_compose_rpcmsg(CLIENT *clnt, rpcproc_t procnum,
    rdma_buf_t *rpcmsg, XDR *xdrs,
    xdrproc_t xdr_args, void* argsp)
{
	RDMA_CLIENT *rclnt;
	CONN *conn;

	rclnt = (RDMA_CLIENT *)clnt->cl_private;
	
	/*
	 * Copy in the preserialized RPC header
	 * information.
	 */
	bcopy(rclnt->dw_rpchdr, rpcmsg->addr, DW_HDRSIZE);

	/*
	 * transaction id is the 1st thing in the output
	 * buffer.
	 */
	/* LINTED pointer alignment */
	//(*(uint32_t *)(rpcmsg->addr)) = rclnt->dw_xid--;
	(*(uint32_t *)(rpcmsg->addr)) = rclnt->dw_xid;
	
	//printf("%s:xid is %d\n",__func__,(*(uint32_t *)(rpcmsg->addr)));

	/* MODIFIED by dwyane @ 2010-12-26 *//*
	uint32_t *p = (uint32_t *)(rpcmsg->addr);
	*(++p) = 0;
	*(++p) = 2;
	*//* END */

	/* Skip the preserialized stuff. */
	XDR_SETPOS(xdrs, DW_HDRSIZE);//xdrrdma_setpos

	/* Serialize dynamic stuff into the output buffer. */
	if (!XDR_PUTINT32(xdrs, (int32_t *)&procnum))//xdrrdma_putint32
		return (CLNT_RDMA_FAIL);

	//PRINTF_INFO("jordan\n");
	if (!AUTH_MARSHALL(clnt->cl_auth, xdrs)) //authnone_marshal
	 	return (CLNT_RDMA_FAIL);
	if(!(*xdr_args)(xdrs, argsp)){
		return (CLNT_RDMA_FAIL);
	}
	rclnt->dw_outsz = XDR_GETPOS(xdrs);

	return (CLNT_RDMA_SUCCESS);
}

int
clnt_compose_rdma_header(CONN *conn, CLIENT *clnt, rdma_buf_t *clmsg,
    XDR **xdrs, u_int32_t *op)
{
	u_int32_t vers;
	uint32_t rdma_credit = 34;
	/* MODIFIED by dwyane @ 2011-01-11 */
	RDMA_CLIENT *rclnt;
	rclnt = (RDMA_CLIENT *)clnt->cl_private;
	/* END */

	vers = 1;
	clmsg->type = SEND_BUFFER;

	if (rdma_buf_alloc(conn, clmsg)) {
		return (CLNT_RDMA_FAIL);
	}
        //printf("func:%s line:%d     xdr pos is %d\n",__func__,__LINE__, XDR_GETPOS(*xdrs));
	*xdrs = &rclnt->dw_outxdr;
	xdrmem_create(*xdrs, clmsg->addr, clmsg->len, XDR_ENCODE);
        //printf("func:%s line:%d     xdr pos is %d\n",__func__,__LINE__, XDR_GETPOS(*xdrs));
	(*(uint32_t *)clmsg->addr) = rclnt->dw_xid;
	XDR_SETPOS(*xdrs, sizeof (uint32_t));
        //printf("func:%s line:%d     xdr pos is %d\n",__func__,__LINE__, XDR_GETPOS(*xdrs));
	(void) xdr_u_int(*xdrs, &vers);
	(void) xdr_u_int(*xdrs, &rdma_credit);
	(void) xdr_u_int(*xdrs, op);
        //printf("func:%s line:%d     xdr pos is %d\n",__func__,__LINE__, XDR_GETPOS(*xdrs));
	return (CLNT_RDMA_SUCCESS);
}



static void
clnt_decode_long_reply(CONN *conn,
    struct clist *cl_long_reply,
    struct clist *cl_rdma_reply, XDR *xdrs,
    XDR **rxdrp, struct clist *rcl,struct clist *wcl,
    struct clist *cl_recvlist,
    u_int32_t  op, u_int32_t off)
{
	if (op != RDMA_NOMSG) {
		xdrrdma_create(xdrs,
		    (caddr_t)(uintptr_t)(conn->to_be_done1->data + off),
		    conn->to_be_done1->length - off, 0, rcl,wcl, XDR_DECODE, conn);
		
		*rxdrp = xdrs;
		return;
	}
	/* op must be RDMA_NOMSG */
	if (rcl) {
		//DTRACE_PROBE(krpc__e__clntrdma__declongreply__serverreadlist);
		return;
	}
	if (cl_long_reply->u.c_daddr) {
		xdrrdma_create(xdrs, (caddr_t)cl_long_reply->u.c_daddr3,
		    cl_rdma_reply->c_len, 0, NULL,NULL, XDR_DECODE, conn);

		*rxdrp = xdrs;
	}
}


/* END */

/* MODIFIED by dwyane @ 2010-12-30 */


/*
 * If xp_cl is NULL value, then the RPC payload will NOT carry
 * an RDMA READ chunk list, in this case we insert FALSE into
 * the XDR stream. Otherwise we use the clist and RDMA register
 * the memory and encode the clist into the outbound XDR stream.
 */
int
clnt_setup_rlist(CONN *conn, XDR *xdrs, XDR *call_xdrp)
{
	int status;
	struct clist *rclp;
	int32_t xdr_flag = XDR_RDMA_RLIST_REG;

	XDR_CONTROL(call_xdrp, XDR_RDMA_GET_RLIST, &rclp);
    PRINTF_INFO("in %s, pointer of clist is %p\n", __func__, rclp);
        /* END */

        if (rclp != NULL) {
                status = clist_register(conn, rclp, CLIST_REG_SOURCE);
				
                if (status != RDMA_SUCCESS) {
                        /* MODIFIED by dwyane @ 2011-01-06 */
                        PRINTF_ERR("in %s, return CLNT_RDMA_FAIL\n", __func__);
                        /* END */
                        return (CLNT_RDMA_FAIL);
                }
                XDR_CONTROL(call_xdrp, XDR_RDMA_SET_FLAGS, &xdr_flag);
        }
        (void) xdr_do_clist(xdrs, &rclp);
        return (CLNT_RDMA_SUCCESS);
}

