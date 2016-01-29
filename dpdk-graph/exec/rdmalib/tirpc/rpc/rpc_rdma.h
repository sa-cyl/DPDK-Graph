/* MODIFIED by dwyane @ 2010-12-23 */
//#include <reentrant.h>
//#include <rpc/svc.h>
//#include <rpc/xdr.h>
#include <rpc/rpc.h>
#include <infiniband/arch.h>
#include <rdma/rdma_cma.h>
/* END */

/* MODIFIED by dwyane @ 2010-12-24 */

//#define	MAX_SVC_XFER_SIZE (4*1024*1024)
#define MAX_SVC_XFER_SIZE    4294967296

#define	RDMA_BUFS_RQST	34	/* Num bufs requested by client */
#define	RDMA_BUFS_GRANT	32	/* Num bufs granted by server */

#define SVC_MAX_SEND_WR 	1
#define SVC_MAX_SEND_SGE 	2
#define SVC_MAX_RECV_WR 	2
#define SVC_MAX_RECV_SGE 	1
#define CLNT_MAX_SEND_WR	2
#define CLNT_MAX_SEND_SGE	2
#define CLNT_MAX_RECV_WR	2
#define CLNT_MAX_RECV_SGE	1







/* structure used to pass information for READ over rdma write */
typedef enum {
	RCI_WRITE_UIO_CHUNK = 1,
	RCI_WRITE_ADDR_CHUNK = 2,
	RCI_REPLY_CHUNK = 3
} rci_type_t;

/*
 * The XDR offset value is used by the XDR
 * routine to identify the position in the
 * RPC message where the opaque object would
 * normally occur. Neither the data content
 * of the chunk, nor its size field are included
 * in the RPC message.  The XDR offset is calculated
 * as if the chunks were present.
 *
 * The remaining fields identify the chunk of data
 * on the sender.  The c_memhandle identifies a
 * registered RDMA memory region and the c_addr
 * and c_len fields identify the chunk within it.
 */

typedef struct {
	u_int32_t rcil_len;
	u_int32_t rcil_len_alt;
} rdma_chunkinfo_lengths_t;

enum rdma_proc {
	RDMA_MSG	= 0,	/* chunk list and RPC msg follow */
	RDMA_NOMSG	= 1,	/* only chunk list follows */
	RDMA_MSGP	= 2,	/* chunk list and RPC msg with padding follow */
	RDMA_DONE	= 3	/* signal completion of chunk transfer */
};

typedef enum {
    RPCCALL_WLIST,
    RPCCALL_WCHUNK,
    RPCCALL_NOWRITE
}rpccall_write_t;

typedef enum {

	RDMA_SUCCESS = 0,	/* successful operation */

	RDMA_INVAL = 1,		/* invalid parameter */
	RDMA_TIMEDOUT = 2,	/* operation timed out */
	RDMA_INTR = 3,		/* operation interrupted */
	RDMA_NORESOURCE = 4,	/* insufficient resource */
	/*
	 * connection errors
	 */
	RDMA_REJECT = 5,	/* connection req rejected */
	RDMA_NOLISTENER = 6,	/* no listener on server */
	RDMA_UNREACHABLE = 7,	/* host unreachable */
	RDMA_CONNLOST = 8,	/* connection lost */

	RDMA_XPRTFAILED = 9,	/* RDMA transport failed */
	RDMA_PROTECTERR = 10,	/* memory protection error */
	RDMA_OVERRUN = 11,	/* transport overrun */
	RDMA_RECVQEMPTY = 12,	/* incoming pkt dropped, recv q empty */
	RDMA_PROTFAILED = 13,	/* RDMA protocol failed */
	RDMA_NOTSUPP = 14,	/* requested feature not supported */
	RDMA_REMOTERR = 15,	/* error at remote end */
	/*
	 * RDMATF errors
	 */
	RDMA_BADVERS = 16,	/* mismatch RDMATF versions */
	RDMA_REG_EXIST = 17,	/* RDMATF registration already exists */
	RDMA_HCA_ATTACH = 18,
	RDMA_HCA_DETACH = 19,

	/*
	 * fallback error
	 */
	RDMA_FAILED = 20	/* generic error */
} rdma_stat;

/*
 * RDMA xdr buffer control and other control flags. Add new flags here,
 * set them in private structure for xdr over RDMA in xdr_rdma.c
 */
#define	XDR_RDMA_CHUNK			0x1
#define	XDR_RDMA_WLIST_REG		0x2
#define	XDR_RDMA_RLIST_REG		0x4

#define	LONG_REPLY_LEN	65536
#define	WCL_BUF_LEN	32768
#define	RCL_BUF_LEN	32768
/*
 * The size of an RPC call or reply message
 */
#define	RPC_MSG_SZ	2048

//#define RPC_LONG_SIZE     400000000
//#define RPC_LONG_SIZE      10000000
/*
 * RDMA chunk size
 */
#define	RDMA_MINCHUNK	1536

/*
 * Storage for a chunk list
 */
#define	RPC_CL_SZ  1024



/*
 * Size of receive buffer
 */
#define	RPC_BUF_SIZE	2048

struct vec{
	void 		*data;
	u_int32_t		length;
	u_int64_t		data_prt;
};


/*
 * RDMA buffer types
 */

struct mrc {
	u_int32_t	mrc_rmr;	/* Remote MR context, sent OTW */
	union {
		struct mr {
			u_int32_t	lmr; 	/* Local MR context */
			u_int64_t	linfo;	/* Local memory info */
		} mr;
	} lhdl;
};

#define	mrc_lmr		lhdl.mr.lmr
#define	mrc_linfo	lhdl.mr.linfo

