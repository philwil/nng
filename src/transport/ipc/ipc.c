//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/nng_impl.h"

// IPC transport.   Platform specific IPC operations must be
// supplied as well.  Normally the IPC is UNIX domain sockets or
// Windows named pipes.  Other platforms could use other mechanisms.

typedef struct nni_ipc_pipe nni_ipc_pipe;
typedef struct nni_ipc_ep   nni_ipc_ep;

// nni_ipc_pipe is one end of an IPC connection.
struct nni_ipc_pipe {
	const char *       addr;
	nni_plat_ipc_pipe *ipp;
	uint16_t           peer;
	uint16_t           proto;
	size_t             rcvmax;

	uint8_t txhead[1 + sizeof(uint64_t)];
	uint8_t rxhead[1 + sizeof(uint64_t)];
	int     gottxhead;
	int     gotrxhead;
	int     wanttxhead;
	int     wantrxhead;

	nni_aio *user_txaio;
	nni_aio *user_rxaio;
	nni_aio *user_negaio;
	nni_aio  txaio;
	nni_aio  rxaio;
	nni_aio  negaio;
	nni_msg *rxmsg;
	nni_mtx  mtx;
};

struct nni_ipc_ep {
	char             addr[NNG_MAXADDRLEN + 1];
	nni_plat_ipc_ep *iep;
	int              closed;
	uint16_t         proto;
	size_t           rcvmax;
	nni_aio          aio;
	nni_aio *        user_aio;
	nni_mtx          mtx;
};

static void nni_ipc_pipe_send_cb(void *);
static void nni_ipc_pipe_recv_cb(void *);
static void nni_ipc_pipe_nego_cb(void *);
static void nni_ipc_ep_cb(void *);

static int
nni_ipc_tran_init(void)
{
	return (0);
}

static void
nni_ipc_tran_fini(void)
{
}

static void
nni_ipc_pipe_close(void *arg)
{
	nni_ipc_pipe *pipe = arg;

	nni_plat_ipc_pipe_close(pipe->ipp);
}

static void
nni_ipc_pipe_fini(void *arg)
{
	nni_ipc_pipe *pipe = arg;

	nni_aio_fini(&pipe->rxaio);
	nni_aio_fini(&pipe->txaio);
	nni_aio_fini(&pipe->negaio);
	if (pipe->ipp != NULL) {
		nni_plat_ipc_pipe_fini(pipe->ipp);
	}
	if (pipe->rxmsg) {
		nni_msg_free(pipe->rxmsg);
	}
	nni_mtx_fini(&pipe->mtx);
	NNI_FREE_STRUCT(pipe);
}

static int
nni_ipc_pipe_init(nni_ipc_pipe **pipep, nni_ipc_ep *ep, void *ipp)
{
	nni_ipc_pipe *pipe;
	int           rv;

	if ((pipe = NNI_ALLOC_STRUCT(pipe)) == NULL) {
		return (NNG_ENOMEM);
	}
	if ((rv = nni_mtx_init(&pipe->mtx)) != 0) {
		goto fail;
	}
	rv = nni_aio_init(&pipe->txaio, nni_ipc_pipe_send_cb, pipe);
	if (rv != 0) {
		goto fail;
	}
	rv = nni_aio_init(&pipe->rxaio, nni_ipc_pipe_recv_cb, pipe);
	if (rv != 0) {
		goto fail;
	}
	rv = nni_aio_init(&pipe->negaio, nni_ipc_pipe_nego_cb, pipe);
	if (rv != 0) {
		goto fail;
	}

	pipe->proto  = ep->proto;
	pipe->rcvmax = ep->rcvmax;
	pipe->ipp    = ipp;
	pipe->addr   = ep->addr;

	*pipep = pipe;
	return (0);

fail:
	nni_ipc_pipe_fini(pipe);
	return (rv);
}

static void
nni_ipc_cancel_start(nni_aio *aio)
{
	nni_ipc_pipe *pipe = aio->a_prov_data;

	nni_mtx_lock(&pipe->mtx);
	if ((aio = pipe->user_negaio) != NULL) {
		pipe->user_negaio = NULL;
		nni_aio_stop(aio);
	}
	nni_mtx_unlock(&pipe->mtx);
}

