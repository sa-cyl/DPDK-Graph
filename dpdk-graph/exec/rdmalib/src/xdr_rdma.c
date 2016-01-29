
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
#include <sys/cdefs.h>

/*
 * xdr_rec.c, Implements TCP/IP based XDR streams with a "record marking"
 * layer above tcp (for rpc's use).
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 *
 * These routines interface XDRSTREAMS to a tcp/ip connection.
 * There is a record marking layer between the xdr stream
 * and the tcp transport level.  A record is composed on one or more
 * record fragments.  A record fragment is a thirty-two bit header followed
 * by n bytes of data, where n is contained in the header.  The header
 * is represented as a htonl(u_long).  Thegh order bit encodes
 * whether or not the fragment is the last fragment of the record
 * (1 => fragment is last, 0 => more fragments to follow. 
 * The other 31 bits encode the byte length of the fragment.
 */

#include <sys/types.h>

#include <netinet/in.h>

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rpc/types.h>
#include <rpc/xdr.h>
#include <rpc/auth.h>
#include <rpc/svc_auth.h>
#include <rpc/svc.h>
#include <rpc/clnt.h>
#include <stddef.h>
#include "rpc_com.h"
#include <unistd.h>

#include <rpc/rpc_rdma.h>

/* MODIFIED by dwyane @ 2011-01-12 */
#include "dwyane.h"
/* END */

/* MODIFIED by dwyane @ 2010-12-24 */
static bool_t   xdrrdma_getint32(XDR *, int32_t *);
static bool_t   xdrrdma_putint32(XDR *, int32_t *);
static bool_t   xdrrdma_getbytes(XDR *, caddr_t, int);
static bool_t   xdrrdma_putbytes(XDR *, caddr_t, int);
static bool_t   xdrrdma_putbytes_svc(XDR *, caddr_t, int);
u_int32_t		xdrrdma_getpos(XDR *);
bool_t		xdrrdma_setpos(XDR *, u_int32_t);
static rpc_inline_t *xdrrdma_inline(XDR *, int);
void		xdrrdma_destroy(XDR *);
static bool_t   xdrrdma_control(XDR *, int, void *);
static bool_t  xdrrdma_read_a_chunk(XDR *, CONN**);
static void xdrrdma_free_xdr_chunks(CONN *, struct clist *);

typedef struct {
	void*		xp_offp;
	int		xp_min_chunk;
	u_int32_t		xp_flags;	/* Controls setting for rdma xdr */
	int		xp_buf_size;	/* size of xdr buffer */
	int		xp_off;		/* overall offset */
	struct clist	*xp_rcl;	/* head of chunk list */
	struct clist	**xp_rcl_next;	/* location to place/find next chunk */
	struct clist	*xp_rcl_xdr;	/* copy of rcl containing RPC message */
	struct clist	*xp_wcl;	/* head of write chunk list */
	struct clist	**xp_wcl_next;	/* location to place/find next chunk */
	//CONN		*xp_conn;	/* connection for chunk data xfer */
	CONN *xp_conn;
	u_int32_t		xp_reply_chunk_len;
	/* used to track length for security modes: integrity/privacy */
	u_int32_t		xp_reply_chunk_len_alt;
} xrdma_private_t;

struct xdr_ops  xdrrdma_ops = {
	xdrrdma_getint32,//getlong
	xdrrdma_putint32,
	xdrrdma_getbytes,
	xdrrdma_putbytes,
	xdrrdma_getpos,
	xdrrdma_setpos,
	xdrrdma_inline,
	xdrrdma_destroy,
	xdrrdma_control,

};

struct xdr_ops  xdrrdma_ops_svc = {
	xdrrdma_getint32,//getlong
	xdrrdma_putint32,
	xdrrdma_getbytes,
	xdrrdma_putbytes_svc,
	xdrrdma_getpos,
	xdrrdma_setpos,
	xdrrdma_inline,
	xdrrdma_destroy,
	xdrrdma_control,

};

/* END */

/* MODIFIED by dwyane @ 2010-12-24 */

/*
 * The procedure xdrrdma_create initializes a stream descriptor for a memory
 * buffer.
 */
void
xdrrdma_create(XDR *xdrs, void *addr, u_int32_t size,
    int min_chunk, struct clist *rcl,struct clist *wcl, enum xdr_op op, CONN *conn)
{
	xrdma_private_t *xdrp;
	struct clist   *cle;

	xdrs->x_op = op;
	xdrs->x_ops = &xdrrdma_ops;
	xdrs->x_base = addr;
	xdrs->x_handy = size;
	xdrs->x_public = NULL;

	xdrp = (xrdma_private_t *)mem_alloc(sizeof (xrdma_private_t));
	xdrs->x_private = xdrp;
	xdrp->xp_offp = addr;
	xdrp->xp_min_chunk = min_chunk;
	xdrp->xp_flags = 0;
	xdrp->xp_buf_size = size;
	xdrp->xp_rcl = rcl;
	xdrp->xp_reply_chunk_len = 0;
	xdrp->xp_reply_chunk_len_alt = 0;
	xdrp->xp_conn = conn;

	if (op == XDR_ENCODE && rcl != NULL) {
		/* Find last element in chunk list and set xp_rcl_next */
		for (cle = rcl; cle->c_next != NULL; cle = cle->c_next)
			continue;
		xdrp->xp_rcl_next = &(cle->c_next);
	} else {
		xdrp->xp_rcl_next = &(xdrp->xp_rcl);
	}

	xdrp->xp_wcl = wcl;
	if (op == XDR_ENCODE && wcl != NULL) {
		for (cle = wcl; cle->c_next != NULL; cle = cle->c_next)
			continue;
		
		xdrp->xp_wcl_next = &(cle->c_next);
	} else {
		xdrp->xp_wcl_next = &(xdrp->xp_wcl);
	}

	//xdrp->xp_conn = conn;
	if (xdrp->xp_min_chunk != 0)
		xdrp->xp_flags |= XDR_RDMA_CHUNK;
}


