/*
 * Copyright (C) 2009-2012 the libgit2 contributors
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#include "git2.h"
#include "smart.h"
#include "refs.h"

static int git_smart__recv_cb(gitno_buffer *buf)
{
	transport_smart *t = (transport_smart *) buf->cb_data;
	size_t old_len, bytes_read;
	int error;

	assert(t->current_stream);

	old_len = buf->offset;

	if ((error = t->current_stream->read(t->current_stream, buf->data + buf->offset, buf->len - buf->offset, &bytes_read)) < 0)
		return error;

	buf->offset += bytes_read;

	if (t->packetsize_cb)
		t->packetsize_cb((int)bytes_read, t->packetsize_payload);

	return (int)(buf->offset - old_len);
}

GIT_INLINE(int) git_smart__reset_stream(transport_smart *t, bool close_subtransport)
{
	if (t->current_stream) {
		t->current_stream->free(t->current_stream);
		t->current_stream = NULL;
	}

	if (close_subtransport &&
		t->wrapped->close(t->wrapped) < 0)
		return -1;

	return 0;
}

static int git_smart__set_callbacks(
	git_transport *transport,
	git_transport_message_cb progress_cb,
	git_transport_message_cb error_cb,
	void *message_cb_payload)
{
	transport_smart *t = (transport_smart *)transport;

	t->progress_cb = progress_cb;
	t->error_cb = error_cb;
	t->message_cb_payload = message_cb_payload;

	return 0;
}

static int git_smart__connect(
	git_transport *transport,
	const char *url,
	git_cred_acquire_cb cred_acquire_cb,
	int direction,
	int flags)
{
	transport_smart *t = (transport_smart *)transport;
	git_smart_subtransport_stream *stream;
	int error;
	git_pkt *pkt;
	git_pkt_ref *first;
	git_smart_service_t service;

	if (git_smart__reset_stream(t, true) < 0)
		return -1;

	t->url = git__strdup(url);
	GITERR_CHECK_ALLOC(t->url);

	t->direction = direction;
	t->flags = flags;
	t->cred_acquire_cb = cred_acquire_cb;

	if (GIT_DIRECTION_FETCH == t->direction)
		service = GIT_SERVICE_UPLOADPACK_LS;
	else if (GIT_DIRECTION_PUSH == t->direction)
		service = GIT_SERVICE_RECEIVEPACK_LS;
	else {
		giterr_set(GITERR_NET, "Invalid direction");
		return -1;
	}

	if ((error = t->wrapped->action(&stream, t->wrapped, t->url, service)) < 0)
		return error;

	/* Save off the current stream (i.e. socket) that we are working with */
	t->current_stream = stream;

	gitno_buffer_setup_callback(NULL, &t->buffer, t->buffer_data, sizeof(t->buffer_data), git_smart__recv_cb, t);

	/* 2 flushes for RPC; 1 for stateful */
	if ((error = git_smart__store_refs(t, t->rpc ? 2 : 1)) < 0)
		return error;

	/* Strip the comment packet for RPC */
	if (t->rpc) {
		pkt = (git_pkt *)git_vector_get(&t->refs, 0);

		if (!pkt || GIT_PKT_COMMENT != pkt->type) {
			giterr_set(GITERR_NET, "Invalid response");
			return -1;
		} else {
			/* Remove the comment pkt from the list */
			git_vector_remove(&t->refs, 0);
			git__free(pkt);
		}
	}

	/* We now have loaded the refs. */
	t->have_refs = 1;

	first = (git_pkt_ref *)git_vector_get(&t->refs, 0);

	/* Detect capabilities */
	if (git_smart__detect_caps(first, &t->caps) < 0)
		return -1;

	/* If the only ref in the list is capabilities^{} with OID_ZERO, remove it */
	if (1 == t->refs.length && !strcmp(first->head.name, "capabilities^{}") &&
		git_oid_iszero(&first->head.oid)) {
		git_vector_clear(&t->refs);
		git_pkt_free((git_pkt *)first);
	}

	if (t->rpc && git_smart__reset_stream(t, false) < 0)
		return -1;

	/* We're now logically connected. */
	t->connected = 1;

	return 0;
}

static int git_smart__ls(git_transport *transport, git_headlist_cb list_cb, void *payload)
{
	transport_smart *t = (transport_smart *)transport;
	unsigned int i;
	git_pkt *p = NULL;

	if (!t->have_refs) {
		giterr_set(GITERR_NET, "The transport has not yet loaded the refs");
		return -1;
	}

	git_vector_foreach(&t->refs, i, p) {
		git_pkt_ref *pkt = NULL;

		if (p->type != GIT_PKT_REF)
			continue;

		pkt = (git_pkt_ref *)p;

		if (list_cb(&pkt->head, payload))
			return GIT_EUSER;
	}

	return 0;
}

