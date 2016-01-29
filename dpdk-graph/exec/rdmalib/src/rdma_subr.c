/* MODIFIED by dwyane @ 2010-12-25 */
//#include <rpc/svc.h>
#include <rpc/rpc_rdma.h>
/* MODIFIED by dwyane @ 2011-01-12 */
#include "dwyane.h"
/* END */

rdma_stat
clist_deregister(struct clist *cl);

rdma_stat
rdma_svc_postrecv(CONN *conn)
{
	struct clist *cl1 = NULL,*cl2 = NULL;
	rdma_stat retval;
	rdma_buf_t rbuf1 = {0},rbuf2 = {0};
	int retry_num = 10;
	int sleep_time = 1;

	PRINTF_INFO("%s\n", __func__);
	rbuf1.type = RECV_BUFFER;
	rbuf2.type = RECV_BUFFER;
again1:
	if (rdma_buf_alloc(conn, &rbuf1)) {
		retval = RDMA_NORESOURCE;
		PRINTF_ERR("there is no resource, bfree is %d\n", conn->recv_pool->bpool->buffree);
		if(retry_num--){
			sleep(sleep_time);
			goto again1;
		}
	} else {
		clist_add(&cl1, 0, rbuf1.len, &rbuf1.handle, rbuf1.addr,
		    NULL, NULL);
		retval = dw_svc_post(conn, cl1);
	}
#if 0
again2:
	if(conn->to_be_done1->data==NULL){
		if (rdma_buf_alloc(conn, &rbuf2)) {
			retval = RDMA_NORESOURCE;
			PRINTF_ERR("there is no resource, bfree is %d\n", conn->recv_pool->bpool->buffree);
			if(retry_num--){
				sleep(sleep_time);
				goto again2;
			}
		} else {
			clist_add(&cl2, 0, rbuf2.len, &rbuf2.handle, rbuf2.addr,
			    NULL, NULL);
			retval = dw_svc_post(conn, cl2);
		}
	}
	if(conn->to_be_done1->data==NULL){
		conn->to_be_done1->data = cl1->w.c_saddr;
		conn->to_be_done1->length = cl1->c_len;
		conn->to_be_done1->data_prt = cl1->w.c_saddr;
		conn->to_be_done2->data = cl2->w.c_saddr;
		conn->to_be_done2->length = cl2->c_len;
		conn->to_be_done2->data_prt = cl2->w.c_saddr;
	}
	else{
		conn->to_be_done1->data = conn->to_be_done2->data;
		conn->to_be_done1->length = conn->to_be_done2->length;
		conn->to_be_done1->data_prt = conn->to_be_done2->data_prt;
		conn->to_be_done2->data = cl1->w.c_saddr;
		conn->to_be_done2->length = cl1->c_len;
		conn->to_be_done2->data_prt = cl1->w.c_saddr;
	}
#endif

	clist_free(cl1);
	cl1 = NULL;
	//clist_free(cl2);
	//cl2 = NULL;
	return (retval);
}

