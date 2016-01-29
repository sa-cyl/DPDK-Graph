/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#include <rpc/types.h>
#include <rpc/xdr.h>
#include <sys/types.h>
//#include <sys/sdt.h>
#include <rpc/auth.h>
#include <rpc/rpc_rdma.h>
#include "dwyane.h"

struct private {
	int			min_chunk;
	u_int32_t		flags;			/* controls setting for rdma xdr */
	int			num_chunk;
	caddr_t		inline_buf;		/* temporary buffer for xdr inlining */
	int			inline_len;		/* inline buffer length */
	u_int32_t		xp_reply_chunk_len;
	u_int32_t		xp_reply_chunk_len_alt;
};

/* ARGSUSED */
static bool_t
x_putint32_t(XDR *xdrs, int32_t *ip)
{
	xdrs->x_handy += BYTES_PER_XDR_UNIT;
	return (TRUE);
}

/* ARGSUSED */
static bool_t
x_putbytes(XDR *xdrs, char *bp, int len)
{
	struct private *xdrp = (struct private *)xdrs->x_private;

	/*
	 * min_chunk = 0, means that the stream of bytes, to estimate size of,
	 * contains no chunks to seperate out. See xdrrdma_putbytes()
	 */
	if (len < xdrp->min_chunk || !(xdrp->flags & XDR_RDMA_CHUNK)) {
		xdrs->x_handy += len;
		return (TRUE);
	}
	/*
	 * Chunk item. No impact on xdr size.
	 */
	xdrp->num_chunk++;

	return (TRUE);
}

static u_int32_t
x_getpostn(XDR *xdrs)
{
	return (xdrs->x_handy);
}

/* ARGSUSED */
static bool_t
x_setpostn(XDR *xdrs, u_int32_t pos)
{
	/* This is not allowed */
	return (FALSE);
}

/* ARGSUSED */
static bool_t
x_control(XDR *xdrs, int request, void *info)
{
	int32_t *int32p;
	u_int32_t in_flags;
	rdma_chunkinfo_t *rcip = NULL;
	rdma_chunkinfo_lengths_t *rcilp = NULL;
	struct private *xdrp = (struct private *)xdrs->x_private;

	switch (request) {
	case XDR_RDMA_SET_FLAGS:
		/*
		 * Set the flags provided in the *info in xp_flags for rdma xdr
		 * stream control.
		 */
		int32p = (int32_t *)info;
		in_flags = (u_int32_t)(*int32p);

		xdrp->flags = in_flags;
		return (TRUE);

	case XDR_RDMA_GET_FLAGS:
		/*
		 * Get the flags provided in xp_flags return through *info
		 */
		int32p = (int32_t *)info;

		*int32p = (int32_t)xdrp->flags;
		return (TRUE);

	case XDR_RDMA_GET_CHUNK_LEN:
		rcilp = (rdma_chunkinfo_lengths_t *)info;
		rcilp->rcil_len = xdrp->xp_reply_chunk_len;
		rcilp->rcil_len_alt = xdrp->xp_reply_chunk_len_alt;

		return (TRUE);

	case XDR_RDMA_ADD_CHUNK:
		rcip = (rdma_chunkinfo_t *)info;

		switch (rcip->rci_type) {
		case RCI_WRITE_UIO_CHUNK:
			xdrp->xp_reply_chunk_len_alt += rcip->rci_len;
			break;

		case RCI_WRITE_ADDR_CHUNK:
			xdrp->xp_reply_chunk_len_alt += rcip->rci_len;
			break;

		case RCI_REPLY_CHUNK:
			xdrp->xp_reply_chunk_len += rcip->rci_len;
			break;
		}
		return (TRUE);

	default:
		return (FALSE);
	}
}

/* ARGSUSED */
static rpc_inline_t *
x_inline(XDR *xdrs, int len)
{
	struct private *xdrp = (struct private *)xdrs->x_private;

	if (len == 0) {
		return (NULL);
	}
	if (xdrs->x_op != XDR_ENCODE) {
		return (NULL);
	}
	if (len >= xdrp->min_chunk) {
		return (NULL);
	}
	if (len <= xdrp->inline_len) {
		/* inline_buf was already allocated, just reuse it */
		xdrs->x_handy += len;
		return ((rpc_inline_t *)xdrp->inline_buf);
	} else {
		/* Free the earlier space and allocate new area */
		if (xdrp->inline_buf)
			mem_free(xdrp->inline_buf, xdrp->inline_len);
		if ((xdrp->inline_buf = (caddr_t)mem_alloc(len)) == NULL) {
			xdrp->inline_len = 0;
			return (NULL);
		}
		xdrp->inline_len = len;
		xdrs->x_handy += len;
		return ((rpc_inline_t *)xdrp->inline_buf);
	}
}