void
xdrrdma_create_svc(XDR *xdrs, void *addr, u_int32_t size,
    int min_chunk, struct clist *rcl,struct clist *wcl, enum xdr_op op, CONN *conn)
{
	xrdma_private_t *xdrp;
	struct clist   *cle;

	xdrs->x_op = op;
	xdrs->x_ops = &xdrrdma_ops_svc;
	xdrs->x_base = addr;
	xdrs->x_handy = size;
	xdrs->x_public = NULL;

	xdrp = (xrdma_private_t *)mem_alloc(sizeof (xrdma_private_t));
	xdrs->x_private = xdrp;
	xdrp->xp_offp = addr;
	xdrp->xp_min_chunk = min_chunk;
	xdrp->xp_flags = 0;
	xdrp->xp_buf_size = size;
	xdrp->xp_rcl = rcl;
	xdrp->xp_reply_chunk_len = 0;
	xdrp->xp_reply_chunk_len_alt = 0;
	xdrp->xp_conn = conn;

	if (op == XDR_ENCODE && rcl != NULL) {
		/* Find last element in chunk list and set xp_rcl_next */
		for (cle = rcl; cle->c_next != NULL; cle = cle->c_next)
			continue;

		xdrp->xp_rcl_next = &(cle->c_next);
	} else {
		xdrp->xp_rcl_next = &(xdrp->xp_rcl);
	}

	xdrp->xp_wcl = wcl;
	//if (op == XDR_ENCODE && wcl != NULL) {
		//for (cle = wcl; cle->c_next != NULL; cle = cle->c_next)
			//continue;
		
		//xdrp->xp_wcl_next = &(cle->c_next);
	//} else {
		xdrp->xp_wcl_next = &(xdrp->xp_wcl);
	//}


	//xdrp->xp_conn = conn;
	if (xdrp->xp_min_chunk != 0)
		xdrp->xp_flags |= XDR_RDMA_CHUNK;
}

/* ARGSUSED */
void
xdrrdma_destroy(XDR * xdrs)
{
	xrdma_private_t	*xdrp = (xrdma_private_t *)(xdrs->x_private);

	if (xdrp == NULL)
		return;

	if (xdrp->xp_wcl) {
		//printf("In %s: %d\n",__func__,__LINE__);
		if (xdrp->xp_flags & XDR_RDMA_WLIST_REG) {
			(void) clist_deregister(xdrp->xp_wcl);
			rdma_buf_free(xdrp->xp_conn, &xdrp->xp_wcl->rb_longbuf);
		}
		clist_free(xdrp->xp_wcl);
		xdrp->xp_wcl = NULL;
	}

	if (xdrp->xp_rcl) {
		//printf("In %s: %d\n",__func__,__LINE__);
		if (xdrp->xp_flags & XDR_RDMA_RLIST_REG) {
			(void) clist_deregister(xdrp->xp_rcl);
			rdma_buf_free(xdrp->xp_conn, &xdrp->xp_rcl->rb_longbuf);
		}
		clist_free(xdrp->xp_rcl);
		xdrp->xp_rcl=NULL;
	}

	//if (xdrp->xp_rcl_xdr)
		//xdrrdma_free_xdr_chunks(xdrp->xp_conn, xdrp->xp_rcl_xdr);
    if((xrdma_private_t *)xdrs->x_private)
	   (void) mem_free((xrdma_private_t *)xdrs->x_private, sizeof (xrdma_private_t));
	xdrs->x_private = NULL;
}

static	bool_t
xdrrdma_getint32(XDR *xdrs, int32_t *int32p)
{
	xrdma_private_t	*xdrp = (xrdma_private_t *)(xdrs->x_private);
	int chunked = 0;

	if ((xdrs->x_handy -= (int)sizeof (int32_t)) < 0) {
		/*
		 * check if rest of the rpc message is in a chunk
		 */
#if 0
		if (!xdrrdma_read_a_chunk(xdrs, &xdrp->xp_conn)) {
			return (FALSE);
		}
#endif
		chunked = 1;
	}

	/* LINTED pointer alignment */
	*int32p = (int32_t)ntohl((u_int32_t)(*((int32_t *)(xdrp->xp_offp))));

	//DTRACE_PROBE1(krpc__i__xdrrdma_getint32, int32_t, *int32p);

	xdrp->xp_offp += sizeof (int32_t);

	if (chunked)
		xdrs->x_handy -= (int)sizeof (int32_t);

	if (xdrp->xp_off != 0) {
		xdrp->xp_off += sizeof (int32_t);
	}

	return (TRUE);
}

static	bool_t
xdrrdma_putint32(XDR *xdrs, int32_t *int32p)
{
	xrdma_private_t	*xdrp = (xrdma_private_t *)(xdrs->x_private);

	if ((xdrs->x_handy -= (int)sizeof (int32_t)) < 0){
		PRINTF_ERR("xdrs->x_handy -= (int)sizeof (int32_t)) < 0 in %s\n",__func__);
		return (FALSE);
	}

	/* LINTED pointer alignment */
	*(int32_t *)xdrp->xp_offp = (int32_t)htonl((uint32_t)(*int32p));
	xdrp->xp_offp += sizeof (int32_t);

	return (TRUE);
}

/*
 * DECODE bytes from XDR stream for rdma.
 * If the XDR stream contains a read chunk list,
 * it will go through xdrrdma_getrdmablk instead.
 */