dw_bufpool_t *
dw_rbufpool_create(struct ibv_pd *pd, int ptype, int num)
{
	dw_bufpool_t	*dwbp = NULL;
	bufpool_t	*bp = NULL;
	void		*buf;
	enum ibv_access_flags access;
	struct ibv_mr_reg_arg mem_attr;

	int		i, j;
	struct ibv_mr *temp;

	dwbp = (dw_bufpool_t *)mem_alloc(sizeof (dw_bufpool_t));

	bp = (bufpool_t *)mem_alloc(sizeof (bufpool_t) + num * sizeof (void *));

	pthread_mutex_init(&bp->buflock, NULL);
	bp->numelems = num;


	switch (ptype) {
	case SEND_BUFFER:
		access = IBV_ACCESS_LOCAL_WRITE;
		bp->rsize = RPC_MSG_SZ;
		break;
	case RECV_BUFFER:
		access = IBV_ACCESS_LOCAL_WRITE;
		bp->rsize = RPC_BUF_SIZE;
		break;
	/*
        case RDMA_LONG_BUFFER:
                access = IBV_ACCESS_LOCAL_WRITE;
                bp->rsize = RPC_LONG_SIZE;
                break;*/
	default:
		goto fail;
	}

	/*
	 * Register the pool.
	 */
	bp->bufsize = num * bp->rsize;
	bp->buf = mem_alloc(bp->bufsize);//a big buffer
	dwbp->mr_hdl = (struct ibv_mr **)mem_alloc(num * sizeof (struct ibv_mr*));
	temp = dwbp->mr_hdl;
	
	for (i = 0, buf = bp->buf; i < num; i++, buf += bp->rsize) {
		//bzero(&dwbp->mr_desc[i], sizeof (struct ibv_mr_reg_arg));
		mem_attr.addr = buf;
		mem_attr.length = bp->rsize;
		mem_attr.access = access;
		dwbp->mr_hdl[i] = ibv_reg_mr(pd, mem_attr.addr, mem_attr.length, mem_attr.access);
		if (!(dwbp->mr_hdl[i])) {
			for (j = 0; j < i; j++) {
				(void) ibv_dereg_mr(dwbp->mr_hdl[i]);
			}
			goto fail;
		}
	}

	buf = bp->buf;
	for (i = 0; i < num; i++, buf += bp->rsize) {
		bp->buflist[i] = (void *)buf;
	}
	bp->buffree = num - 1;	/* no. of free buffers *///ming 2011-9-25 why num-1 not num?
	dwbp->bpool = bp;

	return (dwbp);
fail:
	if (bp) {
		if (bp->buf)
			mem_free(bp->buf, bp->bufsize);
			mem_free(bp, sizeof (bufpool_t) + num * sizeof (void *));
	}
	if (dwbp) {
		if (dwbp->mr_hdl)
			mem_free(dwbp->mr_hdl, num * sizeof (struct ibv_mr));
		mem_free(dwbp, sizeof (dw_bufpool_t));
	}
	return (NULL);
}

void *
dw_buf_alloc(CONN *conn, rdma_buf_t *rdbuf)
{
	rdma_btype	ptype = rdbuf->type;
	void		*buf;
	dw_bufpool_t	*dwbp = NULL;
	bufpool_t	*bp;
	int		i;

	/*
	 * Obtain pool address based on type of pool
	 */
	switch (ptype) {
	case SEND_BUFFER:
		dwbp = conn->send_pool;
		break;
	case RECV_BUFFER:
		dwbp = conn->recv_pool;
		break;
	/*case RDMA_LONG_BUFFER:
        dwbp = conn->longrpc_pool;
        break;*/
	default:
		return (NULL);
	}
	if (dwbp == NULL){
		printf("dwbp == NULL. In %s\n",__func__);
		return (NULL);
	}

	bp = dwbp->bpool;

	pthread_mutex_lock(&bp->buflock);
	if (bp->buffree < 0) {
		pthread_mutex_unlock(&bp->buflock);
		printf("bp->buffree < 0. In %s\n",__func__);
		return (NULL);
	}

	/* XXXX put buf, rdbuf->handle.mrc_rmr, ... in one place. */
	buf = bp->buflist[bp->buffree];
	rdbuf->addr = buf;
//	rdbuf->len = bp->rsize;
	for (i = bp->numelems - 1; i >= 0; i--) {
		if ((uintptr_t)buf == (uintptr_t)dwbp->mr_hdl[i]->addr) {
			rdbuf->handle.mrc_rmr =
			    (uint32_t)dwbp->mr_hdl[i]->rkey;
			rdbuf->handle.mrc_linfo =
			    (uintptr_t)dwbp->mr_hdl[i];
			rdbuf->handle.mrc_lmr =
			    (uint32_t)dwbp->mr_hdl[i]->lkey;
			bp->buffree--;

			pthread_mutex_unlock(&bp->buflock);

			return (buf);
		}
	}

	pthread_mutex_unlock(&bp->buflock);

	return (NULL);
}

rdma_stat
rdma_buf_alloc(CONN *conn, rdma_buf_t *rdbuf)
{
	rdbuf->addr = dw_buf_alloc(conn, rdbuf);
	if (rdbuf->addr) {
		switch (rdbuf->type) {
		case SEND_BUFFER:
			rdbuf->len = RPC_MSG_SZ;
			break;
		case RECV_BUFFER:
			rdbuf->len = RPC_BUF_SIZE; /* 2K */
			break;
        //case RDMA_LONG_BUFFER:
			//break;
		default:
			rdbuf->len = 0;
		}
		return (RDMA_SUCCESS);
	} else
		return (RDMA_FAILED);
}

