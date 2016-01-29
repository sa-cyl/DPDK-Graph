
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
 * svc.c, Server-side remote procedure call interface.
 *
 * There are two sets of procedures here.  The xprt routines are
 * for handling transport handles.  The svc routines handle the
 * list of service routines.
 *
 * Copyright (C) 1984, Sun Microsystems, Inc.
 */
#include <pthread.h>

#include <reentrant.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <rpc/rpc.h>
#ifdef PORTMAP
#include <rpc/pmap_clnt.h>
#endif /* PORTMAP */

#include "rpc_com.h"

/* MODIFIED by dwyane @ 2010-12-21 */
#include <sys/epoll.h>
#include <rpc/list.h>
/* END */

/* MODIFIED by dwyane @ 2011-01-12 */
#include "dwyane.h"
/* END */

#define	RQCRED_SIZE	400	/* this size is excessive */

#define SVC_VERSQUIET 0x0001	/* keep quiet about vers mismatch */
#define version_keepquiet(xp) ((u_long)(xp)->xp_p3 & SVC_VERSQUIET)

#define max(a, b) (a > b ? a : b)

/*
 * The services list
 * Each entry represents a set of procedures (an rpc program).
 * The dispatch routine takes request structs and runs the
 * apropriate procedure.
 */
static struct svc_callout
{
  struct svc_callout *sc_next;
  rpcprog_t sc_prog;
  rpcvers_t sc_vers;
  char *sc_netid;
  void (*sc_dispatch) (struct svc_req *, SVCXPRT *);
} *svc_head;

extern rwlock_t svc_lock;
extern rwlock_t svc_fd_lock;
/* MODIFIED by dwyane @ 2010-12-20 */
extern rwlock_t pending_lock;
/* END */
#ifdef HAVE_LIBGSSAPI
extern struct svc_auth_ops svc_auth_gss_ops;
#endif

static struct svc_callout *svc_find (rpcprog_t, rpcvers_t,
				     struct svc_callout **, char *);
static void __xprt_do_unregister (SVCXPRT * xprt, bool_t dolock);

/* ***************  SVCXPRT related stuff **************** */

/*
 * Activate a transport handle.
 */
void
xprt_register (xprt)
     SVCXPRT *xprt;
{
  int sock;

  	/* MODIFIED by dwyane @ 2010-12-21 */
	struct epoll_event ev;
	/* END */

  assert (xprt != NULL);

  sock = xprt->xp_fd;

  rwlock_wrlock (&svc_fd_lock);
  if (__svc_xports == NULL)
    {
      __svc_xports = (SVCXPRT **) mem_alloc (FD_SETSIZE * sizeof (SVCXPRT *));
      if (__svc_xports == NULL)
	return;
      memset (__svc_xports, '\0', FD_SETSIZE * sizeof (SVCXPRT *));
    }

	/* MODIFIED by dwyane @ 2010-12-21 */
	if(!epfd){
		//PRINTF_INFO("create epfd\n");
		epfd=epoll_create(256);
		if(!epfd){
			printf("create epfd error\n");
			return;
		}
	}
	/* END */
  if (sock < FD_SETSIZE)
    {
      __svc_xports[sock] = xprt;
      FD_SET (sock, &svc_fdset);
	/* MODIFIED by dwyane @ 2010-12-21 */
	ev.data.fd	=	sock;
	ev.events	=	EPOLLIN|EPOLLET;
	epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &ev);  
	/* END */
      svc_maxfd = max (svc_maxfd, sock);
    }
  rwlock_unlock (&svc_fd_lock);
}

void
xprt_unregister (SVCXPRT * xprt)
{
  __xprt_do_unregister (xprt, TRUE);
}

void
__xprt_unregister_unlocked (SVCXPRT * xprt)
{
  __xprt_do_unregister (xprt, FALSE);
}

/*
 * De-activate a transport handle.
 */
