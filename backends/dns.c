/* Copyright (c) 2013 Pavel Šimerda, Red Hat, Inc. (psimerda at redhat.com) and others
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <netresolve-backend.h>
#include <stdlib.h>
#include <stdio.h>
#include <ldns/ldns.h>

#if defined(USE_UNBOUND)

#include <unbound.h>
#define priv_dns priv_dns

#elif defined(USE_ARES)

#include <ares.h>
#define priv_dns priv_aresdns

#endif

struct priv_dns {
	netresolve_query_t query;
	int protocol;
	uint16_t port;
	int priority;
	int weight;
	char *name;
	int family;
	const uint8_t *address;
	int cls;
	int type;
	ldns_pkt *ip4_pkt;
	ldns_pkt *ip6_pkt;
#if defined(USE_UNBOUND)
	struct ub_ctx* ctx;
#elif defined(USE_ARES)
	ares_channel channel;
	fd_set rfds;
	fd_set wfds;
	int nfds;
#endif
};

static void lookup(struct priv_dns *priv, int family);

#if defined(USE_ARES)

static void
register_fds(netresolve_query_t query)
{
	struct priv_dns *priv = netresolve_backend_get_priv(query);
	fd_set rfds;
	fd_set wfds;
	int nfds, fd;

	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	nfds = ares_fds(priv->channel, &rfds, &wfds);

	for (fd = 0; fd < nfds || fd < priv->nfds; fd++) {
		if (!FD_ISSET(fd, &rfds) && FD_ISSET(fd, &priv->rfds)) {
			FD_CLR(fd, &priv->rfds);
			netresolve_backend_watch_fd(query, fd, 0);
		} else if (FD_ISSET(fd, &rfds) && !FD_ISSET(fd, &priv->rfds)) {
			FD_SET(fd, &priv->rfds);
			netresolve_backend_watch_fd(query, fd, POLLIN);
		}
		if (!FD_ISSET(fd, &wfds) && FD_ISSET(fd, &priv->wfds)) {
			FD_CLR(fd, &priv->wfds);
			netresolve_backend_watch_fd(query, fd, 0);
		} else if (FD_ISSET(fd, &wfds) && !FD_ISSET(fd, &priv->wfds)) {
			FD_SET(fd, &priv->wfds);
			netresolve_backend_watch_fd(query, fd, POLLOUT);
		}
	}

	priv->nfds = nfds;

	if (!nfds)
		netresolve_backend_finished(query);
}

#endif

static void
#if defined(USE_UNBOUND)
callback(void *arg, int status, struct ub_result* result)
#elif defined(USE_ARES)
callback(void *arg, int status, int timeouts, unsigned char *abuf, int alen)
#endif
{
	struct priv_dns *priv = arg;
	netresolve_query_t query = priv->query;
	const uint8_t *answer;
	size_t length;

#if defined(USE_UNBOUND)
	if (status) {
		error("libunbound: %s", ub_strerror(status));
		return;
	}

	answer = result->answer_packet;
	length = result->answer_len;
#elif defined(USE_ARES)
	switch (status) {
	case ARES_EDESTRUCTION:
		return;
	case ARES_SUCCESS:
		break;
	default:
		error("ares: %s", ares_strerror(status));
		return;
	}

	answer = abuf;
	length = alen;
#endif

	if (priv->type) {
		netresolve_backend_set_dns_answer(query, answer, length);
		netresolve_backend_finished(query);
		return;
	}

	ldns_pkt *pkt;

	if (ldns_wire2pkt(&pkt, answer, length)) {
		error("can't parse the DNS answer");
		netresolve_backend_failed(query);
		return;
	}

#if defined(USE_UNBOUND)
	ub_resolve_free(result);
#endif

	debug("received %s record", ldns_rr_descript(pkt->_question->_rrs[0]->_rr_type)->_name);

	switch (pkt->_question->_rrs[0]->_rr_type) {
	case LDNS_RR_TYPE_A:
		priv->ip4_pkt = pkt;
		return;
	case LDNS_RR_TYPE_AAAA:
		priv->ip6_pkt = pkt;
		return;
	case LDNS_RR_TYPE_SRV:
		/* FIXME: We only support one SRV record per name. */
		free(priv->name);
		priv->priority = ldns_rdf2native_int16(pkt->_answer->_rrs[0]->_rdata_fields[0]);
		priv->weight = ldns_rdf2native_int16(pkt->_answer->_rrs[0]->_rdata_fields[1]);
		priv->port = ldns_rdf2native_int16(pkt->_answer->_rrs[0]->_rdata_fields[2]);
		priv->name = ldns_rdf2str(pkt->_answer->_rrs[0]->_rdata_fields[3]);
		lookup(priv, AF_INET);
		lookup(priv, AF_INET6);
		break;
	case LDNS_RR_TYPE_PTR:
		/* FIXME: We only support one PTR record. */
		if (!pkt->_answer->_rr_count) {
			netresolve_backend_failed(query);
			break;
		}
		char *name = ldns_rdf2str(pkt->_answer->_rrs[0]->_rdata_fields[0]);
		netresolve_backend_add_name_info(query, name, NULL);
		free(name);
		netresolve_backend_finished(query);
		break;
	default:
		error("ignoring unknown response");
		netresolve_backend_failed(query);
	}

	ldns_pkt_free(pkt);
}