void
rdma_buf_free(CONN *conn, rdma_buf_t *rbuf)
{
	if (!rbuf || rbuf->addr == NULL) {
		return;
	}
	dw_rbuf_free(conn, rbuf->type, &rbuf->addr);
	bzero(rbuf, sizeof (rdma_buf_t));
}

#if 0
/*
 * If xp_cl is NULL value, then the RPC payload will NOT carry
 * an RDMA READ chunk list, in this case we insert FALSE into
 * the XDR stream. Otherwise we use the clist and RDMA register
 * the memory and encode the clist into the outbound XDR stream.
 */
static int
clnt_setup_rlist(CONN *conn, XDR *xdrs, XDR *call_xdrp)
{
	int status;
	struct clist *rclp;
	int32_t xdr_flag = XDR_RDMA_RLIST_REG;

	XDR_CONTROL(call_xdrp, XDR_RDMA_GET_RLIST, &rclp);

	if (rclp != NULL) {
		status = clist_register(conn, rclp, CLIST_REG_SOURCE);
		if (status != RDMA_SUCCESS) {
			return (CLNT_RDMA_FAIL);
		}
		XDR_CONTROL(call_xdrp, XDR_RDMA_SET_FLAGS, &xdr_flag);
	}
	(void) xdr_do_clist(xdrs, &rclp);

	return (CLNT_RDMA_SUCCESS);
}


/*
 * If xp_wcl is NULL value, then the RPC payload will NOT carry
 * an RDMA WRITE chunk list, in this case we insert FALSE into
 * the XDR stream. Otherwise we use the clist and  RDMA register
 * the memory and encode the clist into the outbound XDR stream.
 */
static int
clnt_setup_wlist(CONN *conn, XDR *xdrs, XDR *call_xdrp, rdma_buf_t *rndbuf)
{
	int status;
	struct clist *wlist, *rndcl;
	int wlen, rndlen;
	int32_t xdr_flag = XDR_RDMA_WLIST_REG;

	XDR_CONTROL(call_xdrp, XDR_RDMA_GET_WLIST, &wlist);

	if (wlist != NULL) {
		/*
		 * If we are sending a non 4-byte alligned length
		 * the server will roundup the length to 4-byte
		 * boundary. In such a case, a trailing chunk is
		 * added to take any spill over roundup bytes.
		 */
		wlen = clist_len(wlist);
		rndlen = (roundup(wlen, BYTES_PER_XDR_UNIT) - wlen);
		if (rndlen) {
			rndcl = clist_alloc();
			/*
			 * calc_length() will allocate a PAGESIZE
			 * buffer below.
			 */
			rndcl->c_len = calc_length(rndlen);
			rndcl->rb_longbuf.type = RDMA_LONG_BUFFER;
			rndcl->rb_longbuf.len = rndcl->c_len;
			if (rdma_buf_alloc(conn, &rndcl->rb_longbuf)) {
				clist_free(rndcl);
				return (CLNT_RDMA_FAIL);
			}

			/* Roundup buffer freed back in caller */
			*rndbuf = rndcl->rb_longbuf;

			rndcl->u.c_daddr3 = rndcl->rb_longbuf.addr;
			rndcl->c_next = NULL;
			rndcl->c_dmemhandle = rndcl->rb_longbuf.handle;
			wlist->c_next = rndcl;
		}

		status = clist_register(conn, wlist, CLIST_REG_DST);
		if (status != RDMA_SUCCESS) {
			rdma_buf_free(conn, rndbuf);
			bzero(rndbuf, sizeof (rdma_buf_t));
			return (CLNT_RDMA_FAIL);
		}
		XDR_CONTROL(call_xdrp, XDR_RDMA_SET_FLAGS, &xdr_flag);
	}

	if (!xdr_encode_wlist(xdrs, wlist)) {
		if (rndlen) {
			rdma_buf_free(conn, rndbuf);
			bzero(rndbuf, sizeof (rdma_buf_t));
		}
		return (CLNT_RDMA_FAIL);
	}

	return (CLNT_RDMA_SUCCESS);
}