static void
__xprt_do_unregister (xprt, dolock)
     SVCXPRT *xprt;
     bool_t dolock;
{
  int sock;

	/* MODIFIED by dwyane @ 2010-12-21 */
	struct epoll_event ev;
	/* END */

  assert (xprt != NULL);

  sock = xprt->xp_fd;

  if (dolock)
    rwlock_wrlock (&svc_fd_lock);
  if ((sock < FD_SETSIZE) && (__svc_xports[sock] == xprt))
    {
      __svc_xports[sock] = NULL;
      FD_CLR (sock, &svc_fdset);
	/* MODIFIED by dwyane @ 2010-12-21 */
	ev.data.fd	=	sock;
	ev.events	=	EPOLLIN|EPOLLET;
	epoll_ctl(epfd, EPOLL_CTL_DEL, sock, &ev);  
	/* END */
      if (sock >= svc_maxfd)
	{
	  for (svc_maxfd--; svc_maxfd >= 0; svc_maxfd--)
	    if (__svc_xports[svc_maxfd])
	      break;
	}
    }
  if (dolock)
    rwlock_unlock (&svc_fd_lock);
}

/*
 * Add a service program to the callout list.
 * The dispatch routine will be called when a rpc request for this
 * program number comes in.
 */
bool_t
svc_reg (xprt, prog, vers, dispatch, nconf)
     SVCXPRT *xprt;
     const rpcprog_t prog;
     const rpcvers_t vers;
     void (*dispatch) (struct svc_req *, SVCXPRT *);
     const struct netconfig *nconf;
{
  bool_t dummy;
  struct svc_callout *prev;
  struct svc_callout *s;
  struct netconfig *tnconf;
  char *netid = NULL;
  int flag = 0;

/* VARIABLES PROTECTED BY svc_lock: s, prev, svc_head */
  if (xprt->xp_netid)
    {
      netid = strdup (xprt->xp_netid);
      flag = 1;
    }
  else if (nconf && nconf->nc_netid)
    {
      netid = strdup (nconf->nc_netid);
      flag = 1;
    }
  else if ((tnconf = __rpcgettp (xprt->xp_fd)) != NULL)
    {
      netid = strdup (tnconf->nc_netid);
      flag = 1;
      freenetconfigent (tnconf);
    }				/* must have been created with svc_raw_create */
  if ((netid == NULL) && (flag == 1))
    {
      return (FALSE);
    }

  rwlock_wrlock (&svc_lock);
  if ((s = svc_find (prog, vers, &prev, netid)) != NULL)
    {
      if (netid)
	free (netid);
      if (s->sc_dispatch == dispatch)
	goto rpcb_it;		/* he is registering another xptr */
      rwlock_unlock (&svc_lock);
      return (FALSE);
    }
  s = mem_alloc (sizeof (struct svc_callout));
  if (s == NULL)
    {
      if (netid)
	free (netid);
      rwlock_unlock (&svc_lock);
      return (FALSE);
    }

  s->sc_prog = prog;
  s->sc_vers = vers;
  s->sc_dispatch = dispatch;
  s->sc_netid = netid;
  s->sc_next = svc_head;
  svc_head = s;

  if ((xprt->xp_netid == NULL) && (flag == 1) && netid)
    ((SVCXPRT *) xprt)->xp_netid = strdup (netid);

rpcb_it:
  rwlock_unlock (&svc_lock);
  /* now register the information with the local binder service */
  if (nconf)
    {
      /*LINTED const castaway */
      dummy = rpcb_set (prog, vers, (struct netconfig *) nconf,
			&((SVCXPRT *) xprt)->xp_ltaddr);
      return (dummy);
    }
  return (TRUE);
}

/*
 * Remove a service program from the callout list.
 */
void
svc_unreg (prog, vers)
     const rpcprog_t prog;
     const rpcvers_t vers;
{
  struct svc_callout *prev;
  struct svc_callout *s;

  /* unregister the information anyway */
  (void) rpcb_unset (prog, vers, NULL);
  rwlock_wrlock (&svc_lock);
  while ((s = svc_find (prog, vers, &prev, NULL)) != NULL)
    {
      if (prev == NULL)
	{
	  svc_head = s->sc_next;
	}
      else
	{
	  prev->sc_next = s->sc_next;
	}
      s->sc_next = NULL;
      if (s->sc_netid)
	mem_free (s->sc_netid, sizeof (s->sc_netid) + 1);
      mem_free (s, sizeof (struct svc_callout));
    }
  rwlock_unlock (&svc_lock);
}

/* ********************** CALLOUT list related stuff ************* */

#ifdef PORTMAP
/*
 * Add a service program to the callout list.
 * The dispatch routine will be called when a rpc request for this
 * program number comes in.
 */
