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
#include <netresolve-private.h>
#include <getopt.h>
#include <ldns/ldns.h>
#include <arpa/nameser.h>

static int
count_argv(char **argv)
{
	int count = 0;

	while (*argv++)
		count++;

	return count;
}

static void
read_and_write(int rfd, int wfd)
{
	char buffer[1024];
	ssize_t rsize, wsize, offset;

	rsize = read(rfd, buffer, sizeof(buffer));
	if (rsize == -1) {
		debug("read: %s\n", strerror(errno));
		abort();
	}
	if (rsize == 0) {
		if (rfd == 0)
			return;
		else {
			debug("end of input\n");
			abort();
		}
	}
	for (offset = 0; offset < rsize; offset += wsize) {
		debug("%s: <<<%*s>>>\n",
				(rfd == 0) ? "sending" : "receiving",
				(int) (rsize - offset), buffer + offset);
		wsize = write(wfd, buffer + offset, rsize - offset);
		if (wsize <= 0) {
			debug("write: %s\n", strerror(errno));
			abort();
		}
	}
}

static void
on_connect(netresolve_query_t context, int idx, int sock, void *user_data)
{
	*(int *) user_data = sock;
}

static char *
get_dns_string(netresolve_query_t query)
{
	const uint8_t *answer;
	size_t length;

	if (!(answer = netresolve_query_get_dns_answer(query, &length)))
		return NULL;

	ldns_pkt *pkt;

	int status = ldns_wire2pkt(&pkt, answer, length);

	if (status) {
		fprintf(stderr, "ldns: %s", ldns_get_errorstr_by_id(status));
		return NULL;
	}
	
	char *result = ldns_pkt2str(pkt);

	ldns_pkt_free(pkt);
	return result;
}

int
main(int argc, char **argv)
{
	static const struct option longopts[] = {
		{ "help", 0, 0, 'h' },
		{ "verbose", 0, 0, 'v' },
		{ "connect", 0, 0, 'c' },
		{ "node", 1, 0, 'n' },
		{ "host", 1, 0, 'n' },
		{ "service", 1, 0, 's' },
		{ "family", 1, 0, 'f' },
		{ "socktype", 1, 0, 't' },
		{ "protocol", 1, 0, 'p' },
		{ "backends", 1, 0, 'b' },
		{ "srv", 0, 0, 'S' },
		{ "address", 1, 0, 'a' },
		{ "port", 1, 0, 'P' },
		{ "class", 1, 0, 'C' },
		{ "type", 1, 0, 'T' },
		{ NULL, 0, 0, 0 }
	};
	static const char *opts = "hvcn::s:f:t:p:b:Sa:P:";
	int opt, idx = 0;
	bool connect = false;
	char *nodename = NULL, *servname = NULL;
	char *address_str = NULL, *port_str = NULL;
	int cls = ns_c_in, type = 0;
	netresolve_t context;
	netresolve_query_t query;

	netresolve_set_log_level(NETRESOLVE_LOG_LEVEL_ERROR);

	context = netresolve_context_new();
	if (!context) {
		fprintf(stderr, "netresolve: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}

	while ((opt = getopt_long(count_argv(argv), argv, opts, longopts, &idx)) != -1) {
		switch (opt) {
		case 'h':
			fprintf(stderr,
					"-h,--help -- help\n"
					"-v,--verbose -- show more verbose output\n"
					"-c,--connect -- attempt to connect to a host like netcat/socat\n"
					"-n,--node <nodename> -- node name\n"
					"-s,--service <servname> -- service name\n"
					"-f,--family any|ip4|ip6 -- family name\n"
					"-t,--socktype any|stream|dgram|seqpacket -- socket type\n"
					"-p,--protocol any|tcp|udp|sctp -- transport protocol\n"
					"-b,--backends <backends> -- comma-separated list of backends\n"
					"-S,--srv -- resolve DNS SRV record\n"
					"-a,--address -- IPv4/IPv6 address (reverse query)\n"
					"-P,--port -- TCP/UDP port\n"
					"-C,--class -- DNS record class\n"
					"-T,--type -- DNS record type\n");
			exit(EXIT_SUCCESS);
		case 'v':
			netresolve_set_log_level(NETRESOLVE_LOG_LEVEL_DEBUG);
			break;
		case 'c':
			connect = true;
			break;
		case 'n':
			nodename = optarg;
			break;
		case 's':
			servname = optarg;
			break;
		case 'f':
			netresolve_context_set_options(context,
					NETRESOLVE_OPTION_FAMILY, netresolve_family_from_string(optarg),
					NETRESOLVE_OPTION_DONE);
			break;
		case 't':
			netresolve_context_set_options(context,
					NETRESOLVE_OPTION_SOCKTYPE, netresolve_socktype_from_string(optarg),
					NETRESOLVE_OPTION_DONE);
			break;
		case 'p':
			netresolve_context_set_options(context,
					NETRESOLVE_OPTION_PROTOCOL, netresolve_protocol_from_string(optarg),
					NETRESOLVE_OPTION_DONE);
			break;
		case 'b':
			netresolve_set_backend_string(context, optarg);
			break;
		case 'S':
			netresolve_context_set_options(context,
					NETRESOLVE_OPTION_DNS_SRV_LOOKUP, (int) true,
					NETRESOLVE_OPTION_DONE);
			break;
		case 'a':
			address_str = optarg;
			break;
		case 'P':
			port_str = optarg;
			break;
		case 'C':
			cls = ldns_get_rr_class_by_name(optarg);
			break;
		case 'T':
			type = ldns_get_rr_type_by_name(optarg);
			break;
		default:
			exit(EXIT_FAILURE);
		}
	}

	if (argv[optind])
		abort();

	if (connect) {
		int sock = -1;
		struct pollfd fds[2];

		netresolve_connect(context, nodename, servname, -1, -1, -1, on_connect, &sock);

		if (sock == -1) {
			fprintf(stderr, "no connection\n");
			return EXIT_FAILURE;
		}

		fprintf(stderr, "Connected.\n");

		fds[0].fd = 0;
		fds[0].events = POLLIN;
		fds[1].fd = sock;
		fds[1].events = POLLIN;

		while (true) {
			if (poll(fds, 2, -1) == -1) {
				fprintf(stderr, "poll: %s\n", strerror(errno));
				break;
			}

			if (fds[0].revents & POLLIN)
				read_and_write(0, sock);
			if (fds[1].revents & POLLIN)
				read_and_write(sock, 1);
		}

		return EXIT_SUCCESS;
	} else if (type)
		query = netresolve_query_dns(context, nodename, cls, type, NULL, NULL);
	else if (address_str || port_str) {
		Address address;
		int family, ifindex;

		if (!netresolve_backend_parse_address(address_str, &address, &family, &ifindex))
			return EXIT_FAILURE;
		query = netresolve_query_reverse(context, family, &address, ifindex, -1, port_str ? strtol(port_str, NULL, 10) : 0, NULL, NULL);
	} else
		query = netresolve_query_forward(context, nodename, servname, NULL, NULL);

	if (!query) {
		fprintf(stderr, "netresolve: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}

	debug("%s", netresolve_get_request_string(query));

	const char *response_string = netresolve_get_response_string(query);
	char *dns_string = get_dns_string(query);

	if (response_string)
		printf("%s", response_string);
	if (dns_string) {
		printf("%s", dns_string);
		free(dns_string);
	}

	netresolve_context_free(context);
	return EXIT_SUCCESS;
}