static	bool_t
xdrrdma_getbytes(XDR *xdrs, caddr_t addr, int len)
{
	xrdma_private_t	*xdrp = (xrdma_private_t *)(xdrs->x_private);
	struct clist	*rcle = *(xdrp->xp_rcl_next);
	struct clist	*rcls = *(xdrp->xp_rcl_next);
	
	struct clist	*wcle = *(xdrp->xp_wcl_next);
	struct clist	*wcls = *(xdrp->xp_wcl_next);

	struct clist	cl;
	bool_t		retval = TRUE;
	u_int32_t	total_len = len;
	u_int32_t	cur_offset = 0;
	u_int32_t	total_segments = 0;
	u_int32_t	actual_segments = 0;
	u_int32_t	status = RDMA_SUCCESS;
	u_int32_t	alen = 0;
	u_int32_t	xpoff;

	while (rcle) {
		total_segments++;
		rcle = rcle->c_next;
	}

	rcle = *(xdrp->xp_rcl_next);

	if (xdrp->xp_off) {
		xpoff = xdrp->xp_off;
	} else {
		xpoff = ((uintptr_t)xdrp->xp_offp - (uintptr_t)xdrs->x_base);
	}

	/* MODIFIED by dwyane @ 2011-01-07 */
	if(rcle){
		PRINTF_INFO("c_xdroff is %u\n", rcle->c_xdroff);
		PRINTF_INFO("c_smemhandle.mrc_rmr is %u\n", rcle->c_smemhandle.mrc_rmr);
		PRINTF_INFO("c_len is %u\n", rcle->c_len);
		PRINTF_INFO("w.c_saddr is %llu\n", rcle->w.c_saddr);
	}
	/* END */

	/*
	 * If there was a chunk at the current offset, then setup a read
	 * chunk list which records the destination address and length
	 * and will RDMA READ the data in later.
	 */
	 
#if 1
	if (rcle != NULL && rcle->c_xdroff == xpoff) {
		if(wcle!=NULL)
			PRINTF_ERR("err occurs in %s %d",__func__,__LINE__);
		for (actual_segments = 0;
		    actual_segments < total_segments; actual_segments++) {

			if (total_len <= 0)
				break;

			if (status != RDMA_SUCCESS)
				goto out1;

			rcle->u.c_daddr = (u_int64_t)(uintptr_t)addr + cur_offset;
			alen = 0;
			if (rcle->c_len > total_len) {
				alen = rcle->c_len;
				rcle->c_len = total_len;
			}
			if (!alen)
				xdrp->xp_rcl_next = &rcle->c_next;

			cur_offset += rcle->c_len;
			total_len -= rcle->c_len;

			if ((total_segments - actual_segments - 1) == 0 &&
			    total_len > 0) {
				//DTRACE_PROBE(
				//    krpc__e__xdrrdma_getbytes_chunktooshort);
				retval = FALSE;
			}

			if ((total_segments - actual_segments - 1) > 0 &&
			    total_len == 0) {
				//DTRACE_PROBE2(krpc__e__xdrrdma_getbytes_toobig,
				//    int, total_segments, int, actual_segments);
			}

			/*
			 * RDMA READ the chunk data from the remote end.
			 * First prep the destination buffer by registering
			 * it, then RDMA READ the chunk data. Since we are
			 * doing streaming memory, sync the destination
			 * buffer to CPU and deregister the buffer.
			 */
			if (xdrp->xp_conn == NULL) {
				return (FALSE);
			}
			cl = *rcle;
			cl.c_next = NULL;
			status = clist_register(xdrp->xp_conn, &cl,
			    CLIST_REG_DST);
			if (status != RDMA_SUCCESS) {
				retval = FALSE;
				/*
				 * Deregister the previous chunks
				 * before return
				 */
				goto out1;
			}

			rcle->c_dmemhandle = cl.c_dmemhandle;
			rcle->c_dsynchandle = cl.c_dsynchandle;

			/*
			 * Now read the chunk in
			 */
			status = dw_read(xdrp->xp_conn, &cl);
#endif

			if (status != RDMA_SUCCESS) {
				PRINTF_ERR("err while dw_read\n");
				retval = FALSE;
			}

			rcle = rcle->c_next;

		}
			

out1:

		/*
		 * Deregister the chunks
		 */
		rcle = rcls;
		while (actual_segments != 0) {
			cl = *rcle;
			cl.c_next = NULL;

			cl.c_regtype = CLIST_REG_DST;
			(void) clist_deregister(&cl);

			rcle = rcle->c_next;
			actual_segments--;
		}

		if (alen) {
			rcle = *(xdrp->xp_rcl_next);
			rcle->w.c_saddr =
			    (u_int64_t)(uintptr_t)rcle->w.c_saddr + rcle->c_len;
			rcle->c_len = alen - rcle->c_len;
		}

		return (retval);
	}

	if (wcle != NULL) {
		for (actual_segments = 0;actual_segments < total_segments; actual_segments++) {
			alen = 0;
			if (wcle->c_len > total_len) {
				alen = wcle->c_len;
				wcle->c_len = total_len;
			}
			if (!alen)
				xdrp->xp_wcl_next = &wcle->c_next;

			cur_offset += wcle->c_len;
			total_len -= wcle->c_len;

			if ((total_segments - actual_segments - 1) == 0 &&
			    total_len > 0) {
				retval = FALSE;
			}

			wcle = wcle->c_next;
			
		}
		return TRUE;
	}

	if ((xdrs->x_handy -= len) < 0)
		return (FALSE);
	//printf("Bytes string is %.10s, len is %d.In %s: %d\n",xdrp->xp_offp,len,__func__,__LINE__);
	bcopy(xdrp->xp_offp, addr, len);

	xdrp->xp_offp += len;

	if (xdrp->xp_off != 0)
		xdrp->xp_off += len;

	return (TRUE);
}

/*
 * ENCODE some bytes into an XDR stream xp_min_chunk = 0, means the stream of
 * bytes contain no chunks to seperate out, and if the bytes do not fit in
 * the supplied buffer, grow the buffer and free the old buffer.
 */
static	bool_t
xdrrdma_putbytes(XDR *xdrs, caddr_t addr, int len)
{
	xrdma_private_t	*xdrp = (xrdma_private_t *)(xdrs->x_private);
	/*
	 * Is this stream accepting chunks?
	 * If so, does the either of the two following conditions exist?
	 * - length of bytes to encode is greater than the min chunk size?
	 * - remaining space in this stream is shorter than length of
	 *   bytes to encode?
	 *
	 * If the above exists, then create a chunk for this encoding
	 * and save the addresses, etc.
	 */
	//printf("Bytes string is %.10s, len is %d.In %s: %d\n",addr,len,__func__,__LINE__);
	if (xdrp->xp_flags & XDR_RDMA_CHUNK &&
	    ((xdrp->xp_min_chunk != 0 &&
	    len >= xdrp->xp_min_chunk) ||
	    (xdrs->x_handy - len  < 0))) {
		struct clist	*cle;
		int	offset = (uintptr_t)xdrp->xp_offp - (uintptr_t)xdrs->x_base;

		cle = clist_alloc();
		cle->c_xdroff = offset;
		cle->c_len = len;
		cle->w.c_saddr = (u_int64_t)(uintptr_t)addr;
		cle->c_next = NULL;

		/* MODIFIED by dwyane @ 2011-01-06 */
		PRINTF_INFO("in %s, pointer of clist is %p\n", __func__, cle);
		/* END */

		*(xdrp->xp_rcl_next) = cle;
		xdrp->xp_rcl_next = &(cle->c_next);

		return (TRUE);
	}
	/* Is there enough space to encode what is left? */
	if ((xdrs->x_handy -= len) < 0) {
		return (FALSE);
	}
	bcopy(addr, xdrp->xp_offp, len);
	xdrp->xp_offp += len;

	return (TRUE);
}