static void
nni_ipc_pipe_nego_cb(void *arg)
{
	nni_ipc_pipe *pipe = arg;
	nni_aio *     aio  = &pipe->negaio;
	int           rv;

	nni_mtx_lock(&pipe->mtx);
	if ((rv = nni_aio_result(aio)) != 0) {
		goto done;
	}

	// We start transmitting before we receive.
	if (pipe->gottxhead < pipe->wanttxhead) {
		pipe->gottxhead += nni_aio_count(aio);
	} else if (pipe->gotrxhead < pipe->wantrxhead) {
		pipe->gotrxhead += nni_aio_count(aio);
	}

	if (pipe->gottxhead < pipe->wanttxhead) {
		aio->a_niov           = 1;
		aio->a_iov[0].iov_len = pipe->wanttxhead - pipe->gottxhead;
		aio->a_iov[0].iov_buf = &pipe->txhead[pipe->gottxhead];
		// send it down...
		nni_plat_ipc_pipe_send(pipe->ipp, aio);
		nni_mtx_unlock(&pipe->mtx);
		return;
	}
	if (pipe->gotrxhead < pipe->wantrxhead) {
		aio->a_niov           = 1;
		aio->a_iov[0].iov_len = pipe->wantrxhead - pipe->gotrxhead;
		aio->a_iov[0].iov_buf = &pipe->rxhead[pipe->gotrxhead];
		nni_plat_ipc_pipe_recv(pipe->ipp, aio);
		nni_mtx_unlock(&pipe->mtx);
		return;
	}
	// We have both sent and received the headers.  Lets check the
	// receive side header.
	if ((pipe->rxhead[0] != 0) || (pipe->rxhead[1] != 'S') ||
	    (pipe->rxhead[2] != 'P') || (pipe->rxhead[3] != 0) ||
	    (pipe->rxhead[6] != 0) || (pipe->rxhead[7] != 0)) {
		rv = NNG_EPROTO;
		goto done;
	}

	NNI_GET16(&pipe->rxhead[4], pipe->peer);

done:
	if ((aio = pipe->user_negaio) != NULL) {
		pipe->user_negaio = NULL;
		nni_aio_finish(aio, rv, 0);
	}
	nni_mtx_unlock(&pipe->mtx);
}

static void
nni_ipc_pipe_send_cb(void *arg)
{
	nni_ipc_pipe *pipe = arg;
	nni_aio *     aio;
	int           rv;
	size_t        len;

	nni_mtx_lock(&pipe->mtx);
	if ((aio = pipe->user_txaio) == NULL) {
		nni_mtx_unlock(&pipe->mtx);
		return;
	}
	pipe->user_txaio = NULL;
	if ((rv = nni_aio_result(&pipe->txaio)) != 0) {
		len = 0;
	} else {
		len = nni_msg_len(aio->a_msg);
		nni_msg_free(aio->a_msg);
		aio->a_msg = NULL;
	}
	nni_aio_finish(aio, rv, len);
	nni_mtx_unlock(&pipe->mtx);
}