static void
resolve(struct priv_dns *priv, const char *name, int type, int class)
{
	debug("looking up %s record for %s", ldns_rr_descript(type)->_name, priv->name);
#if defined(USE_UNBOUND)
	ub_resolve_async(priv->ctx, name, type, class, priv, callback, NULL);
#elif defined(USE_ARES)
	ares_query(priv->channel, name, class, type, callback, priv);
#endif
}

static const char *
protocol_to_string(int proto)
{
	switch (proto) {
	case IPPROTO_UDP:
		return "udp";
	case IPPROTO_TCP:
		return "tcp";
	case IPPROTO_SCTP:
		return "sctp";
	default:
		return "0";
	}
}

static void
lookup(struct priv_dns *priv, int family)
{
	int type;

	if (priv->family != AF_UNSPEC && priv->family != family)
		return;

	switch (family) {
	case AF_INET:
		type = LDNS_RR_TYPE_A;
		break;
	case AF_INET6:
		type = LDNS_RR_TYPE_AAAA;
		break;
	default:
		return;
	}

	resolve(priv, priv->name, type, LDNS_RR_CLASS_IN);
}

static void
lookup_srv(struct priv_dns *priv)
{
	char *name;

	priv->protocol = netresolve_backend_get_protocol(priv->query);

	if (asprintf(&name, "_%s._%s.%s",
			netresolve_backend_get_servname(priv->query),
			protocol_to_string(priv->protocol),
			netresolve_backend_get_nodename(priv->query)) == -1) {
		error("memory allocation failed");
		netresolve_backend_failed(priv->query);
		return;
	}

	resolve(priv, name, LDNS_RR_TYPE_SRV, LDNS_RR_CLASS_IN);
}

static void
lookup_reverse(struct priv_dns *priv, int family)
{
	char *name;

	switch (family) {
	case AF_INET:
		if (asprintf(&name, "%d.%d.%d.%d.in-addr.arpa.",
				priv->address[3],
				priv->address[2],
				priv->address[1],
				priv->address[0]) == -1) {
			error("memory allocation failed");
			netresolve_backend_failed(priv->query);
			return;
		}
		break;
	default:
		debug("unknown address family: %d", family);
		netresolve_backend_failed(priv->query);
		return;
	}

	resolve(priv, name, LDNS_RR_TYPE_PTR, LDNS_RR_CLASS_IN);
}

static void
lookup_dns(struct priv_dns *priv)
{
	resolve(priv, priv->name, priv->type, priv->cls);
}


static bool
apply_pkt(netresolve_query_t query, const ldns_pkt *pkt)
{
	struct priv_dns *priv = netresolve_backend_get_priv(query);
	int rcode = ldns_pkt_get_rcode(pkt);

	if (rcode) {
		error("rcode: %d", rcode);
		return false;
	}

	ldns_rr_list *answer = ldns_pkt_answer(pkt);

	for (int i = 0; i < answer->_rr_count; i++) {
		ldns_rr *rr = answer->_rrs[i];

		switch (rr->_rr_type) {
		case LDNS_RR_TYPE_CNAME:
			break;
		case LDNS_RR_TYPE_A:
			netresolve_backend_add_path(query,
					AF_INET, rr->_rdata_fields[0]->_data, 0,
					0, priv->protocol, priv->port,
					priv->priority, priv->weight, rr->_ttl);
			break;
		case LDNS_RR_TYPE_AAAA:
			netresolve_backend_add_path(query,
					AF_INET6, rr->_rdata_fields[0]->_data, 0,
					0, priv->protocol, priv->port,
					priv->priority, priv->weight, rr->_ttl);
			break;
		default:
			break;
		}
	}

	return true;
}