bool_t
svc_register (xprt, prog, vers, dispatch, protocol)
     SVCXPRT *xprt;
     u_long prog;
     u_long vers;
     void (*dispatch) (struct svc_req *, SVCXPRT *);
     int protocol;
{
  struct svc_callout *prev;
  struct svc_callout *s;

  assert (xprt != NULL);
  assert (dispatch != NULL);

  if ((s = svc_find ((rpcprog_t) prog, (rpcvers_t) vers, &prev, NULL)) !=
      NULL)
    {
      if (s->sc_dispatch == dispatch)
	goto pmap_it;		/* he is registering another xptr */
      return (FALSE);
    }
  s = mem_alloc (sizeof (struct svc_callout));
  if (s == NULL)
    {
      return (FALSE);
    }
  s->sc_prog = (rpcprog_t) prog;
  s->sc_vers = (rpcvers_t) vers;
  s->sc_dispatch = dispatch;
  s->sc_next = svc_head;
  svc_head = s;
pmap_it:
  /* now register the information with the local binder service */
  /* MODIFIED by dwyane @ 2010-12-23 */
  if(protocol == 37549){
	//printf("RDMA transp reg success!\n");
	return(TRUE);
  }
/* END */
  if (protocol)
    {
      return (pmap_set (prog, vers, protocol, xprt->xp_port));
    }
  return (TRUE);
}

/*
 * Remove a service program from the callout list.
 */
void
svc_unregister (prog, vers)
     u_long prog;
     u_long vers;
{
  struct svc_callout *prev;
  struct svc_callout *s;

  if ((s = svc_find ((rpcprog_t) prog, (rpcvers_t) vers, &prev, NULL)) ==
      NULL)
    return;
  if (prev == NULL)
    {
      svc_head = s->sc_next;
    }
  else
    {
      prev->sc_next = s->sc_next;
    }
  s->sc_next = NULL;
  mem_free (s, sizeof (struct svc_callout));
  /* now unregister the information with the local binder service */
  (void) pmap_unset (prog, vers);
}
#endif /* PORTMAP */

/*
 * Search the callout list for a program number, return the callout
 * struct.
 */
static struct svc_callout *
svc_find (prog, vers, prev, netid)
     rpcprog_t prog;
     rpcvers_t vers;
     struct svc_callout **prev;
     char *netid;
{
  struct svc_callout *s, *p;

  assert (prev != NULL);

  p = NULL;
  for (s = svc_head; s != NULL; s = s->sc_next)
    {
      if (((s->sc_prog == prog) && (s->sc_vers == vers)) &&
	  ((netid == NULL) || (s->sc_netid == NULL) ||
	   (strcmp (netid, s->sc_netid) == 0)))
	break;
      p = s;
    }
  *prev = p;
  return (s);
}

/* ******************* REPLY GENERATION ROUTINES  ************ */

/*
 * Send a reply to an rpc request
 */
bool_t
svc_sendreply (xprt, xdr_results, xdr_location)
     SVCXPRT *xprt;
     xdrproc_t xdr_results;
     void *xdr_location;
{
  struct rpc_msg rply;
	//struct timeval tim1,tim2;
	//unsigned long used_time;
  assert (xprt != NULL);
  static bool_t retval;

  rply.rm_direction = REPLY;
  rply.rm_reply.rp_stat = MSG_ACCEPTED;
  rply.acpted_rply.ar_verf = xprt->xp_verf;
  rply.acpted_rply.ar_stat = SUCCESS;
  rply.acpted_rply.ar_results.where = xdr_location;
  PRINTF_INFO("%s:result value is %p:%d\n",__func__,
				xdr_location,*(int *)xdr_location);
  rply.acpted_rply.ar_results.proc = xdr_results;
  //gettimeofday(&tim1,NULL);
  retval = SVC_REPLY (xprt, &rply);
  //gettimeofday(&tim2,NULL);
  //used_time = (tim2.tv_sec-tim1.tv_sec)*1000000.0 + (tim2.tv_usec-tim1.tv_usec);
  //printf("SVC_REPLY used_time is %ul,In %s: %d\n",used_time,__func__,__LINE__);
  return (retval);
}

/*
 * No procedure error reply
 */
void
svcerr_noproc (xprt)
     SVCXPRT *xprt;
{
  struct rpc_msg rply;

  assert (xprt != NULL);

  rply.rm_direction = REPLY;
  rply.rm_reply.rp_stat = MSG_ACCEPTED;
  rply.acpted_rply.ar_verf = xprt->xp_verf;
  rply.acpted_rply.ar_stat = PROC_UNAVAIL;
  SVC_REPLY (xprt, &rply);
}