static void
nni_ipc_pipe_recv_cb(void *arg)
{
	nni_ipc_pipe *pipe = arg;
	nni_aio *     aio;
	int           rv;

	nni_mtx_lock(&pipe->mtx);
	aio = pipe->user_rxaio;
	if (aio == NULL) {
		// aio was canceled
		nni_mtx_unlock(&pipe->mtx);
		return;
	}

	if ((rv = nni_aio_result(&pipe->rxaio)) != 0) {
		// Error on receive.  This has to cause an error back
		// to the user.  Also, if we had allocated an rxmsg, lets
		// toss it.
		if (pipe->rxmsg != NULL) {
			nni_msg_free(pipe->rxmsg);
			pipe->rxmsg = NULL;
		}
		pipe->user_rxaio = NULL;
		nni_aio_finish(aio, rv, 0);
		nni_mtx_unlock(&pipe->mtx);
		return;
	}

	// If we don't have a message yet, we were reading the TCP message
	// header, which is just the length.  This tells us the size of the
	// message to allocate and how much more to expect.
	if (pipe->rxmsg == NULL) {
		uint64_t len;

		// Check to make sure we got msg type 1.
		if (pipe->rxhead[0] != 1) {
			nni_aio_finish(aio, NNG_EPROTO, 0);
			nni_mtx_unlock(&pipe->mtx);
			return;
		}

		// We should have gotten a message header.
		NNI_GET64(pipe->rxhead + 1, len);

		// Make sure the message payload is not too big.  If it is
		// the caller will shut down the pipe.
		if (len > pipe->rcvmax) {
			pipe->user_rxaio = NULL;
			nni_aio_finish(aio, NNG_EMSGSIZE, 0);
			nni_mtx_unlock(&pipe->mtx);
			return;
		}

		// Note that all IO on this pipe is blocked behind this
		// allocation.  We could possibly look at using a separate
		// lock for the read side in the future, so that we allow
		// transmits to proceed normally.  In practice this is
		// unlikely to be much of an issue though.
		if ((rv = nng_msg_alloc(&pipe->rxmsg, (size_t) len)) != 0) {
			pipe->user_rxaio = NULL;
			nni_aio_finish(aio, rv, 0);
			nni_mtx_unlock(&pipe->mtx);
			return;
		}

		// Submit the rest of the data for a read -- we want to
		// read the entire message now.
		pipe->rxaio.a_iov[0].iov_buf = nni_msg_body(pipe->rxmsg);
		pipe->rxaio.a_iov[0].iov_len = nni_msg_len(pipe->rxmsg);
		pipe->rxaio.a_niov           = 1;

		nni_plat_ipc_pipe_recv(pipe->ipp, &pipe->rxaio);
		nni_mtx_unlock(&pipe->mtx);
		return;
	}

	// Otherwise we got a message read completely.  Let the user know the
	// good news.
	pipe->user_rxaio = NULL;
	aio->a_msg       = pipe->rxmsg;
	pipe->rxmsg      = NULL;
	nni_aio_finish(aio, 0, nni_msg_len(aio->a_msg));
	nni_mtx_unlock(&pipe->mtx);
}

static void
nni_ipc_cancel_tx(nni_aio *aio)
{
	nni_ipc_pipe *pipe = aio->a_prov_data;

	nni_mtx_lock(&pipe->mtx);
	pipe->user_txaio = NULL;
	nni_mtx_unlock(&pipe->mtx);

	// stop the underlying aio ... we don't want a result for it.
	nni_aio_stop(&pipe->txaio);
}

static void
nni_ipc_pipe_send(void *arg, nni_aio *aio)
{
	nni_ipc_pipe *pipe = arg;
	nni_msg *     msg  = aio->a_msg;
	uint64_t      len;

	len = nni_msg_len(msg) + nni_msg_header_len(msg);

	nni_mtx_lock(&pipe->mtx);
	if (nni_aio_start(aio, nni_ipc_cancel_tx, pipe) != 0) {
		nni_mtx_unlock(&pipe->mtx);
		return;
	}

	pipe->user_txaio = aio;

	pipe->txhead[0] = 1; // message type, 1.
	NNI_PUT64(pipe->txhead + 1, len);

	pipe->txaio.a_iov[0].iov_buf = pipe->txhead;
	pipe->txaio.a_iov[0].iov_len = sizeof(pipe->txhead);
	pipe->txaio.a_iov[1].iov_buf = nni_msg_header(msg);
	pipe->txaio.a_iov[1].iov_len = nni_msg_header_len(msg);
	pipe->txaio.a_iov[2].iov_buf = nni_msg_body(msg);
	pipe->txaio.a_iov[2].iov_len = nni_msg_len(msg);
	pipe->txaio.a_niov           = 3;

	nni_plat_ipc_pipe_send(pipe->ipp, &pipe->txaio);
	nni_mtx_unlock(&pipe->mtx);
}

static void
nni_ipc_cancel_rx(nni_aio *aio)
{
	nni_ipc_pipe *pipe = aio->a_prov_data;

	nni_mtx_lock(&pipe->mtx);
	pipe->user_rxaio = NULL;
	nni_mtx_unlock(&pipe->mtx);

	// stop the underlying aio ... we don't want a result for it.
	nni_aio_stop(&pipe->rxaio);
}