static int
harmless()
{
	/* Always return FALSE/NULL, as the case may be */
	return (0);
}

static void
x_destroy(XDR *xdrs)
{
	struct private *xdrp = (struct private *)xdrs->x_private;

	xdrs->x_handy = 0;
	if (xdrp) {
		if (xdrp->inline_buf)
			mem_free(xdrp->inline_buf, xdrp->inline_len);
		mem_free(xdrp, sizeof (struct private));
		xdrs->x_private = NULL;
	}
	xdrs->x_base = 0;
}

struct xdr_ops *
xdrrdma_xops(void)
{
	static struct xdr_ops ops;

	/* to stop ANSI-C compiler from complaining */
	typedef  bool_t (* dummyfunc1)(XDR *, long *);
	typedef  bool_t (* dummyfunc2)(XDR *, caddr_t, int);
	typedef  bool_t (* dummyfunc3)(XDR *, int32_t *);

	ops.x_putbytes = x_putbytes;
	ops.x_inline = x_inline;
	ops.x_getpostn = x_getpostn;
	ops.x_setpostn = x_setpostn;
	ops.x_destroy = x_destroy;
	ops.x_control = x_control;


	ops.x_getlong = (dummyfunc3)harmless;
	ops.x_putlong = x_putint32_t;


	/* the other harmless ones */
	ops.x_getbytes = (dummyfunc2)harmless;

	return (&ops);
}

static bool_t
xdrrdma_common(XDR *xdrs, int min_chunk)
{
	struct private *xdrp;

	xdrs->x_ops = xdrrdma_xops();
	xdrs->x_op = XDR_ENCODE;
	xdrs->x_handy = 0;
	xdrs->x_base = NULL;
	xdrs->x_private = mem_alloc(sizeof (struct private));
	xdrp = (struct private *)xdrs->x_private;
	xdrp->min_chunk = min_chunk;
	xdrp->flags = 0;
	if (xdrp->min_chunk != 0)
		xdrp->flags |= XDR_RDMA_CHUNK;

	xdrp->xp_reply_chunk_len = 0;
	xdrp->xp_reply_chunk_len_alt = 0;

	return (TRUE);
}

unsigned int
xdrrdma_sizeof(xdrproc_t func, void *data, int min_chunk,
    u_int32_t *reply_size, u_int32_t *reply_size_alt)
{
	XDR x;
	struct xdr_ops ops;
	bool_t stat;
	struct private *xdrp;

	x.x_ops = &ops;
	(void) xdrrdma_common(&x, min_chunk);

	stat = func(&x, data);
	xdrp = (struct private *)x.x_private;
	if (xdrp) {
		if (reply_size != NULL)
			*reply_size = xdrp->xp_reply_chunk_len;
		if (reply_size_alt != NULL)
			*reply_size_alt = xdrp->xp_reply_chunk_len_alt;
		if (xdrp->inline_buf)
			mem_free(xdrp->inline_buf, xdrp->inline_len);
		mem_free(xdrp, sizeof (struct private));
	}
	return (stat == TRUE ? (unsigned int)x.x_handy: 0);
}

unsigned int
xdrrdma_authsize(AUTH *auth, struct cred *cred, int min_chunk)
{
	XDR x;
	//struct xdr_ops ops;
	bool_t stat;
	struct private *xdrp;

	//x.x_ops = &ops;
	(void) xdrrdma_common(&x, min_chunk);

	stat = AUTH_MARSHALL(auth, &x);//, cred);
	xdrp = (struct private *)x.x_private;
	if (xdrp) {
		if (xdrp->inline_buf)
			mem_free(xdrp->inline_buf, xdrp->inline_len);
		mem_free(xdrp, sizeof (struct private));
	}
	return (stat == TRUE ? (unsigned int)x.x_handy: 0);
}



struct private_wcl {
	void*		xp_offp;
	int		xp_min_chunk;
	u_int32_t		xp_flags;	/* Controls setting for rdma xdr */
	//int		xp_buf_size;	/* size of xdr buffer */
	//int		xp_off;		/* overall offset */
	//struct clist	*xp_rcl;	/* head of chunk list */
	//struct clist	*xp_rcl_xdr;	/* copy of rcl containing RPC message */
	struct clist	*xp_wcl;	/* head of write chunk list */
	struct clist	**xp_wcl_next;	/* location to place/find next chunk */
	//CONN		*xp_conn;	/* connection for chunk data xfer */
	//CONN *xp_conn;
	//u_int32_t		xp_reply_chunk_len;
	/* used to track length for security modes: integrity/privacy */
	//u_int32_t		xp_reply_chunk_len_alt;
};