static	bool_t
xdrrdma_putbytes_svc(XDR *xdrs, caddr_t addr, int len)
{
	xrdma_private_t	*xdrp = (xrdma_private_t *)(xdrs->x_private);
	/*
	 * Is this stream accepting chunks?
	 * If so, does the either of the two following conditions exist?
	 * - length of bytes to encode is greater than the min chunk size?
	 * - remaining space in this stream is shorter than length of
	 *   bytes to encode?
	 *
	 * If the above exists, then create a chunk for this encoding
	 * and save the addresses, etc.
	 */
	//printf("Bytes string is %.10s, len is %d.In %s: %d\n",addr,len,__func__,__LINE__);
	printf("x_handy is %d, len is %d in %s:%d\n",xdrs->x_handy,len,__func__,__LINE__);
	if (xdrp->xp_flags & XDR_RDMA_CHUNK &&
	    ((xdrp->xp_min_chunk != 0 &&
	    len >= xdrp->xp_min_chunk) ||
	    (xdrs->x_handy - len  < 0))) {
		struct clist	*cle = *(xdrp->xp_wcl_next);
		int	offset = (uintptr_t)xdrp->xp_offp - (uintptr_t)xdrs->x_base;

		//cle = clist_alloc();
		//if(cle->c_xdroff != offset)
			//PRINTF_ERR("cle->c_xdroff != offset, In %s:%d\n",__func__,__LINE__);
		//cle->c_len = len;
		if(cle->c_len!=len)
			PRINTF_ERR("cle->c_len!=len, In %d:%d\n",__func__,__LINE__);
		cle->w.c_saddr = (u_int64_t)(uintptr_t)addr;
		//cle->c_next = NULL;

		/* MODIFIED by dwyane @ 2011-01-06 */
		PRINTF_INFO("in %s, pointer of clist is %p\n", __func__, cle);
		/* END */

		//*(xdrp->xp_wcl_next) = cle;
		xdrp->xp_wcl_next = &(cle->c_next);

		return (TRUE);
	}
	/* Is there enough space to encode what is left? */
	if ((xdrs->x_handy -= len) < 0) {
		printf("err occurs in %s:%d\n",__func__,__LINE__);
		return (FALSE);
	}
	bcopy(addr, xdrp->xp_offp, len);
	xdrp->xp_offp += len;

	return (TRUE);
}


u_int32_t
xdrrdma_getpos(XDR *xdrs)
{
	xrdma_private_t *xdrp = (xrdma_private_t *)(xdrs->x_private);

	return ((u_int32_t)((uintptr_t)xdrp->xp_offp - (uintptr_t)xdrs->x_base));
}

bool_t
xdrrdma_setpos(XDR *xdrs, u_int32_t pos)
{
	xrdma_private_t	*xdrp = (xrdma_private_t *)(xdrs->x_private);

	caddr_t		newaddr = xdrs->x_base + pos;
	caddr_t		lastaddr = xdrp->xp_offp + xdrs->x_handy;
	ptrdiff_t	diff;

	if (newaddr > lastaddr)
		return (FALSE);

	xdrp->xp_offp = newaddr;
	diff = lastaddr - newaddr;
	xdrs->x_handy = (int)diff;

	return (TRUE);
}

/* ARGSUSED */
static rpc_inline_t *
xdrrdma_inline(XDR *xdrs, int len)
{
	rpc_inline_t	*buf = NULL;
	xrdma_private_t	*xdrp = (xrdma_private_t *)(xdrs->x_private);
	struct clist	*cle = *(xdrp->xp_rcl_next);

	if (xdrs->x_op == XDR_DECODE) {
		/*
		 * Since chunks aren't in-line, check to see whether there is
		 * a chunk in the inline range.
		 */
		if (cle != NULL &&
		    cle->c_xdroff <= ((uintptr_t)xdrp->xp_offp - (uintptr_t)xdrs->x_base + len))
			return (NULL);
	}

	/* LINTED pointer alignment */
	buf = (rpc_inline_t *)xdrp->xp_offp;
	/*
	if (!IS_P2ALIGNED(buf, sizeof (int32_t)))
		return (NULL);
	*/

	if ((xdrs->x_handy < len) || (xdrp->xp_min_chunk != 0 &&
	    len >= xdrp->xp_min_chunk)) {
		return (NULL);
	} else {
		xdrs->x_handy -= len;
		xdrp->xp_offp += len;
		return (buf);
	}
}