static void
nni_ipc_pipe_recv(void *arg, nni_aio *aio)
{
	nni_ipc_pipe *pipe = arg;

	nni_mtx_lock(&pipe->mtx);

	if (nni_aio_start(aio, nni_ipc_cancel_rx, pipe) != 0) {
		nni_mtx_unlock(&pipe->mtx);
		return;
	}

	pipe->user_rxaio = aio;
	NNI_ASSERT(pipe->rxmsg == NULL);

	// Schedule a read of the IPC header.
	pipe->rxaio.a_iov[0].iov_buf = pipe->rxhead;
	pipe->rxaio.a_iov[0].iov_len = sizeof(pipe->rxhead);
	pipe->rxaio.a_niov           = 1;

	nni_plat_ipc_pipe_recv(pipe->ipp, &pipe->rxaio);
	nni_mtx_unlock(&pipe->mtx);
}

static void
nni_ipc_pipe_start(void *arg, nni_aio *aio)
{
	nni_ipc_pipe *pipe = arg;
	int           rv;

	nni_mtx_lock(&pipe->mtx);
	pipe->txhead[0] = 0;
	pipe->txhead[1] = 'S';
	pipe->txhead[2] = 'P';
	pipe->txhead[3] = 0;
	NNI_PUT16(&pipe->txhead[4], pipe->proto);
	NNI_PUT16(&pipe->txhead[6], 0);

	pipe->user_negaio             = aio;
	pipe->gotrxhead               = 0;
	pipe->gottxhead               = 0;
	pipe->wantrxhead              = 8;
	pipe->wanttxhead              = 8;
	pipe->negaio.a_niov           = 1;
	pipe->negaio.a_iov[0].iov_len = 8;
	pipe->negaio.a_iov[0].iov_buf = &pipe->txhead[0];
	rv = nni_aio_start(aio, nni_ipc_cancel_start, pipe);
	if (rv != 0) {
		nni_mtx_unlock(&pipe->mtx);
		return;
	}
	nni_plat_ipc_pipe_send(pipe->ipp, &pipe->negaio);
	nni_mtx_unlock(&pipe->mtx);
}

static uint16_t
nni_ipc_pipe_peer(void *arg)
{
	nni_ipc_pipe *pipe = arg;

	return (pipe->peer);
}

static int
nni_ipc_pipe_getopt(void *arg, int option, void *buf, size_t *szp)
{
#if 0
	nni_inproc_pipe *pipe = arg;
	size_t len;

	switch (option) {
	case NNG_OPT_LOCALADDR:
	case NNG_OPT_REMOTEADDR:
		len = strlen(pipe->addr) + 1;
		if (len > *szp) {
			(void) memcpy(buf, pipe->addr, *szp);
		} else {
			(void) memcpy(buf, pipe->addr, len);
		}
		*szp = len;
		return (0);
	}
#endif
	return (NNG_ENOTSUP);
}

static void
nni_ipc_ep_fini(void *arg)
{
	nni_ipc_ep *ep = arg;

	nni_plat_ipc_ep_fini(ep->iep);
	nni_aio_fini(&ep->aio);
	nni_mtx_fini(&ep->mtx);
	NNI_FREE_STRUCT(ep);
}

static int
nni_ipc_ep_init(void **epp, const char *url, nni_sock *sock, int mode)
{
	nni_ipc_ep *ep;
	int         rv;

	if ((strlen(url) > NNG_MAXADDRLEN - 1) ||
	    (strncmp(url, "ipc://", strlen("ipc://")) != 0)) {
		return (NNG_EADDRINVAL);
	}

	if ((ep = NNI_ALLOC_STRUCT(ep)) == NULL) {
		return (NNG_ENOMEM);
	}
	if (((rv = nni_mtx_init(&ep->mtx)) != 0) ||
	    ((rv = nni_aio_init(&ep->aio, nni_ipc_ep_cb, ep)) != 0) ||
	    ((rv = nni_plat_ipc_ep_init(&ep->iep, url, mode)) != 0)) {
		nni_ipc_ep_fini(ep);
		return (rv);
	}
	ep->closed = 0;
	ep->proto  = nni_sock_proto(sock);
	ep->rcvmax = nni_sock_rcvmaxsz(sock);
	(void) snprintf(ep->addr, sizeof(ep->addr), "%s", url);

	*epp = ep;
	return (0);
}

static void
nni_ipc_ep_close(void *arg)
{
	nni_ipc_ep *ep = arg;

	nni_plat_ipc_ep_close(ep->iep);
}

static int
nni_ipc_ep_bind(void *arg)
{
	nni_ipc_ep *ep = arg;

	return (nni_plat_ipc_ep_listen(ep->iep));
}