#endif

/*
 * Creates a new chunk list entry, and
 * adds it to the end of a chunk list.
 */
void
clist_add(struct clist **clp, uint32_t xdroff, int len,
	struct mrc *shandle, caddr_t saddr,
	struct mrc *dhandle, caddr_t daddr)
{
	struct clist *cl;

	/* Find the end of the list */

	while (*clp != NULL)
		clp = &((*clp)->c_next);

	cl = clist_alloc();
	cl->c_xdroff = xdroff;
	cl->c_len = len;
	cl->w.c_saddr = (uint64_t)(uintptr_t)saddr;
	if (shandle)
		cl->c_smemhandle = *shandle;
	cl->u.c_daddr = (uint64_t)(uintptr_t)daddr;
	if (dhandle)
		cl->c_dmemhandle = *dhandle;
	cl->c_next = NULL;

	*clp = cl;
}

struct clist *
clist_alloc(void)
{
	struct clist *clp;

	clp = mem_alloc(sizeof(struct clist));
	bzero(clp, sizeof (*clp));

	return (clp);
}

/*
 * Frees up entries in chunk list
 */
void
clist_free(struct clist *cl)
{
	struct clist *c = cl;

	while (c != NULL) {
		cl = cl->c_next;
		mem_free(c, sizeof(struct clist));
		c = cl;
	}
}

rdma_stat
rdma_clnt_postrecv(CONN *conn, uint32_t xid)
{
	struct clist *cl = NULL;
	rdma_stat retval;
	rdma_buf_t rbuf = {0};

	rbuf.type = RECV_BUFFER;
	if (rdma_buf_alloc(conn, &rbuf)) {
		printf("!!rdma_buf_alloc failure in %s\n",__func__);
		return (RDMA_NORESOURCE);
	}

	clist_add(&cl, 0, rbuf.len, &rbuf.handle, rbuf.addr,
	    NULL, NULL);
	retval = dw_clnt_post(conn, cl, xid);
	clist_free(cl);
	cl = NULL;
	if(retval!=RDMA_SUCCESS)
		printf("!!dw_clnt_post failure in %s\n",__func__);
	return (retval);
}


rdma_stat
dw_clnt_post(CONN *conn, struct clist *cl, u_int32_t msgid)
{
	struct clist	*clp = cl;

	int		nds;
	struct ibv_sge	sgl[2];
	struct ibv_recv_wr		recv_wr = { };
	struct ibv_recv_wr	       *bad_recv_wr;
	rdma_stat	ret;

	/*
	 * rdma_clnt_postrecv uses RECV_BUFFER.
	 */

	nds = 0;
	while (cl != NULL) {
		if (nds >= 2) {
			ret = RDMA_FAILED;
			goto done;
		}
		sgl[nds].addr = cl->w.c_saddr;
		sgl[nds].lkey = cl->c_smemhandle.mrc_lmr; /* lkey */
		sgl[nds].length = cl->c_len;
		cl = cl->c_next;
		nds++;
	}

	if (nds != 1) {
		ret = RDMA_FAILED;
		printf("nds != 1 in %s\n",__func__);
		goto done;
	}

	conn->to_be_done1->data = sgl[0].addr;
	recv_wr.wr_id = msgid;
	recv_wr.sg_list = sgl;
	recv_wr.num_sge = nds;

	if (ibv_post_recv(conn->cm_id->qp, &recv_wr, &bad_recv_wr)){
		printf("!!ibv_post_recv failure in %s\n",__func__);
		return 1;
	}

	return (RDMA_SUCCESS);

done:
	while (clp != NULL) {
		dw_rbuf_free(conn, RECV_BUFFER,
		    &(clp->w.c_saddr3));
		clp = clp->c_next;
	}
	return (ret);
}