static	bool_t
xdrrdma_control(XDR *xdrs, int request, void *info)
{
	int32_t		*int32p;
	int		len, i;
	u_int32_t		in_flags;
	xrdma_private_t	*xdrp = (xrdma_private_t *)(xdrs->x_private);
	rdma_chunkinfo_t *rcip = NULL;
	rdma_wlist_conn_info_t *rwcip = NULL;
	rdma_chunkinfo_lengths_t *rcilp = NULL;
	struct uio *uiop;
	struct clist	*rwl = NULL;
	struct clist	*prev = NULL;

	switch (request) {
	case XDR_PEEK:
		/*
		 * Return the next 4 byte unit in the XDR stream.
		 */
		if (xdrs->x_handy < sizeof (int32_t))
			return (FALSE);

		int32p = (int32_t *)info;
		*int32p = (int32_t)ntohl((uint32_t)
		    (*((int32_t *)(xdrp->xp_offp))));

		return (TRUE);

	case XDR_SKIPBYTES:
		/*
		 * Skip the next N bytes in the XDR stream.
		 */
		int32p = (int32_t *)info;
		len = RNDUP((int)(*int32p));
		if ((xdrs->x_handy -= len) < 0)
			return (FALSE);
		xdrp->xp_offp += len;

		return (TRUE);

	case XDR_RDMA_SET_FLAGS:
		/*
		 * Set the flags provided in the *info in xp_flags for rdma
		 * xdr stream control.
		 */
		int32p = (int32_t *)info;
		in_flags = (u_int32_t)(*int32p);

		xdrp->xp_flags |= in_flags;
		return (TRUE);

	case XDR_RDMA_GET_FLAGS:
		/*
		 * Get the flags provided in xp_flags return through *info
		 */
		int32p = (int32_t *)info;

		*int32p = (int32_t)xdrp->xp_flags;
		return (TRUE);

	case XDR_RDMA_GET_CHUNK_LEN:
		rcilp = (rdma_chunkinfo_lengths_t *)info;
		rcilp->rcil_len = xdrp->xp_reply_chunk_len;
		rcilp->rcil_len_alt = xdrp->xp_reply_chunk_len_alt;

		return (TRUE);
#if 0

	case XDR_RDMA_ADD_CHUNK:
		/*
		 * Store wlist information
		 */

		rcip = (rdma_chunkinfo_t *)info;

		switch (rcip->rci_type) {
		case RCI_WRITE_UIO_CHUNK:
			xdrp->xp_reply_chunk_len_alt += rcip->rci_len;

			if (rcip->rci_len < xdrp->xp_min_chunk) {
				xdrp->xp_wcl = NULL;
				*(rcip->rci_clpp) = NULL;
				return (TRUE);
			}
			uiop = rcip->rci_a.rci_uiop;

			for (i = 0; i < uiop->uio_iovcnt; i++) {
				rwl = clist_alloc();
				rwl->c_len = uiop->uio_iov[i].iov_len;
				rwl->u.c_daddr =
				    (u_int64_t)(uintptr_t)
				    (uiop->uio_iov[i].iov_base);
				/*
				 * if userspace address, put adspace ptr in
				 * clist. If not, then do nothing since it's
				 * already set to NULL (from kmem_zalloc)
				 */
				if (uiop->uio_segflg == UIO_USERSPACE) {
					rwl->c_adspc = ttoproc(curthread)->p_as;
				}

				if (prev == NULL)
					prev = rwl;
				else {
					prev->c_next = rwl;
					prev = rwl;
				}
			}

			rwl->c_next = NULL;
			xdrp->xp_wcl = rwl;
			*(rcip->rci_clpp) = rwl;

			break;

		case RCI_WRITE_ADDR_CHUNK:
			rwl = clist_alloc();

			rwl->c_len = rcip->rci_len;
			rwl->u.c_daddr3 = rcip->rci_a.rci_addr;
			rwl->c_next = NULL;
			xdrp->xp_reply_chunk_len_alt += rcip->rci_len;

			xdrp->xp_wcl = rwl;
			*(rcip->rci_clpp) = rwl;

			break;

		case RCI_REPLY_CHUNK:
			xdrp->xp_reply_chunk_len += rcip->rci_len;
			break;
		}
		return (TRUE);
#endif

	case XDR_RDMA_GET_WLIST:
		*((struct clist **)info) = xdrp->xp_wcl;
		return (TRUE);

	case XDR_RDMA_SET_WLIST:
		xdrp->xp_wcl = (struct clist *)info;
		return (TRUE);

	case XDR_RDMA_GET_RLIST:
		*((struct clist **)info) = xdrp->xp_rcl;
		return (TRUE);

	case XDR_RDMA_GET_WCINFO:
		rwcip = (rdma_wlist_conn_info_t *)info;

		rwcip->rwci_wlist = xdrp->xp_wcl;
		rwcip->rwci_conn= xdrp->xp_conn;

		return (TRUE);

	default:
		return (FALSE);
	}
}

bool_t xdr_do_clist(XDR *, clist **);

/*
 * Not all fields in struct clist are interesting to the RPC over RDMA
 * protocol. Only XDR the interesting fields.
 */
bool_t
xdr_clist(XDR *xdrs, clist *objp)
{
	/* MODIFIED by dwyane @ 2011-01-07 */
	if(xdrs->x_op == XDR_ENCODE){
		PRINTF_INFO("c_xdroff is %u\n", objp->c_xdroff);
		PRINTF_INFO("c_smemhandle.mrc_rmr is %u\n", objp->c_smemhandle.mrc_rmr);
		PRINTF_INFO("c_len is %u\n", objp->c_len);
		PRINTF_INFO("w.c_saddr is %llu\n", objp->w.c_saddr);
	}
	/* END */
	
	if (!xdr_u_int32_t(xdrs, &objp->c_xdroff))
		return (FALSE);
	if (!xdr_u_int32_t(xdrs, &objp->c_smemhandle.mrc_rmr))
		return (FALSE);
	if (!xdr_u_int32_t(xdrs, &objp->c_len))
		return (FALSE);
	if (!xdr_u_int64_t(xdrs, &objp->w.c_saddr))
		return (FALSE);
	if (!xdr_do_clist(xdrs, &objp->c_next))
		return (FALSE);
	return (TRUE);
}

/*
 * The following two functions are forms of xdr_pointer()
 * and xdr_reference(). Since the generic versions just
 * kmem_alloc() a new clist, we actually want to use the
 * rdma_clist kmem_cache.
 */

/*
 * Generate or free a clist structure from the
 * kmem_cache "rdma_clist"
 */
bool_t
xdr_ref_clist(XDR *xdrs, caddr_t *pp)
{
	caddr_t loc = *pp;
	bool_t stat;

	if (loc == NULL) {
		switch (xdrs->x_op) {
		case XDR_FREE:
			return (TRUE);

		case XDR_DECODE:
			*pp = loc = (caddr_t)clist_alloc();
			break;

		case XDR_ENCODE:
			//ASSERT(loc);
			break;
		}
	}

	stat = xdr_clist(xdrs, (struct clist *)loc);

	if (xdrs->x_op == XDR_FREE) {
		mem_free((struct clist *)loc, sizeof(struct clist));
		*pp = NULL;
	}
	return (stat);
}

/*
 * XDR a pointer to a possibly recursive clist. This differs
 * with xdr_reference in that it can serialize/deserialiaze
 * trees correctly.
 *
 *  What is sent is actually a union:
 *
 *  union object_pointer switch (boolean b) {
 *  case TRUE: object_data data;
 *  case FALSE: void nothing;
 *  }
 *
 * > objpp: Pointer to the pointer to the object.
 *
 */

bool_t
xdr_do_clist(XDR *xdrs, clist **objpp)
{
	bool_t more_data;

	more_data = (*objpp != NULL);
	if (!xdr_bool(xdrs, &more_data))
		return (FALSE);
	if (!more_data) {
		*objpp = NULL;
		return (TRUE);
	}
	return (xdr_ref_clist(xdrs, (caddr_t *)objpp));
}

u_int32_t
xdr_getbufsize(XDR *xdrs)
{
	xrdma_private_t *xdrp = (xrdma_private_t *)(xdrs->x_private);

	return ((u_int32_t)xdrp->xp_buf_size);
}

/* ARGSUSED */
bool_t
xdr_encode_rlist_svc(XDR *xdrs, clist *rlist)
{
	bool_t	vfalse = FALSE;

	//ASSERT(rlist == NULL);
	return (xdr_bool(xdrs, &vfalse));
}

