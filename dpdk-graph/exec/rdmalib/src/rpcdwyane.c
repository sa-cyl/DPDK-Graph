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

void debug_test()
{}


rdma_stat
dw_reg_mem(struct ibv_pd *pd, struct mrc *buf_handlestruct, void **mr_handle, struct ibv_mr_reg_arg *mem_attr );
rdma_stat
dw_registermemsync(CONN *conn, caddr_t buf, u_int32_t buflen,
	struct mrc *buf_handle, void **mr_handle, clist_dstsrc dstsrc);


rdma_stat
dw_reg_mem(struct ibv_pd *pd, struct mrc *buf_handlestruct, void **mr_handle, struct ibv_mr_reg_arg *mem_attr )
{
	struct ibv_mr *mr;
	
	mr = ibv_reg_mr(pd, mem_attr->addr, mem_attr->length, mem_attr->access);
	if (!mr) {
		/* MODIFIED by dwyane @ 2011-01-06 */
		PRINTF_ERR("in %s, return CLNT_RDMA_FAIL\n", __func__);
		/* END */
		(void) ibv_dereg_mr(mr);
		return (RDMA_INVAL);
	}

	buf_handlestruct->mrc_rmr = mr->rkey;
	buf_handlestruct->mrc_lmr = mr->lkey;
	buf_handlestruct->mrc_linfo = mr;

	*mr_handle = mr;
	
	return (RDMA_SUCCESS);
}

rdma_stat
dw_registermemsync(CONN *conn, caddr_t buf, u_int32_t buflen,
	struct mrc *buf_handle, void **mr_handle, clist_dstsrc dstsrc)
{
	int status;
	enum ibv_access_flags access;
	struct ibv_mr_reg_arg mem_attr;
	
	switch (dstsrc) {
		case CLIST_REG_SOURCE:
			access = IBV_ACCESS_LOCAL_WRITE | 
					IBV_ACCESS_REMOTE_READ;
			break;
		case CLIST_REG_DST:
			access = IBV_ACCESS_LOCAL_WRITE | 
					IBV_ACCESS_REMOTE_WRITE;
			break;
		default:
			return (RDMA_INVAL);
	}

	mem_attr.addr = buf;
	mem_attr.length = buflen;
	mem_attr.access = access;

	return(dw_reg_mem(conn->pd, buf_handle, mr_handle, &mem_attr));
}


/*
 * RDMA Read a buffer from the remote address.
 */
rdma_stat
dw_read(CONN *conn, struct clist *cl)
{
	int 						total_msg_size;
	int						err;
	struct ibv_cq		       	*evt_cq;
	void			       		*cq_context;

	struct ibv_sge			sge;
	struct ibv_send_wr		send_wr = { };
	struct ibv_send_wr	       *bad_send_wr;
	struct ibv_wc				wc;
	
	if (cl == NULL) {
		return (RDMA_FAILED);
	}

	total_msg_size = 0;
		
	sge.addr 		= cl->u.c_daddr3;
	sge.lkey 		= cl->c_dmemhandle.mrc_lmr; /* lkey */
	sge.length 	= cl->c_len;
	
	total_msg_size += cl->c_len;

	PRINTF_INFO("the total_msg_size is %d\n", total_msg_size);
	send_wr.wr_id 		= IBV_WR_RDMA_READ;
	send_wr.opcode     	= IBV_WR_RDMA_READ;
	send_wr.send_flags 	= IBV_SEND_SIGNALED;
	send_wr.sg_list    		= &sge;
	send_wr.num_sge    	= 1;
	send_wr.next			= NULL;

	send_wr.wr.rdma.rkey			= cl->c_smemhandle.mrc_rmr;
	send_wr.wr.rdma.remote_addr 	= cl->w.c_saddr;
		
	if (ibv_post_send(conn->cm_id->qp, &send_wr, &bad_send_wr)){
		PRINTF_ERR("err while ibv_post_send\n");
		return RDMA_FAILED;
	}
	
	while((err = ibv_poll_cq(conn->cm_id->qp->send_cq, 1, &wc)) == 0){
		//PRINTF_ERR("wait\n");
	}

	if(err < 0){
		PRINTF_ERR("err occure while ibv_poll_cq in %s\n", __func__);
		return RDMA_FAILED;
	}
		
	if (wc.status != IBV_WC_SUCCESS){
		PRINTF_ERR("err %d in %s\n", wc.status, __func__);
		return RDMA_FAILED;
	}

	ibv_ack_cq_events(conn->cm_id->qp->send_cq, 1);

	if(wc.wr_id!= IBV_WR_RDMA_READ){
		PRINTF_ERR("wc.opcode is %d\n", wc.opcode);
		return RDMA_FAILED;
	}

	PRINTF_INFO("read ok\n");
	return (RDMA_SUCCESS);
	
}


rdma_stat
dw_write(CONN *conn, struct clist *cl)
{
	struct clist	*clp;	
	int 	err; 
	int 	nds;
	
	u_int32_t		total_msg_size;
	
	struct ibv_cq				*evt_cq;
	void						*cq_context;
	
	struct ibv_sge			sgl[2];
	struct ibv_send_wr		send_wr = { };
	struct ibv_send_wr		   *bad_send_wr;
	struct ibv_wc				wc;
	if(cl==NULL)
		return (RDMA_SUCCESS);
	nds = 0;
	total_msg_size = 0;
	clp = cl;
	while (clp != NULL) {
		if (nds >= 2) {
			PRINTF_ERR("nds >= 2.In %s %d\n", __func__,__LINE__);
			//yh:5-30 可能大于2，现在还未处理
			//return (RDMA_FAILED);
		}
		sgl[nds].addr = clp->w.c_saddr3;
		sgl[nds].lkey = clp->c_smemhandle.mrc_lmr; /* lkey */
		sgl[nds].length = clp->c_len;
		total_msg_size += clp->c_len;
		PRINTF_INFO("the length of seg%d is %d\n", nds, clp->c_len);
		clp = clp->c_next;
		nds++;
	}
	
	PRINTF_INFO("the total_msg_size is %d, msgid is %d\n", total_msg_size);
	send_wr.wr_id		= IBV_WR_RDMA_WRITE;
	send_wr.opcode		= IBV_WR_RDMA_WRITE;
	send_wr.send_flags	= IBV_SEND_SIGNALED;
	send_wr.sg_list 		= sgl;
	send_wr.num_sge 	= nds;
	send_wr.next			= NULL;

	send_wr.wr.rdma.rkey			= cl->c_dmemhandle.mrc_rmr;
	send_wr.wr.rdma.remote_addr 	= cl->u.c_daddr;
	
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

	if(wc.wr_id != IBV_WR_RDMA_WRITE){
		PRINTF_ERR("wc.wr_id != msgid .In %s: %d\n",__func__,__LINE__);
		return RDMA_FAILED;
	}
	PRINTF_INFO("write ok\n");
	return (RDMA_SUCCESS);											
}