rdma_stat
dw_svc_post(CONN *conn, struct clist *cl)
{
	int						nds;
	struct ibv_sge			sgl[2];
	struct ibv_recv_wr		recv_wr = { };
	struct ibv_recv_wr	       *bad_recv_wr;
	rdma_stat				ret;

	/*
	 * rdma_clnt_postrecv uses RECV_BUFFER.
	 */
	PRINTF_INFO("%s\n", __func__);
	
	nds = 0;
	while (cl != NULL) {
		if (nds >= 2) {//ming why no more than 2?
			PRINTF_ERR("nds >= 2 while dw_svc_post\n");
			return(RDMA_FAILED);
		}
		sgl[nds].addr = cl->w.c_saddr;
		sgl[nds].lkey = cl->c_smemhandle.mrc_lmr; /* lkey */
		sgl[nds].length = cl->c_len;
		cl = cl->c_next;
		nds++;
	}

	/*if (nds != 1) {
		return(RDMA_FAILED);
	}*/

	/* MODIFIED by dwyane @ 2011-01-12 */
	conn->to_be_done1->data = sgl[0].addr;
	conn->to_be_done1->length = sgl[0].length;
	conn->to_be_done1->data_prt = sgl[0].addr;
	/* END */
	recv_wr.wr_id		= sgl[0].addr;
	recv_wr.sg_list 	= sgl;
	recv_wr.num_sge 	= nds;

	if (ibv_post_recv(conn->cm_id->qp, &recv_wr, &bad_recv_wr)){
		PRINTF_ERR("err happen while dw_svc_post\n");
		return 1;
	}

	PRINTF_INFO("the post data is %p\n", recv_wr.wr_id);
	return (RDMA_SUCCESS);
}



void
dw_rbuf_free(CONN *conn, int ptype, void **buf)
{

	dw_bufpool_t *dwbp = NULL;
	bufpool_t *bp;

	/*
	 * Obtain pool address based on type of pool
	 */
	switch (ptype) {
	case SEND_BUFFER:
		dwbp = conn->send_pool;
		break;
	case RECV_BUFFER:
		dwbp = conn->recv_pool;
		break;
        /*case RDMA_LONG_BUFFER:
                dwbp = conn->longrpc_pool;
                break;*/
	default:
		return;
	}
	if (dwbp == NULL)
		return;

	bp = dwbp->bpool;

	pthread_mutex_lock(&bp->buflock);
	if (++bp->buffree >= bp->numelems) {
		/*
		 * Should never happen
		 */
		bp->buffree--;
	} else {
		bp->buflist[bp->buffree] = *buf;
	}
	*buf = NULL;
	pthread_mutex_unlock(&bp->buflock);
}


/*
 * Send buffers are freed here only in case of error in posting
 * on QP. If the post succeeded, the send buffers are freed upon
 * send completion in rib_sendwait() or in the scq_handler.
 */
rdma_stat
dw_send(CONN *conn, struct clist *cl, uint32_t msgid)
{

	struct clist	*clp;

	//rdma_stat	ret = RDMA_SUCCESS;

	int		err; 
	int		nds;

	u_int32_t		total_msg_size;

	struct ibv_cq		       	*evt_cq;
	void			       		*cq_context;

	struct ibv_sge			sgl[2];
	struct ibv_send_wr		send_wr = { };
	struct ibv_send_wr	       *bad_send_wr;
	struct ibv_wc				wc;

	nds = 0;
	total_msg_size = 0;
	clp = cl;
	while (clp != NULL) {
		if (nds >= 2) {
			PRINTF_ERR("nds >= 2.In %s %d\n", __func__,__LINE__);
			return (RDMA_FAILED);
		}
		sgl[nds].addr = clp->w.c_saddr;
		sgl[nds].lkey = clp->c_smemhandle.mrc_lmr; /* lkey */
		sgl[nds].length = clp->c_len;
		total_msg_size += clp->c_len;
		PRINTF_INFO("the length of seg%d is %d\n", nds, clp->c_len);
		clp = clp->c_next;
		nds++;
	}

	PRINTF_INFO("the total_msg_size is %d, msgid is %d\n", total_msg_size, msgid);
	send_wr.wr_id 		= msgid;
	send_wr.opcode     	= IBV_WR_SEND;
	send_wr.send_flags 	= IBV_SEND_SIGNALED;
	send_wr.sg_list    		= sgl;
	send_wr.num_sge    	= nds;

	PRINTF_INFO("nds is %d in %s\n", nds, __func__);

	if (ibv_post_send(conn->cm_id->qp, &send_wr, &bad_send_wr)){
		PRINTF_ERR("err while ibv_post_send\n");
		return RDMA_FAILED;
	}

	while((err = ibv_poll_cq(conn->cm_id->qp->send_cq, 1, &wc)) == 0);

	if(err < 0){
		PRINTF_ERR("err occure while ibv_poll_cq in %s\n", __func__);
		return RDMA_FAILED;
	}
		
	if (wc.status != IBV_WC_SUCCESS){
		PRINTF_ERR("err %d in %s\n", wc.status, __func__);
		return RDMA_FAILED;
	}

	ibv_ack_cq_events(conn->cm_id->qp->send_cq, 1);

	if(wc.wr_id != msgid){
		PRINTF_ERR("wc.wr_id != msgid .In %s: %d\n",__func__,__LINE__);
		return RDMA_FAILED;
	}
	PRINTF_INFO("send ok\n");
	return (RDMA_SUCCESS);
}