bool_t
xdr_encode_wlist(CONN *conn,XDR *xdrs, clist *w,clist_dstsrc CLIST_TYPE)
{
	bool_t		vfalse = FALSE, vtrue = TRUE;
	int		i;
	u_int32_t		num_segment = 0;
	struct clist	*cl;
	int status;
	/* does a wlist exist? */
	if (w == NULL) {
		return (xdr_bool(xdrs, &vfalse));
	}
	/* Encode N consecutive segments, 1, N, HLOO, ..., HLOO, 0 */

	if (status != RDMA_SUCCESS) {
		PRINTF_ERR("in %s, return CLNT_RDMA_FAIL\n", __func__);
		return (CLNT_RDMA_FAIL);
	}

	if (!xdr_bool(xdrs, &vtrue))
		return (FALSE);

	for (cl = w; cl != NULL; cl = cl->c_next) {
		num_segment++;
	}

	if (!xdr_u_int32_t(xdrs, &num_segment))
		return (FALSE);
	for (i = 0; i < num_segment; i++) {
		status = clist_register(conn, w, CLIST_TYPE);

		//DTRACE_PROBE1(krpc__i__xdr_encode_wlist_len, uint_t, w->c_len);

		if (!xdr_u_int32_t(xdrs, &w->c_dmemhandle.mrc_rmr))
			return (FALSE);

		if (!xdr_u_int32_t(xdrs, &w->c_len))
			return (FALSE);

		if (!xdr_u_int64_t(xdrs, &w->u.c_daddr))
			return (FALSE);

		w = w->c_next;
	}

	if (!xdr_bool(xdrs, &vfalse))
		return (FALSE);

	return (TRUE);
}


/*
 * Conditionally decode a RDMA WRITE chunk list from XDR stream.
 *
 * If the next boolean in the XDR stream is false there is no
 * RDMA WRITE chunk list present. Otherwise iterate over the
 * array and for each entry: allocate a struct clist and decode.
 * Pass back an indication via wlist_exists if we have seen a
 * RDMA WRITE chunk list.
 */
bool_t
xdr_decode_wlist(XDR *xdrs, struct clist **w, bool_t *wlist_exists)
{
	struct clist	*tmp;
	bool_t		more = FALSE;
	uint32_t	seg_array_len;
	uint32_t	i;

	if (!xdr_bool(xdrs, &more))
		return (FALSE);

	/* is there a wlist? */
	if (more == FALSE) {
		*wlist_exists = FALSE;
		return (TRUE);
	}
	*wlist_exists = TRUE;

	if (!xdr_u_int32_t(xdrs, &seg_array_len))
		return (FALSE);

	tmp = *w = clist_alloc();
	for (i = 0; i < seg_array_len; i++) {

		if (!xdr_u_int32_t(xdrs, &tmp->c_dmemhandle.mrc_rmr))
			return (FALSE);
		if (!xdr_u_int32_t(xdrs, &tmp->c_len))
			return (FALSE);

		//DTRACE_PROBE1(krpc__i__xdr_decode_wlist_len,
		    //uint_t, tmp->c_len);

		if (!xdr_u_int64_t(xdrs, &tmp->u.c_daddr))
			return (FALSE);
		if (i < seg_array_len - 1) {
			tmp->c_next = clist_alloc();
			tmp = tmp->c_next;
		} else {
			tmp->c_next = NULL;
		}
	}

	more = FALSE;
	if (!xdr_bool(xdrs, &more))
		return (FALSE);

	return (TRUE);
}

/*
 * Server side RDMA WRITE list decode.
 * XDR context is memory ops
 */
bool_t
xdr_decode_wlist_svc(XDR *xdrs, struct clist **wclp, bool_t *wwl,
    u_int32_t *total_length, CONN *conn)
{
	struct clist	*first, *ncl;
	char		*memp;
	uint32_t	num_wclist;
	uint32_t	wcl_length = 0;
	uint32_t	i;
	bool_t		more = FALSE;

	*wclp = NULL;
	*wwl = FALSE;
	*total_length = 0;

	if (!xdr_bool(xdrs, &more)) {
		return (FALSE);
	}

	if (more == FALSE) {
		return (TRUE);
	}

	*wwl = TRUE;

	if (!xdr_u_int32_t(xdrs, &num_wclist)) {
//		DTRACE_PROBE(krpc__e__xdrrdma__wlistsvc__listlength);
		return (FALSE);
	}

	first = ncl = clist_alloc();

	for (i = 0; i < num_wclist; i++) {

		if (!xdr_u_int32_t(xdrs, &ncl->c_dmemhandle.mrc_rmr))
			goto err_out;
		if (!xdr_u_int32_t(xdrs, &ncl->c_len))
			goto err_out;
		if (!xdr_u_int64_t(xdrs, &ncl->u.c_daddr))
			goto err_out;

		if (ncl->c_len > MAX_SVC_XFER_SIZE) {
			//DTRACE_PROBE(
			//    krpc__e__xdrrdma__wlistsvc__chunklist_toobig);
			ncl->c_len = MAX_SVC_XFER_SIZE;
		}

		//DTRACE_PROBE1(krpc__i__xdr_decode_wlist_svc_len,
		    //uint_t, ncl->c_len);

		wcl_length += ncl->c_len;

		if (i < num_wclist - 1) {
			ncl->c_next = clist_alloc();
			ncl = ncl->c_next;
		}
	}

	if (!xdr_bool(xdrs, &more))
		goto err_out;
#if 0
	first->rb_longbuf.type = RDMA_LONG_BUFFER;
	first->rb_longbuf.len =
	    wcl_length > WCL_BUF_LEN ? wcl_length : WCL_BUF_LEN;

	if (rdma_buf_alloc(conn, &first->rb_longbuf)) {
		clist_free(first);
		PRINTF_ERR("rdma_buf_alloc failure in %s:%d\n",__func__,__LINE__);
		first = NULL;
		return (FALSE);
	}

	memp = first->rb_longbuf.addr;

	ncl = first;
	for (i = 0; i < num_wclist; i++) {
		ncl->w.c_saddr3 = (caddr_t)memp;
		memp += ncl->c_len;
		ncl = ncl->c_next;
	}
#endif
	*wclp = first;
	*total_length = wcl_length;
	return (TRUE);

err_out:
	clist_free(first);
	first = NULL;
	return (FALSE);
}

#if 1

/*
 * XDR decode the long reply write chunk.
 */