/*
 * Can't decode args error reply
 */
void
svcerr_decode (xprt)
     SVCXPRT *xprt;
{
  struct rpc_msg rply;

  assert (xprt != NULL);

  rply.rm_direction = REPLY;
  rply.rm_reply.rp_stat = MSG_ACCEPTED;
  rply.acpted_rply.ar_verf = xprt->xp_verf;
  rply.acpted_rply.ar_stat = GARBAGE_ARGS;
  SVC_REPLY (xprt, &rply);
}

/*
 * Some system error
 */
void
svcerr_systemerr (xprt)
     SVCXPRT *xprt;
{
  struct rpc_msg rply;

  assert (xprt != NULL);

  rply.rm_direction = REPLY;
  rply.rm_reply.rp_stat = MSG_ACCEPTED;
  rply.acpted_rply.ar_verf = xprt->xp_verf;
  rply.acpted_rply.ar_stat = SYSTEM_ERR;
  SVC_REPLY (xprt, &rply);
}

#if 0
/*
 * Tell RPC package to not complain about version errors to the client.	 This
 * is useful when revving broadcast protocols that sit on a fixed address.
 * There is really one (or should be only one) example of this kind of
 * protocol: the portmapper (or rpc binder).
 */
void
__svc_versquiet_on (xprt)
     SVCXPRT *xprt;
{
  u_long tmp;

  tmp = ((u_long) xprt->xp_p3) | SVC_VERSQUIET;
  xprt->xp_p3 = tmp;
}

void
__svc_versquiet_off (xprt)
     SVCXPRT *xprt;
{
  u_long tmp;

  tmp = ((u_long) xprt->xp_p3) & ~SVC_VERSQUIET;
  xprt->xp_p3 = tmp;
}

void
svc_versquiet (xprt)
     SVCXPRT *xprt;
{
  __svc_versquiet_on (xprt);
}

int
__svc_versquiet_get (xprt)
     SVCXPRT *xprt;
{
  return ((int) xprt->xp_p3) & SVC_VERSQUIET;
}
#endif

/*
 * Authentication error reply
 */
void
svcerr_auth (xprt, why)
     SVCXPRT *xprt;
     enum auth_stat why;
{
  struct rpc_msg rply;

  assert (xprt != NULL);

  rply.rm_direction = REPLY;
  rply.rm_reply.rp_stat = MSG_DENIED;
  rply.rjcted_rply.rj_stat = AUTH_ERROR;
  rply.rjcted_rply.rj_why = why;
  SVC_REPLY (xprt, &rply);
}

/*
 * Auth too weak error reply
 */
void
svcerr_weakauth (xprt)
     SVCXPRT *xprt;
{

  assert (xprt != NULL);

  svcerr_auth (xprt, AUTH_TOOWEAK);
}

/*
 * Program unavailable error reply
 */
void
svcerr_noprog (xprt)
     SVCXPRT *xprt;
{
  struct rpc_msg rply;

  assert (xprt != NULL);

  rply.rm_direction = REPLY;
  rply.rm_reply.rp_stat = MSG_ACCEPTED;
  rply.acpted_rply.ar_verf = xprt->xp_verf;
  rply.acpted_rply.ar_stat = PROG_UNAVAIL;
  SVC_REPLY (xprt, &rply);
}

/*
 * Program version mismatch error reply
 */
void
svcerr_progvers (xprt, low_vers, high_vers)
     SVCXPRT *xprt;
     rpcvers_t low_vers;
     rpcvers_t high_vers;
{
  struct rpc_msg rply;

  assert (xprt != NULL);

  rply.rm_direction = REPLY;
  rply.rm_reply.rp_stat = MSG_ACCEPTED;
  rply.acpted_rply.ar_verf = xprt->xp_verf;
  rply.acpted_rply.ar_stat = PROG_MISMATCH;
  rply.acpted_rply.ar_vers.low = (u_int32_t) low_vers;
  rply.acpted_rply.ar_vers.high = (u_int32_t) high_vers;
  SVC_REPLY (xprt, &rply);
}

/* ******************* SERVER INPUT STUFF ******************* */