rdma_stat
dw_send_imm(CONN *conn, struct clist *cl, uint32_t msgid)
{

	struct clist	*clp;

	//rdma_stat	ret = RDMA_SUCCESS;

	int		err; 
	int		nds;

	u_int32_t		total_msg_size;

	struct ibv_cq		       	*evt_cq;
	void			       		*cq_context;

	struct ibv_sge			sgl[2];
	struct ibv_send_wr		send_wr = { };
	struct ibv_send_wr	       *bad_send_wr;
	struct ibv_wc				wc;

	nds = 0;
	total_msg_size = 0;
	clp = cl;
	while (clp != NULL) {
		if (nds >= 2) {
			//DTRACE_PROBE(rpcib__i__sendandwait_dsegmax_exceeded);
			return (RDMA_FAILED);
		}
		sgl[nds].addr = clp->w.c_saddr;
		sgl[nds].lkey = clp->c_smemhandle.mrc_lmr; /* lkey */
		sgl[nds].length = clp->c_len;
		total_msg_size += clp->c_len;
		PRINTF_INFO("the length of seg%d is %d\n", nds, clp->c_len);
		clp = clp->c_next;
		nds++;
	}

	PRINTF_INFO("the total_msg_size is %d, msgid is %d\n", total_msg_size, msgid);
	send_wr.wr_id 		= msgid;
	send_wr.opcode     	= IBV_WR_SEND_WITH_IMM;
	send_wr.send_flags 	= IBV_SEND_SIGNALED;
	send_wr.sg_list    		= sgl;
	send_wr.num_sge    	= nds;

	PRINTF_INFO("nds is %d in %s\n", nds, __func__);

	if (ibv_post_send(conn->cm_id->qp, &send_wr, &bad_send_wr)){
		PRINTF_ERR("err while ibv_post_send\n");
		return RDMA_FAILED;
	}

	while((err = ibv_poll_cq(conn->cm_id->qp->send_cq, 1, &wc)) == 0);

	if(err < 0){
		PRINTF_ERR("err occure while ibv_poll_cq in %s\n", __func__);
		return RDMA_FAILED;
	}
		
	if (wc.status != IBV_WC_SUCCESS){
		PRINTF_ERR("err %d in %s\n", wc.status, __func__);
		return RDMA_FAILED;
	}

	ibv_ack_cq_events(conn->cm_id->qp->send_cq, 1);

	if(wc.wr_id != msgid)
		return RDMA_FAILED;

	PRINTF_INFO("send ok\n");
	return (RDMA_SUCCESS);
}


/*
 * Client side only interface to "recv" the rpc reply buf
 * posted earlier by rib_post_resp(conn, cl, msgid).
 */
rdma_stat
dw_recv(CONN *conn, u_int32 msgid)
{
	PRINTF_INFO("%s\n", __func__);

	int		err; 
	struct ibv_wc			wc;
	
	while((err = ibv_poll_cq(conn->cm_id->qp->recv_cq, 1, &wc)) == 0);

	if(err < 0){
		PRINTF_ERR("there is an err %d in %s\n",err,__func__);
		return RDMA_FAILED;
	}
		
	if (wc.status != IBV_WC_SUCCESS){
		PRINTF_ERR("wrong completion with code %d in %s\n", wc.status,__func__);
		return RDMA_FAILED;
	}

	ibv_ack_cq_events(conn->cm_id->qp->recv_cq, 1);

	PRINTF_INFO("recv a message\n");
	if(wc.wr_id != msgid){
		PRINTF_INFO("wc.wr_id is %d, msgid is %d\n",wc.wr_id, msgid);
		PRINTF_ERR("the msgid is wrong in %s\n",__func__);
		return RDMA_FAILED;
	}

	conn->to_be_done1->length = wc.byte_len;
	PRINTF_INFO("recv ok,recv len is %d\n",wc.byte_len);
	return (RDMA_SUCCESS);
	
}