/*
 * Memory management for the RDMA buffers
 */
typedef enum {
	SEND_BUFFER,	/* buf for send msg */
	SEND_DESCRIPTOR, /* buf used for send msg descriptor in plugins only */
	RECV_BUFFER,	/* buf for recv msg */
	RECV_DESCRIPTOR, /* buf used for recv msg descriptor in plugins only */
	RDMA_LONG_BUFFER /* chunk buf used in RDMATF only and not in plugins */
} rdma_btype;

typedef enum {
	CLIST_REG_SOURCE = 1,
	CLIST_REG_DST
} clist_dstsrc;

/*
 * RDMA buffer information
 */
typedef struct rdma_buf {
	rdma_btype	type;	/* buffer type */
	u_int32_t		len;	/* length of buffer */
	void			*addr;	/* buffer address */
	struct mrc	handle;	/* buffer registration handle */
	void			*rb_private;
} rdma_buf_t;

typedef struct {
	pthread_mutex_t	buflock;	/* lock for this structure */
	void		*buf;		/* pool address */
	u_int32_t	bufhandle;	/* rkey for this pool */
	u_int32_t		bufsize;	/* size of pool */
	//int		rsize;		/* size of each element */
	u_int32_t       rsize;
	int		numelems;	/* no. of elements allocated */
	int		buffree;		/* no. of free elements */
	void		*buflist[1];	/* free elements in pool */
} bufpool_t;

struct ibv_mr_reg_arg{
	void *addr;
	size_t length;
	enum ibv_access_flags access;
};

typedef struct {
	rci_type_t rci_type;
	union {
		struct uio *rci_uiop;
		caddr_t    rci_addr;
	} rci_a;
	u_int32_t	rci_len;
	struct clist	**rci_clpp; /* point to write chunk list in readargs */
} rdma_chunkinfo_t;

typedef struct {
	bufpool_t	*bpool;
	struct ibv_mr **mr_hdl;	/* vaddr, lkey, rkey */
} dw_bufpool_t;

struct clist {
	u_int32		c_xdroff;	/* XDR offset */
	u_int32		c_len;		/* Length */
	clist_dstsrc	c_regtype;	/* type of registration */
	struct mrc	c_smemhandle;	/* src memory handle */
	u_int64_t 		c_ssynchandle;	/* src sync handle */
	union {
		u_int64_t		c_saddr;	/* src address */
		void* 	c_saddr3;
	} w;
	struct mrc	c_dmemhandle;	/* dst memory handle */
	u_int64_t		c_dsynchandle;	/* dst sync handle */
	union {
		u_int64_t	c_daddr;	/* dst address */
		void *	c_daddr3;
	} u;
	//struct as	*c_adspc;	/* address space for saddr/daddr */
	rdma_buf_t	rb_longbuf;	/* used for long requests/replies */
	struct clist	*c_next;	/* Next chunk */
};

typedef struct clist clist;

/* END */


/* MODIFIED by dwyane @ 2010-12-23 */

#define	DW_HDRSIZE	20
#define	CLNT_RDMA_SUCCESS 0
#define	CLNT_RDMA_FAIL (-1)


/* END */

/* MODIFIED by dwyane @ 2010-12-25 */

typedef struct conn {
	//CLIENT 				clnt;
	struct rdma_cm_id 	*cm_id;
	struct ibv_pd 		*pd;
	struct ibv_comp_channel	       *comp_chan;
#if 	0
	XDR					dw_outxdr;	/* xdr stream for output */
	u_int32_t				dw_outsz;
	XDR					dw_inxdr;	/* xdr stream for input */
	char					dw_rpchdr[DW_HDRSIZE+4]; /* rpc header */
	u_int32_t				dw_xid;	/* current XID */
	struct rpc_err		dw_err;	/* error status */
#endif
	dw_bufpool_t			*send_pool;
	dw_bufpool_t  		*recv_pool;
        //dw_bufpool_t            *longrpc_pool;
	struct vec			*to_be_done1;
	struct vec			*to_be_done2;
} CONN;

typedef struct {
	struct	clist	*rwci_wlist;
	CONN		*rwci_conn;
} rdma_wlist_conn_info_t;

extern rdma_stat rdma_svc_postrecv(CONN *conn);
dw_bufpool_t * dw_rbufpool_create(struct ibv_pd *pd, int ptype, int num);
rdma_stat rdma_buf_alloc(CONN *conn, rdma_buf_t *rdbuf);
void rdma_buf_free(CONN *conn, rdma_buf_t *rbuf);
void
clist_add(struct clist **clp, uint32_t xdroff, int len,
	struct mrc *shandle, caddr_t saddr,
	struct mrc *dhandle, caddr_t daddr);
struct clist *
clist_alloc(void);
void
clist_free(struct clist *cl);
rdma_stat
rdma_clnt_postrecv(CONN *conn, uint32_t xid);
rdma_stat
dw_clnt_post(CONN *conn, struct clist *cl, uint32_t msgid);
rdma_stat
dw_svc_post(CONN *conn, struct clist *cl);
void
dw_rbuf_free(CONN *conn, int ptype, void **buf);
rdma_stat
dw_send(CONN *conn, struct clist *cl, uint32_t msgid);
rdma_stat
dw_send_imm(CONN *conn, struct clist *cl, uint32_t msgid);
rdma_stat
dw_recv(CONN *conn, u_int32 msgid);
CONN* 
alloc_conn(struct rdma_cm_id *cm_id,struct ibv_pd *pd,
				struct ibv_comp_channel *comp_chan,
				dw_bufpool_t *send_pool,dw_bufpool_t *recv_pool);


/* END */