/*
 * Get server side input from some transport.
 *
 * Statement of authentication parameters management:
 * This function owns and manages all authentication parameters, specifically
 * the "raw" parameters (msg.rm_call.cb_cred and msg.rm_call.cb_verf) and
 * the "cooked" credentials (rqst->rq_clntcred).
 * However, this function does not know the structure of the cooked
 * credentials, so it make the following assumptions:
 *   a) the structure is contiguous (no pointers), and
 *   b) the cred structure size does not exceed RQCRED_SIZE bytes.
 * In all events, all three parameters are freed upon exit from this routine.
 * The storage is trivially management on the call stack in user land, but
 * is mallocated in kernel land.
 */

void
svc_getreq (rdfds)
     int rdfds;
{
  fd_set readfds;

  FD_ZERO (&readfds);
  readfds.fds_bits[0] = rdfds;
  svc_getreqset (&readfds);
}

void
svc_getreqset (readfds)
     fd_set *readfds;
{
  int bit, fd;
  fd_mask mask, *maskp;
  int sock;

  assert (readfds != NULL);

  maskp = readfds->fds_bits;
  for (sock = 0; sock < FD_SETSIZE; sock += NFDBITS)
    {
      for (mask = *maskp++; (bit = ffs (mask)) != 0; mask ^= (1 << (bit - 1)))
	{
	  /* sock has input waiting */
	  fd = sock + bit - 1;
	  svc_getreq_common (fd);
	}
    }
}

void
svc_getreq_common (fd)
     int fd;
{
  SVCXPRT *xprt;
  struct svc_req r;
  struct rpc_msg msg;
  int prog_found;
  rpcvers_t low_vers;
  rpcvers_t high_vers;
  enum xprt_stat stat;
  char cred_area[2 * MAX_AUTH_BYTES + RQCRED_SIZE];

  msg.rm_call.cb_cred.oa_base = cred_area;
  msg.rm_call.cb_verf.oa_base = &(cred_area[MAX_AUTH_BYTES]);
  r.rq_clntcred = &(cred_area[2 * MAX_AUTH_BYTES]);

  rwlock_rdlock (&svc_fd_lock);
  xprt = __svc_xports[fd];
  rwlock_unlock (&svc_fd_lock);
  if (xprt == NULL)
    /* But do we control sock? */
    return;
  /* now receive msgs from xprtprt (support batch calls) */
  do
    {
      if (SVC_RECV (xprt, &msg))
	{

	  /* now find the exported program and call it */
	  struct svc_callout *s;
	  enum auth_stat why;

	  r.rq_xprt = xprt;
	  r.rq_prog = msg.rm_call.cb_prog;
	  r.rq_vers = msg.rm_call.cb_vers;
	  r.rq_proc = msg.rm_call.cb_proc;
	  r.rq_cred = msg.rm_call.cb_cred;
	  /* first authenticate the message */
	  if ((why = _authenticate (&r, &msg)) != AUTH_OK)
	    {
	      svcerr_auth (xprt, why);
	      goto call_done;
	    }
	  /* now match message with a registered service */
	  prog_found = FALSE;
	  low_vers = (rpcvers_t) - 1L;
	  high_vers = (rpcvers_t) 0L;
	  for (s = svc_head; s != NULL; s = s->sc_next)
	    {
	      if (s->sc_prog == r.rq_prog)
		{
		  if (s->sc_vers == r.rq_vers)
		    {
		      (*s->sc_dispatch) (&r, xprt);
		      goto call_done;
		    }		/* found correct version */
		  prog_found = TRUE;
		  if (s->sc_vers < low_vers)
		    low_vers = s->sc_vers;
		  if (s->sc_vers > high_vers)
		    high_vers = s->sc_vers;
		}		/* found correct program */
	    }
	  /*
	   * if we got here, the program or version
	   * is not served ...
	   */
	  if (prog_found)
	    svcerr_progvers (xprt, low_vers, high_vers);
	  else
	    svcerr_noprog (xprt);
	  /* Fall through to ... */
	}
      /*
       * Check if the xprt has been disconnected in a
       * recursive call in the service dispatch routine.
       * If so, then break.
       */
      rwlock_rdlock (&svc_fd_lock);
      
      if (xprt != __svc_xports[fd])
	{
	  rwlock_unlock (&svc_fd_lock);
	  break;
	}
      rwlock_unlock (&svc_fd_lock);
    call_done:
      if ((stat = SVC_STAT (xprt)) == XPRT_DIED)
	{
	  SVC_DESTROY (xprt);
	  break;
	}
    else if ((xprt->xp_auth != NULL) 
#ifdef HAVE_LIBGSSAPI
	  	&& (xprt->xp_auth->svc_ah_ops != &svc_auth_gss_ops)
#endif
	) {
	  xprt->xp_auth = NULL;
	}
    }
  while (stat == XPRT_MOREREQS);
}