CONN* alloc_conn(cm_id, pd, comp_chan, send_pool, recv_pool)
	struct rdma_cm_id 	*cm_id;
	struct ibv_pd 		*pd;
	struct ibv_comp_channel	       *comp_chan;
	dw_bufpool_t			*send_pool;
	dw_bufpool_t  		*recv_pool;
{
	CONN *conn;

	conn = mem_alloc(sizeof(CONN));

	conn->cm_id = cm_id;
	conn->pd = pd;
	conn->comp_chan = comp_chan;
	conn->send_pool = send_pool;
	conn->recv_pool = recv_pool;
        //conn->longrpc_pool = longrpc_pool;

	conn->to_be_done1 = mem_alloc(sizeof(struct vec));
	conn->to_be_done2 = mem_alloc(sizeof(struct vec));
	return conn;
}
/* END */

/* MODIFIED by dwyane @ 2011-01-11 */
int free_conn(CONN *conn)
{
	mem_free(conn->to_be_done1, sizeof(struct vec));
	mem_free(conn, sizeof(CONN));

	return 0;
}
/* END */

/* MODIFIED by dwyane @ 2010-12-30 */

rdma_stat
clist_register(CONN *conn, struct clist *cl, clist_dstsrc dstsrc)
{
	struct clist *c;
	int status;

	for (c = cl; c; c = c->c_next) {
		if (c->c_len <= 0)
			continue;

		c->c_regtype = dstsrc;

		switch (dstsrc) {
		case CLIST_REG_SOURCE:
			status = dw_registermemsync(conn,
				/* MODIFIED by dwyane @ 2011-01-06 */
				//(caddr_t)(uintptr_t)c->w.c_saddr3, c->c_len,
				(caddr_t)(uintptr_t)c->w.c_saddr3, c->c_len,
				/* END */  
			    &c->c_smemhandle, (void **)&c->c_ssynchandle, dstsrc);
			break;
		case CLIST_REG_DST:
			status = dw_registermemsync(conn,
			    (caddr_t)(uintptr_t)c->u.c_daddr3, c->c_len,
			    &c->c_dmemhandle, (void **)&c->c_dsynchandle, dstsrc);
			break;
		default:
			return (RDMA_INVAL);
		}
		if (status != RDMA_SUCCESS) {
			PRINTF_ERR("clist_register err in %s:%d",__func__,__LINE__);
			(void) clist_deregister(cl);
			return (status);
		}
	}

	return (RDMA_SUCCESS);
}


rdma_stat
clist_deregister(struct clist *cl)
{

	struct clist *c;
	int status = RDMA_SUCCESS;

	for (c = cl; c; c = c->c_next) {
		switch (c->c_regtype) {
		case CLIST_REG_SOURCE:
			if (c->c_smemhandle.mrc_rmr != 0) {
				status = ibv_dereg_mr(c->c_smemhandle.mrc_linfo);
                                c->c_smemhandle.mrc_rmr = 0;
				c->c_ssynchandle = NULL;
			}
			break;
		case CLIST_REG_DST:
			if (c->c_dmemhandle.mrc_rmr != 0) {
				status = ibv_dereg_mr(c->c_dmemhandle.mrc_linfo);
				c->c_dmemhandle.mrc_rmr = 0;
				c->c_dsynchandle = NULL;
			}
			break;
		default:
			/* clist unregistered. continue */
			break;
		}
		if (status != RDMA_SUCCESS) {
			PRINTF_ERR("clist_deregister err in %s:%d",__func__,__LINE__);
			return (status);
		}
	}

	return (RDMA_SUCCESS);
}


/* END */