bool_t
xdr_decode_reply_wchunk(XDR *xdrs, struct clist **clist)
{
	bool_t		have_rchunk = FALSE;
	struct clist	*first = NULL, *ncl = NULL;
	u_int32_t	num_wclist;
	u_int32_t	i;

	if (!xdr_bool(xdrs, &have_rchunk))
		return (FALSE);

	if (have_rchunk == FALSE)
		return (TRUE);

	if (!xdr_u_int32_t(xdrs, &num_wclist)) {
		//DTRACE_PROBE(krpc__e__xdrrdma__replywchunk__listlength);
		return (FALSE);
	}

	if (num_wclist == 0) {
		return (FALSE);
	}

	first = ncl = clist_alloc();

	for (i = 0; i < num_wclist; i++) {

		if (i > 0) {
			ncl->c_next = clist_alloc();
			ncl = ncl->c_next;
		}

		if (!xdr_u_int32_t(xdrs, &ncl->c_dmemhandle.mrc_rmr))
			goto err_out;
		if (!xdr_u_int32_t(xdrs, &ncl->c_len))
			goto err_out;
		if (!xdr_u_int64_t(xdrs, &ncl->u.c_daddr))
			goto err_out;

		if (ncl->c_len > MAX_SVC_XFER_SIZE) {
			//DTRACE_PROBE(
			    //krpc__e__xdrrdma__replywchunk__chunklist_toobig);
			ncl->c_len = MAX_SVC_XFER_SIZE;
		}
		if (!(ncl->c_dmemhandle.mrc_rmr &&
		    (ncl->c_len > 0) && ncl->u.c_daddr));
			//DTRACE_PROBE(
			    //krpc__e__xdrrdma__replywchunk__invalid_segaddr);

		//DTRACE_PROBE1(krpc__i__xdr_decode_reply_wchunk_c_len,
		    //uint32_t, ncl->c_len);

	}
	*clist = first;
	return (TRUE);

err_out:
	clist_free(first);
	first = NULL;
	return (FALSE);
}

#endif


bool_t
xdr_encode_reply_wchunk(XDR *xdrs,
    struct clist *cl_longreply, u_int32_t seg_array_len)
{
	int		i;
	bool_t		long_reply_exists = TRUE;
	u_int32_t	length;
	u_int64_t		offset;

	if (seg_array_len > 0) {
		if (!xdr_bool(xdrs, &long_reply_exists))
			return (FALSE);
		if (!xdr_u_int32_t(xdrs, &seg_array_len))
			return (FALSE);

		for (i = 0; i < seg_array_len; i++) {
			if (!cl_longreply)
				return (FALSE);
			length = cl_longreply->c_len;
			offset = (u_int64_t) cl_longreply->u.c_daddr;

			//DTRACE_PROBE1(
			    //krpc__i__xdr_encode_reply_wchunk_c_len,
			    //uint32_t, length);

			if (!xdr_u_int32_t(xdrs,
			    &cl_longreply->c_dmemhandle.mrc_rmr))
				return (FALSE);
			if (!xdr_u_int32_t(xdrs, &length))
				return (FALSE);
			if (!xdr_u_int64_t(xdrs, &offset))
				return (FALSE);
			cl_longreply = cl_longreply->c_next;
		}
	} else {
		long_reply_exists = FALSE;
		if (!xdr_bool(xdrs, &long_reply_exists))
			return (FALSE);
	}
	return (TRUE);
}
/*
bool_t
xdrrdma_setbuf(XDR *xdrs_rpc,
    caddr_t xdr_location)
{
   ((xrdma_private_t *)xdrs_rpc->x_private)->xp_rcl->rb_longbuf.addr=xdr_location;
   return (TRUE);
}*/

#if 0
bool_t
xdrrdma_read_from_client(struct clist *rlist, CONN **conn, uint_t count)
{
	struct clist	*rdclist;
	struct clist	cl;
	uint_t		total_len = 0;
	uint32_t	status;
	bool_t		retval = TRUE;

	rlist->rb_longbuf.type = RDMA_LONG_BUFFER;
	rlist->rb_longbuf.len =
	    count > RCL_BUF_LEN ? count : RCL_BUF_LEN;

	if (rdma_buf_alloc(*conn, &rlist->rb_longbuf)) {
		return (FALSE);
	}

	/*
	 * The entire buffer is registered with the first chunk.
	 * Later chunks will use the same registered memory handle.
	 */

	cl = *rlist;
	cl.c_next = NULL;
	if (clist_register(*conn, &cl, CLIST_REG_DST) != RDMA_SUCCESS) {
		rdma_buf_free(*conn, &rlist->rb_longbuf);
		//DTRACE_PROBE(
		    //krpc__e__xdrrdma__readfromclient__clist__reg);
		return (FALSE);
	}

	rlist->c_regtype = CLIST_REG_DST;
	rlist->c_dmemhandle = cl.c_dmemhandle;
	rlist->c_dsynchandle = cl.c_dsynchandle;

	for (rdclist = rlist;
	    rdclist != NULL; rdclist = rdclist->c_next) {
		total_len += rdclist->c_len;

		rdclist->u.c_daddr3 =
		    (caddr_t)((char *)rlist->rb_longbuf.addr +
		    (uint64) rdclist->u.c_daddr3);

		cl = (*rdclist);
		cl.c_next = NULL;

		/*
		 * Use the same memory handle for all the chunks
		 */
		cl.c_dmemhandle = rlist->c_dmemhandle;
		cl.c_dsynchandle = rlist->c_dsynchandle;


		//DTRACE_PROBE1(krpc__i__xdrrdma__readfromclient__buflen,
		    //int, rdclist->c_len);

		/*
		 * Now read the chunk in
		 */
		if (rdclist->c_next == NULL) {
			status = RDMA_READ(*conn, &cl, WAIT);
		} else {
			status = RDMA_READ(*conn, &cl, NOWAIT);
		}
		if (status != RDMA_SUCCESS) {
			DTRACE_PROBE(
			    krpc__e__xdrrdma__readfromclient__readfailed);
			rdma_buf_free(*conn, &rlist->rb_longbuf);
			return (FALSE);
		}
	}

	cl = (*rlist);
	cl.c_next = NULL;
	cl.c_len = total_len;
	if (clist_syncmem(*conn, &cl, CLIST_REG_DST) != RDMA_SUCCESS) {
		retval = FALSE;
	}
	return (retval);
}

#endif

bool_t
xdrrdma_free_clist(CONN *conn, struct clist *clp)
{
	rdma_buf_free(conn, &clp->rb_longbuf);
	clist_free(clp);
	clp = NULL;
	return (TRUE);
}