void
svc_getreq_common_cap (fd)
     int fd;
{
#if 1
  	SVCXPRT *xprt;

  	rwlock_rdlock (&svc_fd_lock);
  	xprt = __svc_xports[fd];
 	rwlock_unlock (&svc_fd_lock);

  	if(xprt->busy)
  		return;
  
  	rwlock_wrlock (&pending_lock);
  	if(!xprt->busy){
  		list_add(&xprt->pending_flag, &pending_xprt);
  		xprt->busy = 1;
  	}
  	rwlock_unlock (&pending_lock);

  	/* MODIFIED by dwyane @ 2010-12-21 */
	//PRINTF_INFO("here comes it, xprt is %p\n", xprt);
	/* END */
#else
  SVCXPRT *xprt;
  struct svc_req r;
  struct rpc_msg msg;
  int prog_found;
  rpcvers_t low_vers;
  rpcvers_t high_vers;
  enum xprt_stat stat;
  char cred_area[2 * MAX_AUTH_BYTES + RQCRED_SIZE];

  msg.rm_call.cb_cred.oa_base = cred_area;
  msg.rm_call.cb_verf.oa_base = &(cred_area[MAX_AUTH_BYTES]);
  r.rq_clntcred = &(cred_area[2 * MAX_AUTH_BYTES]);

  rwlock_rdlock (&svc_fd_lock);
  xprt = __svc_xports[fd];
  rwlock_unlock (&svc_fd_lock);
  if (xprt == NULL)
    /* But do we control sock? */
    return;
  /* now receive msgs from xprtprt (support batch calls) */
  do
    {
      if (SVC_RECV (xprt, &msg))
	{

	  /* now find the exported program and call it */
	  struct svc_callout *s;
	  enum auth_stat why;

	  r.rq_xprt = xprt;
	  r.rq_prog = msg.rm_call.cb_prog;
	  r.rq_vers = msg.rm_call.cb_vers;
	  r.rq_proc = msg.rm_call.cb_proc;
	  r.rq_cred = msg.rm_call.cb_cred;
	  /* first authenticate the message */
	  if ((why = _authenticate (&r, &msg)) != AUTH_OK)
	    {
	      svcerr_auth (xprt, why);
	      goto call_done;
	    }
	  /* now match message with a registered service */
	  prog_found = FALSE;
	  low_vers = (rpcvers_t) - 1L;
	  high_vers = (rpcvers_t) 0L;
	  for (s = svc_head; s != NULL; s = s->sc_next)
	    {
	      if (s->sc_prog == r.rq_prog)
		{
		  if (s->sc_vers == r.rq_vers)
		    {
		      (*s->sc_dispatch) (&r, xprt);
		      goto call_done;
		    }		/* found correct version */
		  prog_found = TRUE;
		  if (s->sc_vers < low_vers)
		    low_vers = s->sc_vers;
		  if (s->sc_vers > high_vers)
		    high_vers = s->sc_vers;
		}		/* found correct program */
	    }
	  /*
	   * if we got here, the program or version
	   * is not served ...
	   */
	  if (prog_found)
	    svcerr_progvers (xprt, low_vers, high_vers);
	  else
	    svcerr_noprog (xprt);
	  /* Fall through to ... */
	}
      /*
       * Check if the xprt has been disconnected in a
       * recursive call in the service dispatch routine.
       * If so, then break.
       */
      rwlock_rdlock (&svc_fd_lock);
      
      if (xprt != __svc_xports[fd])
	{
	  rwlock_unlock (&svc_fd_lock);
	  break;
	}
      rwlock_unlock (&svc_fd_lock);
    call_done:
      if ((stat = SVC_STAT (xprt)) == XPRT_DIED)
	{
	  SVC_DESTROY (xprt);
	  break;
	}
    else if ((xprt->xp_auth != NULL) 
#ifdef HAVE_LIBGSSAPI
	  	&& (xprt->xp_auth->svc_ah_ops != &svc_auth_gss_ops)
#endif
	) {
	  xprt->xp_auth = NULL;
	}
    }
  while (stat == XPRT_MOREREQS);
#endif
}