static void
nni_ipc_ep_finish(nni_ipc_ep *ep)
{
	nni_aio *     aio = ep->user_aio;
	nni_ipc_pipe *pipe;
	int           rv;

	if ((aio = ep->user_aio) == NULL) {
		return;
	}
	ep->user_aio = NULL;
	if ((rv = nni_aio_result(&ep->aio)) != 0) {
		goto done;
	}
	NNI_ASSERT(ep->aio.a_pipe != NULL);

	// Attempt to allocate the parent pipe.  If this fails we'll
	// drop the connection (ENOMEM probably).
	if ((rv = nni_ipc_pipe_init(&pipe, ep, ep->aio.a_pipe)) != 0) {
		nni_plat_ipc_pipe_fini(ep->aio.a_pipe);
		goto done;
	}

	aio->a_pipe = pipe;

done:
	ep->aio.a_pipe = NULL;
	nni_aio_finish(aio, rv, 0);
}

static void
nni_ipc_ep_cb(void *arg)
{
	nni_ipc_ep *ep = arg;

	nni_mtx_lock(&ep->mtx);
	nni_ipc_ep_finish(ep);
	nni_mtx_unlock(&ep->mtx);
}

static void
nni_ipc_cancel_ep(nni_aio *aio)
{
	nni_ipc_ep *ep = aio->a_prov_data;

	nni_mtx_lock(&ep->mtx);
	if (ep->user_aio == aio) {
		ep->user_aio = NULL;
	}
	nni_aio_stop(&ep->aio);
	nni_mtx_unlock(&ep->mtx);
}

static void
nni_ipc_ep_accept(void *arg, nni_aio *aio)
{
	nni_ipc_ep *ep = arg;
	int         rv;

	nni_mtx_lock(&ep->mtx);
	NNI_ASSERT(ep->user_aio == NULL);
	ep->user_aio = aio;

	// If we can't start, then its dying and we can't report either,
	if ((rv = nni_aio_start(aio, nni_ipc_cancel_ep, ep)) != 0) {
		ep->user_aio = NULL;
		nni_mtx_unlock(&ep->mtx);
		return;
	}

	nni_plat_ipc_ep_accept(ep->iep, &ep->aio);
	nni_mtx_unlock(&ep->mtx);
}

static void
nni_ipc_ep_connect(void *arg, nni_aio *aio)
{
	nni_ipc_ep *ep = arg;
	int         rv;

	nni_mtx_lock(&ep->mtx);
	NNI_ASSERT(ep->user_aio == NULL);
	ep->user_aio = aio;

	// If we can't start, then its dying and we can't report either,
	if ((rv = nni_aio_start(aio, nni_ipc_cancel_ep, ep)) != 0) {
		ep->user_aio = NULL;
		nni_mtx_unlock(&ep->mtx);
		return;
	}

	nni_plat_ipc_ep_connect(ep->iep, &ep->aio);
	nni_mtx_unlock(&ep->mtx);
}

static nni_tran_pipe nni_ipc_pipe_ops = {
	.p_fini   = nni_ipc_pipe_fini,
	.p_start  = nni_ipc_pipe_start,
	.p_send   = nni_ipc_pipe_send,
	.p_recv   = nni_ipc_pipe_recv,
	.p_close  = nni_ipc_pipe_close,
	.p_peer   = nni_ipc_pipe_peer,
	.p_getopt = nni_ipc_pipe_getopt,
};

static nni_tran_ep nni_ipc_ep_ops = {
	.ep_init    = nni_ipc_ep_init,
	.ep_fini    = nni_ipc_ep_fini,
	.ep_connect = nni_ipc_ep_connect,
	.ep_bind    = nni_ipc_ep_bind,
	.ep_accept  = nni_ipc_ep_accept,
	.ep_close   = nni_ipc_ep_close,
	.ep_setopt  = NULL,
	.ep_getopt  = NULL,
};

// This is the IPC transport linkage, and should be the only global
// symbol in this entire file.
struct nni_tran nni_ipc_tran = {
	.tran_scheme = "ipc",
	.tran_ep     = &nni_ipc_ep_ops,
	.tran_pipe   = &nni_ipc_pipe_ops,
	.tran_init   = nni_ipc_tran_init,
	.tran_fini   = nni_ipc_tran_fini,
};