#if 0
bool_t
xdrrdma_send_read_data(XDR *xdrs, u_int32_t data_len, struct clist *wcl)
{
	int status;
	xrdma_private_t	*xdrp = (xrdma_private_t *)(xdrs->x_private);
	struct xdr_ops *xops = xdrrdma_xops();
	struct clist *tcl, *wrcl, *cl;
	struct clist fcl;
	int rndup_present, rnduplen;

	rndup_present = 0;
	wrcl = NULL;

	/* caller is doing a sizeof */
	if (xdrs->x_ops != &xdrrdma_ops || xdrs->x_ops == xops)
		return (TRUE);

	/* copy of the first chunk */
	fcl = *wcl;
	fcl.c_next = NULL;

	/*
	 * The entire buffer is registered with the first chunk.
	 * Later chunks will use the same registered memory handle.
	 */

	status = clist_register(xdrp->xp_conn, &fcl, CLIST_REG_SOURCE);
	if (status != RDMA_SUCCESS) {
		return (FALSE);
	}

	wcl->c_regtype = CLIST_REG_SOURCE;
	wcl->c_smemhandle = fcl.c_smemhandle;
	wcl->c_ssynchandle = fcl.c_ssynchandle;

	/*
	 * Only transfer the read data ignoring any trailing
	 * roundup chunks. A bit of work, but it saves an
	 * unnecessary extra RDMA_WRITE containing only
	 * roundup bytes.
	 */

	rnduplen = clist_len(wcl) - data_len;

	if (rnduplen) {

		tcl = wcl->c_next;

		/*
		 * Check if there is a trailing roundup chunk
		 */
		while (tcl) {
			if ((tcl->c_next == NULL) && (tcl->c_len == rnduplen)) {
				rndup_present = 1;
				break;
			}
			tcl = tcl->c_next;
		}

		/*
		 * Make a copy chunk list skipping the last chunk
		 */
		if (rndup_present) {
			cl = wcl;
			tcl = NULL;
			while (cl) {
				if (tcl == NULL) {
					tcl = clist_alloc();
					wrcl = tcl;
				} else {
					tcl->c_next = clist_alloc();
					tcl = tcl->c_next;
				}

				*tcl = *cl;
				cl = cl->c_next;
				/* last chunk */
				if (cl->c_next == NULL)
					break;
			}
			tcl->c_next = NULL;
		}
	}

	if (wrcl == NULL) {
		/* No roundup chunks */
		wrcl = wcl;
	}

	/*
	 * Set the registered memory handles for the
	 * rest of the chunks same as the first chunk.
	 */
	tcl = wrcl->c_next;
	while (tcl) {
		tcl->c_smemhandle = fcl.c_smemhandle;
		tcl->c_ssynchandle = fcl.c_ssynchandle;
		tcl = tcl->c_next;
	}

	/*
	 * Sync the total len beginning from the first chunk.
	 */
	fcl.c_len = clist_len(wrcl);
	status = clist_syncmem(xdrp->xp_conn, &fcl, CLIST_REG_SOURCE);
	if (status != RDMA_SUCCESS) {
		return (FALSE);
	}

	status = RDMA_WRITE(xdrp->xp_conn, wrcl, WAIT);

	if (rndup_present)
		clist_free(wrcl);

	if (status != RDMA_SUCCESS) {
		return (FALSE);
	}

	return (TRUE);
}

#endif


#if 0
/*
 * Reads one chunk at a time
 */

static bool_t
xdrrdma_read_a_chunk(XDR *xdrs, CONN **conn)
{
	int status;
	int32_t len = 0;
	xrdma_private_t	*xdrp = (xrdma_private_t *)(xdrs->x_private);
	struct clist *cle = *(xdrp->xp_rcl_next);
	struct clist *rclp = xdrp->xp_rcl;
	struct clist *clp;

	/*
	 * len is used later to decide xdr offset in
	 * the chunk factoring any 4-byte XDR alignment
	 * (See read chunk example top of this file)
	 */
	while (rclp != cle) {
		len += rclp->c_len;
		rclp = rclp->c_next;
	}

	len = RNDUP(len) - len;

	//ASSERT(xdrs->x_handy <= 0);

	/*
	 * If this is the first chunk to contain the RPC
	 * message set xp_off to the xdr offset of the
	 * inline message.
	 */
	if (xdrp->xp_off == 0)
		xdrp->xp_off = (xdrp->xp_offp - xdrs->x_base);

	if (cle == NULL || (cle->c_xdroff != xdrp->xp_off))
		return (FALSE);

	/*
	 * Make a copy of the chunk to read from client.
	 * Chunks are read on demand, so read only one
	 * for now.
	 */

	rclp = clist_alloc();
	*rclp = *cle;
	rclp->c_next = NULL;

	xdrp->xp_rcl_next = &cle->c_next;

	/*
	 * If there is a roundup present, then skip those
	 * bytes when reading.
	 */
	if (len) {
		rclp->w.c_saddr =
		    (uint64)(uintptr_t)rclp->w.c_saddr + len;
			rclp->c_len = rclp->c_len - len;
	}

	status = xdrrdma_read_from_client(rclp, conn, rclp->c_len);

	if (status == FALSE) {
		clist_free(rclp);
		return (status);
	}

	xdrp->xp_offp = rclp->rb_longbuf.addr;
	xdrs->x_base = xdrp->xp_offp;
	xdrs->x_handy = rclp->c_len;

	/*
	 * This copy of read chunks containing the XDR
	 * message is freed later in xdrrdma_destroy()
	 */

	if (xdrp->xp_rcl_xdr) {
		/* Add the chunk to end of the list */
		clp = xdrp->xp_rcl_xdr;
		while (clp->c_next != NULL)
			clp = clp->c_next;
		clp->c_next = rclp;
	} else {
		xdrp->xp_rcl_xdr = rclp;
	}
	return (TRUE);
}

#endif

#if 0
static void
xdrrdma_free_xdr_chunks(CONN *conn, struct clist *xdr_rcl)
{
	struct clist *cl;

	(void) clist_deregister(conn, xdr_rcl);

	/*
	 * Read chunks containing parts XDR message are
	 * special: in case of multiple chunks each has
	 * its own buffer.
	 */

	cl = xdr_rcl;
	while (cl) {
		rdma_buf_free(conn, &cl->rb_longbuf);
		cl = cl->c_next;
	}

	clist_free(xdr_rcl);
}
#endif


/* END */