struct priv_dns *
setup(netresolve_query_t query, char **settings)
{
	struct priv_dns *priv = netresolve_backend_new_priv(query, sizeof *priv);
	int status;

	if (!priv)
		goto fail;

	const char *name = netresolve_backend_get_nodename(query);
	priv->name = name ? strdup(name) : NULL;
	priv->family = netresolve_backend_get_family(query);

	priv->query = query;

#if defined(USE_UNBOUND)
	priv->ctx = ub_ctx_create();
	if(!priv->ctx)
		goto fail;

	if ((status = ub_ctx_resolvconf(priv->ctx, NULL)) != 0) {
		error("libunbound: %s", ub_strerror(status));
		goto fail;
	}
	netresolve_backend_watch_fd(query, ub_fd(priv->ctx), POLLIN);

	return priv;
#elif defined(USE_ARES)
	/* ares doesn't seem to accept const options */
	static struct ares_options options = {
		.flags = ARES_FLAG_NOSEARCH | ARES_FLAG_NOALIASES,
		.lookups = "b"
	};

	FD_ZERO(&priv->rfds);
	FD_ZERO(&priv->wfds);

	status = ares_library_init(ARES_LIB_INIT_ALL);
	if (status != ARES_SUCCESS)
		goto fail_ares;
	status = ares_init_options(&priv->channel, &options,
			ARES_OPT_FLAGS | ARES_OPT_LOOKUPS);
	if (status != ARES_SUCCESS)
		goto fail_ares;

	return priv;
fail_ares:
	error("ares: %s", ares_strerror(status));
#endif

fail:
	netresolve_backend_failed(query);
	return NULL;
}

void
setup_forward(netresolve_query_t query, char **settings)
{
	struct priv_dns *priv;
	
	if (!(priv = setup(query, settings)))
		return;

	if (!priv->name) {
		netresolve_backend_failed(query);
		return;
	}

	if (netresolve_backend_get_dns_srv_lookup(query))
		lookup_srv(priv);
	else {
		lookup(priv, AF_INET);
		lookup(priv, AF_INET6);
	}

#if defined(USE_ARES)
	register_fds(query);
#endif
}

void
setup_reverse(netresolve_query_t query, char **settings)
{
	struct priv_dns *priv;
	
	if (!(priv = setup(query, settings)))
		return;

	priv->address = netresolve_backend_get_address(query);

	if (!priv->address) {
		netresolve_backend_failed(query);
		return;
	}

	lookup_reverse(priv, AF_INET);

#if defined(USE_ARES)
	register_fds(query);
#endif
}

void
setup_dns(netresolve_query_t query, char **settings)
{
	struct priv_dns *priv;
	
	if (!(priv = setup(query, settings)))
		return;

	if (!priv->name) {
		netresolve_backend_failed(query);
		return;
	}

	netresolve_backend_get_dns_query(query, &priv->cls, &priv->type);

	lookup_dns(priv);

#if defined(USE_ARES)
	register_fds(query);
#endif
}

void
dispatch(netresolve_query_t query, int fd, int events)
{
	struct priv_dns *priv = netresolve_backend_get_priv(query);

#if defined(USE_UNBOUND)
	ub_process(priv->ctx);
#elif defined(USE_ARES)
	int rfd = events & POLLIN ? fd : ARES_SOCKET_BAD;
	int wfd = events & POLLOUT ? fd : ARES_SOCKET_BAD;

	ares_process_fd(priv->channel, rfd, wfd);
	register_fds(query);
#endif

	bool ip4_done = !!priv->ip4_pkt;
	bool ip6_done = !!priv->ip6_pkt;

	switch (priv->family) {
	case AF_UNSPEC:
		break;
	case AF_INET:
		ip6_done = true;
		break;
	case AF_INET6:
		ip4_done = true;
		break;
	default:
		netresolve_backend_failed(query);
		return;
	}

	if (ip4_done && ip6_done) {
		if (priv->ip4_pkt && !apply_pkt(query, priv->ip4_pkt)) {
			netresolve_backend_failed(query);
			return;
		}
		if (priv->ip6_pkt && !apply_pkt(query, priv->ip6_pkt)) {
			netresolve_backend_failed(query);
			return;
		}
		netresolve_backend_finished(query);
	}
}

void
cleanup(netresolve_query_t query)
{
	struct priv_dns *priv = netresolve_backend_get_priv(query);

	if (priv->ip4_pkt)
		ldns_pkt_free(priv->ip4_pkt);
	if (priv->ip6_pkt)
		ldns_pkt_free(priv->ip6_pkt);
	if (priv->name)
		free(priv->name);

#if defined(USE_UNBOUND)
	if (priv->ctx) {
		netresolve_backend_watch_fd(query, ub_fd(priv->ctx), 0);
		ub_ctx_delete(priv->ctx);
	}
#elif defined(USE_ARES)
	for (int fd = 0; fd < priv->nfds; fd++) {
		if (FD_ISSET(fd, &priv->rfds) || FD_ISSET(fd, &priv->wfds)) {
			FD_CLR(fd, &priv->rfds);
			netresolve_backend_watch_fd(query, fd, 0);
		}
	}

	ares_destroy(priv->channel);
#endif
}