int git_smart__negotiation_step(git_transport *transport, void *data, size_t len)
{
	transport_smart *t = (transport_smart *)transport;
	git_smart_subtransport_stream *stream;
	int error;

	if (t->rpc && git_smart__reset_stream(t, false) < 0)
		return -1;

	if (GIT_DIRECTION_FETCH != t->direction) {
		giterr_set(GITERR_NET, "This operation is only valid for fetch");
		return -1;
	}

	if ((error = t->wrapped->action(&stream, t->wrapped, t->url, GIT_SERVICE_UPLOADPACK)) < 0)
		return error;

	/* If this is a stateful implementation, the stream we get back should be the same */
	assert(t->rpc || t->current_stream == stream);

	/* Save off the current stream (i.e. socket) that we are working with */
	t->current_stream = stream;

	if ((error = stream->write(stream, (const char *)data, len)) < 0)
		return error;

	gitno_buffer_setup_callback(NULL, &t->buffer, t->buffer_data, sizeof(t->buffer_data), git_smart__recv_cb, t);

	return 0;
}

int git_smart__get_push_stream(transport_smart *t, git_smart_subtransport_stream **stream)
{
	int error;

	if (t->rpc && git_smart__reset_stream(t, false) < 0)
		return -1;

	if (GIT_DIRECTION_PUSH != t->direction) {
		giterr_set(GITERR_NET, "This operation is only valid for push");
		return -1;
	}

	if ((error = t->wrapped->action(stream, t->wrapped, t->url, GIT_SERVICE_RECEIVEPACK)) < 0)
		return error;

	/* If this is a stateful implementation, the stream we get back should be the same */
	assert(t->rpc || t->current_stream == *stream);

	/* Save off the current stream (i.e. socket) that we are working with */
	t->current_stream = *stream;

	gitno_buffer_setup_callback(NULL, &t->buffer, t->buffer_data, sizeof(t->buffer_data), git_smart__recv_cb, t);

	return 0;
}

static void git_smart__cancel(git_transport *transport)
{
	transport_smart *t = (transport_smart *)transport;

	git_atomic_set(&t->cancelled, 1);
}

static int git_smart__is_connected(git_transport *transport)
{
	transport_smart *t = (transport_smart *)transport;

	return t->connected;
}

static int git_smart__read_flags(git_transport *transport, int *flags)
{
	transport_smart *t = (transport_smart *)transport;

	*flags = t->flags;

	return 0;
}

static int git_smart__close(git_transport *transport)
{
	transport_smart *t = (transport_smart *)transport;
	git_vector *refs = &t->refs;
	git_vector *common = &t->common;
	unsigned int i;
	git_pkt *p;
	int ret;

	ret = git_smart__reset_stream(t, true);

	git_vector_foreach(refs, i, p)
		git_pkt_free(p);

	git_vector_free(refs);

	git_vector_foreach(common, i, p)
		git_pkt_free(p);

	git_vector_free(common);

	if (t->url) {
		git__free(t->url);
		t->url = NULL;
	}

	t->connected = 0;

	return ret;
}

static void git_smart__free(git_transport *transport)
{
	transport_smart *t = (transport_smart *)transport;

	/* Make sure that the current stream is closed, if we have one. */
	git_smart__close(transport);

	/* Free the subtransport */
	t->wrapped->free(t->wrapped);

	git__free(t);
}

int git_transport_smart(git_transport **out, git_remote *owner, void *param)
{
	transport_smart *t;
	git_smart_subtransport_definition *definition = (git_smart_subtransport_definition *)param;

	if (!param)
		return -1;

	t = git__calloc(sizeof(transport_smart), 1);
	GITERR_CHECK_ALLOC(t);

	t->parent.set_callbacks = git_smart__set_callbacks;
	t->parent.connect = git_smart__connect;
	t->parent.close = git_smart__close;
	t->parent.free = git_smart__free;
	t->parent.negotiate_fetch = git_smart__negotiate_fetch;
	t->parent.download_pack = git_smart__download_pack;
	t->parent.push = git_smart__push;
	t->parent.ls = git_smart__ls;
	t->parent.is_connected = git_smart__is_connected;
	t->parent.read_flags = git_smart__read_flags;
	t->parent.cancel = git_smart__cancel;
	
	t->owner = owner;
	t->rpc = definition->rpc;

	if (git_vector_init(&t->refs, 16, NULL) < 0) {
		git__free(t);
		return -1;
	}

	if (definition->callback(&t->wrapped, &t->parent) < 0) {
		git__free(t);
		return -1;
	}	

	*out = (git_transport *) t;
	return 0;
}