void
svc_getreq_poll (pfdp, pollretval)
     struct pollfd *pfdp;
     int pollretval;
{
  int i;
  int fds_found;

  for (i = fds_found = 0; fds_found < pollretval; i++)
    {
      struct pollfd *p = &pfdp[i];

      if (p->revents)
	{
	  /* fd has input waiting */
	  fds_found++;
	  /*
	   *      We assume that this function is only called
	   *      via someone _select()ing from svc_fdset or
	   *      _poll()ing from svc_pollset[].  Thus it's safe
	   *      to handle the POLLNVAL event by simply turning
	   *      the corresponding bit off in svc_fdset.  The
	   *      svc_pollset[] array is derived from svc_fdset
	   *      and so will also be updated eventually.
	   *
	   *      XXX Should we do an xprt_unregister() instead?
	   */
	  if (p->revents & POLLNVAL)
	    {
	      rwlock_wrlock (&svc_fd_lock);
	      FD_CLR (p->fd, &svc_fdset);
	      rwlock_unlock (&svc_fd_lock);
	    }
	  else
	    svc_getreq_common (p->fd);
	}
    }
}

void
svc_getreq_poll_cap (pfdp, pollretval)
     struct pollfd *pfdp;
     int pollretval;
{
  int i;
  int fds_found;

  for (i = fds_found = 0; fds_found < pollretval; i++)
    {
      struct pollfd *p = &pfdp[i];

      if (p->revents)
	{
	  /* fd has input waiting */
	  fds_found++;
	  /*
	   *      We assume that this function is only called
	   *      via someone _select()ing from svc_fdset or
	   *      _poll()ing from svc_pollset[].  Thus it's safe
	   *      to handle the POLLNVAL event by simply turning
	   *      the corresponding bit off in svc_fdset.  The
	   *      svc_pollset[] array is derived from svc_fdset
	   *      and so will also be updated eventually.
	   *
	   *      XXX Should we do an xprt_unregister() instead?
	   */
	  if (p->revents & POLLNVAL)
	    {
	      rwlock_wrlock (&svc_fd_lock);
	      FD_CLR (p->fd, &svc_fdset);
	      rwlock_unlock (&svc_fd_lock);
	    }
	  else
	    svc_getreq_common_cap (p->fd);
	}
    }
}


bool_t
rpc_control (int what, void *arg)
{
  int val;

  switch (what)
    {
    case RPC_SVC_CONNMAXREC_SET:
      val = *(int *) arg;
      if (val <= 0)
	return FALSE;
      __svc_maxrec = val;
      return TRUE;
    case RPC_SVC_CONNMAXREC_GET:
      *(int *) arg = __svc_maxrec;
      return TRUE;
    default:
      break;
    }
  return FALSE;
}