bool_t
x_prepare_wlist_u_int32(XDR *xdrs,u_int *up)
{
	struct private_wcl *xdrp = (struct private_wcl *)xdrs->x_private;
	if ((xdrs->x_handy -= (int)sizeof (int32_t)) < 0){
		PRINTF_ERR("xdrs->x_handy -= (int)sizeof (int32_t)) < 0 in %s\n",__func__);
		return (FALSE);
	}
	xdrp->xp_offp += sizeof (int32_t);
	
	return TRUE;
}



bool_t
x_prepare_wlist_bytes(XDR *xdrs, caddr_t addr, int len){

	struct private_wcl *xdrp = (struct private_wcl *)xdrs->x_private;

#if 0
	if (! x_prepare_wlist_u_int(xdrs, sizep)) {
		PRINTF_ERR("xdr_u_int failure. In %s: %d\n",__func__,__LINE__);
		return (FALSE);
	}
	nodesize = *sizep;
	switch (xdrs->x_op) {

	case XDR_DECODE:
		//printf("%s:xdr decode~\n",__func__);
		if (nodesize == 0) {
			return (TRUE);
		}
		if (sp == NULL) {
			*cpp = sp = mem_alloc(nodesize);
		}
		if (sp == NULL) {
			PRINTF_ERR("err occurs in %s:%d: out of memory\n",__func__,__LINE__);
			return (FALSE);
		}
#endif
		printf("x_handy is %d, len is %d in %s:%d\n",xdrs->x_handy,len,__func__,__LINE__);
		if (xdrp->xp_flags & XDR_RDMA_CHUNK &&
	    	((xdrp->xp_min_chunk != 0 &&
	    	len >= xdrp->xp_min_chunk) ||
	    	(xdrs->x_handy - len  < 0))){
			struct clist	*cle;
			printf("wlist generate in %s!\n",__func__);
			xdrp->xp_offp += len;
			
			cle = clist_alloc();
			cle->c_xdroff = (u_int32)xdrp->xp_offp;
			cle->c_len = len;
			cle->u.c_daddr = (u_int64_t)(uintptr_t)addr;
			cle->c_next = NULL;
			*(xdrp->xp_wcl_next) = cle;
			xdrp->xp_wcl_next = &(cle->c_next);
		}else{
			printf("no wlist generate in %s!\n",__func__);
			xdrp->xp_offp += len;
			xdrs->x_handy -= len;
		}
		//break;
#if 0
	case XDR_ENCODE://should not happen
		PRINTF_ERR("err occurs in %s:%d\n",__func__,__LINE__);
		return FALSE;

	case XDR_FREE://should not happen
		PRINTF_ERR("err occurs in %s:%d\n",__func__,__LINE__);
		return (FALSE);
	}
#endif

	return (TRUE);
}


unsigned int
xdrrdma_prepare_wlist(xdrproc_t xdr_result, void *result_ptr, clist **wcl)
{
	XDR x;
	struct xdr_ops ops;
	bool_t stat;
	struct private_wcl *xdrp;



	/* to stop ANSI-C compiler from complaining */
	typedef  bool_t (* dummyfunc1)(XDR *, long *);
	typedef	 bool_t (* dummyfunc2)(XDR *xdrs, char *bp, int len);
	typedef  rpc_inline_t (* dummyfunc3)(XDR *xdrs, int len);
	ops.x_putbytes = x_prepare_wlist_bytes;
	ops.x_putlong = x_prepare_wlist_u_int32;
	ops.x_inline = (dummyfunc3)harmless;
	ops.x_getpostn = NULL;
	ops.x_setpostn = NULL;
	ops.x_destroy = NULL;
	ops.x_control = NULL;


	ops.x_getlong = NULL;
	ops.x_getbytes = NULL;

	x.x_handy = RPC_MSG_SZ - DW_HDRSIZE - (int)sizeof (int32_t);
	x.x_base = NULL;
	x.x_ops = &ops;
	x.x_op = XDR_ENCODE;
 	x.x_base = NULL;
	x.x_private = mem_alloc(sizeof (struct private_wcl));
	xdrp = (struct private_wcl *)x.x_private;
	xdrp->xp_min_chunk = RDMA_MINCHUNK;
	xdrp->xp_offp = 0;
	xdrp->xp_wcl = NULL;
	xdrp->xp_wcl_next = &(xdrp->xp_wcl);
	if (xdrp->xp_min_chunk != 0)
		xdrp->xp_flags |= XDR_RDMA_CHUNK;
	stat = xdr_result(&x, result_ptr);
	*wcl = xdrp->xp_wcl;
	if (xdrp) {
		mem_free(xdrp, sizeof (struct private_wcl));
	}
	return (TRUE);
}