/* MODIFIED by dwyane @ 2010-12-21 */
void
do_sth_with_xprt(SVCXPRT *xprt)
{
	struct svc_req r;
	struct rpc_msg msg;
  	int prog_found;
  	rpcvers_t low_vers;
  	rpcvers_t high_vers;
  	enum xprt_stat stat;
  	char cred_area[2 * MAX_AUTH_BYTES + RQCRED_SIZE];
	//struct timeval tim1,tim2,tim3,tim4;
	//unsigned long used_time;
	//gettimeofday(&tim3,NULL);

  	msg.rm_call.cb_cred.oa_base = cred_area;
  	msg.rm_call.cb_verf.oa_base = &(cred_area[MAX_AUTH_BYTES]);
  	r.rq_clntcred = &(cred_area[2 * MAX_AUTH_BYTES]);

  	if (xprt == NULL)
    /* But do we control sock? */
    	return;
  	/* now receive msgs from xprtprt (support batch calls) */
  	pthread_mutex_lock(&xprt->busy_lock);
  	while (xprt->busy_flag)
		pthread_cond_wait(&xprt->busy_cond, &xprt->busy_lock);
	xprt->busy_flag = 1;
	pthread_mutex_unlock(&xprt->busy_lock);
  	do
    {	//gettimeofday(&tim1,NULL);
      if (SVC_RECV (xprt, &msg))
	{	//gettimeofday(&tim2,NULL);
	  	//used_time = (tim2.tv_sec-tim1.tv_sec)*1000000.0 + (tim2.tv_usec-tim1.tv_usec);
		//printf("SVC_RECV used_time is %ul,In %s: %d\n",used_time,__func__,__LINE__);
		PRINTF_INFO("svc_recv ok\n");

	  /* now find the exported program and call it */
	  struct svc_callout *s;
	  enum auth_stat why;

	  r.rq_xprt = xprt;
	  r.rq_prog = msg.rm_call.cb_prog;
	  r.rq_vers = msg.rm_call.cb_vers;
	  r.rq_proc = msg.rm_call.cb_proc;
	  r.rq_cred = msg.rm_call.cb_cred;
	 
	  /* first authenticate the message */
	/* MODIFIED by dwyane @ 2010-12-26 */
#if 0
		  /* MODIFIED by dwyane @ 2010-12-26 *//*
	   r.rq_cred.oa_flavor = AUTH_NULL;
	  r.rq_prog = 0x41320020;
	  r.rq_vers = 1;
	  r.rq_proc = 1;
		*//* END */
#else
	  if ((why = _authenticate (&r, &msg)) != AUTH_OK)
	    {
	      svcerr_auth (xprt, why);
	      goto call_done;
	    }
#endif
	  /* END */
	  /* MODIFIED by dwyane @ 2010-12-26 */
	  PRINTF_INFO("get through _authenticate\n");
	/* END */
	  /* now match message with a registered service */
	  prog_found = FALSE;
	  low_vers = (rpcvers_t) - 1L;
	  high_vers = (rpcvers_t) 0L;
	  for (s = svc_head; s != NULL; s = s->sc_next)
	    {
	      if (s->sc_prog == r.rq_prog)
		{
		  if (s->sc_vers == r.rq_vers)
		    {//gettimeofday(&tim1,NULL);
		      (*s->sc_dispatch) (&r, xprt);
			  //gettimeofday(&tim2,NULL);
			  //used_time = (tim2.tv_sec-tim1.tv_sec)*1000000.0 + (tim2.tv_usec-tim1.tv_usec);
			//printf("OSD_PROG used_time is %ul,In %s: %d\n",used_time,__func__,__LINE__);
		      goto call_done;
		    }		/* found correct version */
		  prog_found = TRUE;
		  if (s->sc_vers < low_vers)
		    low_vers = s->sc_vers;
		  if (s->sc_vers > high_vers)
		    high_vers = s->sc_vers;
		}		/* found correct program */
	    }
	  /*
	   * if we got here, the program or version
	   * is not served ...
	   */
	  if (prog_found)
	    svcerr_progvers (xprt, low_vers, high_vers);
	  else
	    svcerr_noprog (xprt);
	  /* Fall through to ... */
	}
      /*
       * Check if the xprt has been disconnected in a
       * recursive call in the service dispatch routine.
       * If so, then break.
       */
       /*
      rwlock_rdlock (&svc_fd_lock);
      if (xprt != __svc_xports[fd])
	{
	  rwlock_unlock (&svc_fd_lock);
	  break;
	}
      rwlock_unlock (&svc_fd_lock);
      */
    call_done:
    pthread_mutex_lock(&xprt->busy_lock);
  	xprt->busy_flag= 0;
  	pthread_mutex_unlock(&xprt->busy_lock);
  	pthread_cond_signal(&xprt->busy_cond);
      if ((stat = SVC_STAT (xprt)) == XPRT_DIED)
	{
	  SVC_DESTROY (xprt);
	  break;
	}
    else if ((xprt->xp_auth != NULL) 
#ifdef HAVE_LIBGSSAPI
	  	&& (xprt->xp_auth->svc_ah_ops != &svc_auth_gss_ops)
#endif
	) {
	  xprt->xp_auth = NULL;
	}
    }
  while (stat == XPRT_MOREREQS);
  //gettimeofday(&tim4,NULL);
  //used_time = (tim4.tv_sec-tim3.tv_sec)*1000000.0 + (tim4.tv_usec-tim3.tv_usec);
	//printf("DO_STH_WITH_XPRT used_time is %ul,In %s: %d\n",used_time,__func__,__LINE__);
  //PRINTF_INFO("done with this xprt %p\n", xprt);
}

/* MODIFIED by dwyane @ 2010-12-23 */
void dwyane_enqueue(SVCXPRT *xprt){
	rwlock_wrlock (&pending_lock);
  	//if(!xprt->busy){
  	list_add(&xprt->pending_flag, &pending_xprt);
  		//xprt->busy = 1;
  	//}
  	rwlock_unlock (&pending_lock);
}
/* END */

