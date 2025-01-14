// SPDX-License-Identifier: GPL-2.0-or-later

/* PASST - Plug A Simple Socket Transport
 *  for qemu/UNIX domain socket mode
 *
 * PASTA - Pack A Subtle Tap Abstraction
 *  for network namespace/tap device mode
 *
 * udp.c - UDP L2-L4 translation routines
 *
 * Copyright (c) 2020-2021 Red Hat GmbH
 * Author: Stefano Brivio <sbrivio@redhat.com>
 */

/**
 * DOC: Theory of Operation
 *
 *
 * For UDP, a reduced version of port-based connection tracking is implemented
 * with two purposes:
 * - binding ephemeral ports when they're used as source port by the guest, so
 *   that replies on those ports can be forwarded back to the guest, with a
 *   fixed timeout for this binding
 * - packets received from the local host get their source changed to a local
 *   address (gateway address) so that they can be forwarded to the guest, and
 *   packets sent as replies by the guest need their destination address to
 *   be changed back to the address of the local host. This is dynamic to allow
 *   connections from the gateway as well, and uses the same fixed 180s timeout
 * 
 * Sockets for bound ports are created at initialisation time, one set for IPv4
 * and one for IPv6.
 *
 * Packets are forwarded back and forth, by prepending and stripping UDP headers
 * in the obvious way, with no port translation.
 *
 * In PASTA mode, the L2-L4 translation is skipped for connections to ports
 * bound between namespaces using the loopback interface, messages are directly
 * transferred between L4 sockets instead. These are called spliced connections
 * for consistency with the TCP implementation, but the splice() syscall isn't
 * actually used as it wouldn't make sense for datagram-based connections: a
 * pair of recvmmsg() and sendmmsg() deals with this case.
 *
 * The connection tracking for PASTA mode is slightly complicated by the absence
 * of actual connections, see struct udp_splice_port, and these examples:
 *
 * - from init to namespace:
 *
 *   - forward direction: 127.0.0.1:5000 -> 127.0.0.1:80 in init from socket s,
 *     with epoll reference: index = 80, splice = 1, orig = 1, ns = 0
 *     - if udp_splice_ns[V4][5000].sock:
 *       - send packet to udp_splice_ns[V4][5000].sock, with destination port
 *         80
 *     - otherwise:
 *       - create new socket udp_splice_ns[V4][5000].sock
 *       - bind in namespace to 127.0.0.1:5000
 *       - add to epoll with reference: index = 5000, splice = 1, orig = 0,
 *         ns = 1
 *     - update udp_splice_init[V4][80].ts and udp_splice_ns[V4][5000].ts with
 *       current time
 *
 *   - reverse direction: 127.0.0.1:80 -> 127.0.0.1:5000 in namespace socket s,
 *     having epoll reference: index = 5000, splice = 1, orig = 0, ns = 1
 *     - if udp_splice_init[V4][80].sock:
 *       - send to udp_splice_init[V4][80].sock, with destination port 5000
 *       - update udp_splice_init[V4][80].ts and udp_splice_ns[V4][5000].ts with
 *         current time
 *     - otherwise, discard
 *
 * - from namespace to init:
 *
 *   - forward direction: 127.0.0.1:2000 -> 127.0.0.1:22 in namespace from
 *     socket s, with epoll reference: index = 22, splice = 1, orig = 1, ns = 1
 *     - if udp4_splice_init[V4][2000].sock:
 *       - send packet to udp_splice_init[V4][2000].sock, with destination
 *         port 22
 *     - otherwise:
 *       - create new socket udp_splice_init[V4][2000].sock
 *       - bind in init to 127.0.0.1:2000
 *       - add to epoll with reference: index = 2000, splice = 1, orig = 0,
 *         ns = 0
 *     - update udp_splice_ns[V4][22].ts and udp_splice_init[V4][2000].ts with
 *       current time
 *
 *   - reverse direction: 127.0.0.1:22 -> 127.0.0.1:2000 in init from socket s,
 *     having epoll reference: index = 2000, splice = 1, orig = 0, ns = 0
 *   - if udp_splice_ns[V4][22].sock:
 *     - send to udp_splice_ns[V4][22].sock, with destination port 2000
 *     - update udp_splice_ns[V4][22].ts and udp_splice_init[V4][2000].ts with
 *       current time
 *   - otherwise, discard
 */

#include <sched.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <assert.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>

#include "checksum.h"
#include "util.h"
#include "ip.h"
#include "siphash.h"
#include "inany.h"
#include "passt.h"
#include "tap.h"
#include "pcap.h"
#include "log.h"

#define UDP_CONN_TIMEOUT	180 /* s, timeout for ephemeral or local bind */
#define UDP_MAX_FRAMES		32  /* max # of frames to receive at once */

/**
 * struct udp_tap_port - Port tracking based on tap-facing source port
 * @sock:	Socket bound to source port used as index
 * @flags:	Flags for recent activity type seen from/to port
 * @ts:		Activity timestamp from tap, used for socket aging
 */
struct udp_tap_port {
	int sock;
	uint8_t flags;
#define PORT_LOCAL	BIT(0)	/* Port was contacted from local address */
#define PORT_LOOPBACK	BIT(1)	/* Port was contacted from loopback address */
#define PORT_GUA	BIT(2)	/* Port was contacted from global unicast */
#define PORT_DNS_FWD	BIT(3)	/* Port used as source for DNS remapped query */

	time_t ts;
};

/**
 * struct udp_splice_port - Bound socket for spliced communication
 * @sock:	Socket bound to index port
 * @ts:		Activity timestamp
 */
struct udp_splice_port {
	int sock;
	time_t ts;
};

/* Port tracking, arrays indexed by packet source port (host order) */
static struct udp_tap_port	udp_tap_map	[IP_VERSIONS][NUM_PORTS];

/* "Spliced" sockets indexed by bound port (host order) */
static struct udp_splice_port udp_splice_ns  [IP_VERSIONS][NUM_PORTS];
static struct udp_splice_port udp_splice_init[IP_VERSIONS][NUM_PORTS];

enum udp_act_type {
	UDP_ACT_TAP,
	UDP_ACT_SPLICE_NS,
	UDP_ACT_SPLICE_INIT,
	UDP_ACT_TYPE_MAX,
};

/* Activity-based aging for bindings */
static uint8_t udp_act[IP_VERSIONS][UDP_ACT_TYPE_MAX][DIV_ROUND_UP(NUM_PORTS, 8)];

/* Static buffers */

/**
 * udp4_l2_buf_t - Pre-cooked IPv4 packet buffers for tap connections
 * @s_in:	Source socket address, filled in by recvmmsg()
 * @taph:	Tap-level headers (partially pre-filled)
 * @iph:	Pre-filled IP header (except for tot_len and saddr)
 * @uh:		Headroom for UDP header
 * @data:	Storage for UDP payload
 */
static struct udp4_l2_buf_t {
	struct sockaddr_in s_in;

	struct tap_hdr taph;
	struct iphdr iph;
	struct udphdr uh;
	uint8_t data[USHRT_MAX -
		     (sizeof(struct iphdr) + sizeof(struct udphdr))];
} __attribute__ ((packed, aligned(__alignof__(unsigned int))))
udp4_l2_buf[UDP_MAX_FRAMES];

/**
 * udp6_l2_buf_t - Pre-cooked IPv6 packet buffers for tap connections
 * @s_in6:	Source socket address, filled in by recvmmsg()
 * @taph:	Tap-level headers (partially pre-filled)
 * @ip6h:	Pre-filled IP header (except for payload_len and addresses)
 * @uh:		Headroom for UDP header
 * @data:	Storage for UDP payload
 */
struct udp6_l2_buf_t {
	struct sockaddr_in6 s_in6;
#ifdef __AVX2__
	/* Align ip6h to 32-byte boundary. */
	uint8_t pad[64 - (sizeof(struct sockaddr_in6) + sizeof(struct ethhdr) +
			  sizeof(uint32_t))];
#endif

	struct tap_hdr taph;
	struct ipv6hdr ip6h;
	struct udphdr uh;
	uint8_t data[USHRT_MAX -
		     (sizeof(struct ipv6hdr) + sizeof(struct udphdr))];
#ifdef __AVX2__
} __attribute__ ((packed, aligned(32)))
#else
} __attribute__ ((packed, aligned(__alignof__(unsigned int))))
#endif
udp6_l2_buf[UDP_MAX_FRAMES];

/* recvmmsg()/sendmmsg() data for tap */
static struct iovec	udp4_l2_iov_sock	[UDP_MAX_FRAMES];
static struct iovec	udp6_l2_iov_sock	[UDP_MAX_FRAMES];

static struct iovec	udp4_l2_iov_tap		[UDP_MAX_FRAMES];
static struct iovec	udp6_l2_iov_tap		[UDP_MAX_FRAMES];

static struct mmsghdr	udp4_l2_mh_sock		[UDP_MAX_FRAMES];
static struct mmsghdr	udp6_l2_mh_sock		[UDP_MAX_FRAMES];

/* recvmmsg()/sendmmsg() data for "spliced" connections */
static struct iovec	udp4_iov_splice		[UDP_MAX_FRAMES];
static struct iovec	udp6_iov_splice		[UDP_MAX_FRAMES];

static struct sockaddr_in udp4_localname = {
	.sin_family = AF_INET,
	.sin_addr = IN4ADDR_LOOPBACK_INIT,
};
static struct sockaddr_in6 udp6_localname = {
	.sin6_family = AF_INET6,
	.sin6_addr = IN6ADDR_LOOPBACK_INIT,
};

static struct mmsghdr	udp4_mh_splice		[UDP_MAX_FRAMES];
static struct mmsghdr	udp6_mh_splice		[UDP_MAX_FRAMES];

/**
 * udp_portmap_clear() - Clear UDP port map before configuration
 */
void udp_portmap_clear(void)
{
	unsigned i;

	for (i = 0; i < NUM_PORTS; i++) {
		udp_tap_map[V4][i].sock = udp_tap_map[V6][i].sock = -1;
		udp_splice_ns[V4][i].sock = udp_splice_ns[V6][i].sock = -1;
		udp_splice_init[V4][i].sock = udp_splice_init[V6][i].sock = -1;
	}
}

/**
 * udp_invert_portmap() - Compute reverse port translations for return packets
 * @fwd:	Port forwarding configuration to compute reverse map for
 */
static void udp_invert_portmap(struct udp_fwd_ports *fwd)
{
	unsigned int i;

	static_assert(ARRAY_SIZE(fwd->f.delta) == ARRAY_SIZE(fwd->rdelta),
		      "Forward and reverse delta arrays must have same size");
	for (i = 0; i < ARRAY_SIZE(fwd->f.delta); i++) {
		in_port_t delta = fwd->f.delta[i];
		in_port_t rport = i + delta;

		if (delta)
			fwd->rdelta[rport] = NUM_PORTS - delta;
	}
}

/**
 * udp_update_l2_buf() - Update L2 buffers with Ethernet and IPv4 addresses
 * @eth_d:	Ethernet destination address, NULL if unchanged
 * @eth_s:	Ethernet source address, NULL if unchanged
 */
void udp_update_l2_buf(const unsigned char *eth_d, const unsigned char *eth_s)
{
	int i;

	for (i = 0; i < UDP_MAX_FRAMES; i++) {
		struct udp4_l2_buf_t *b4 = &udp4_l2_buf[i];
		struct udp6_l2_buf_t *b6 = &udp6_l2_buf[i];

		eth_update_mac(&b4->taph.eh, eth_d, eth_s);
		eth_update_mac(&b6->taph.eh, eth_d, eth_s);
	}
}

/**
 * udp_sock4_iov_init_one() - Initialise a scatter-gather L2 buffer for IPv4
 * @c:		Execution context
 * @i:		Index of buffer to initialize
 */
static void udp_sock4_iov_init_one(const struct ctx *c, size_t i)
{
	struct msghdr *mh = &udp4_l2_mh_sock[i].msg_hdr;
	struct udp4_l2_buf_t *buf = &udp4_l2_buf[i];
	struct iovec *siov = &udp4_l2_iov_sock[i];
	struct iovec *tiov = &udp4_l2_iov_tap[i];

	*buf = (struct udp4_l2_buf_t) {
		.taph = TAP_HDR_INIT(ETH_P_IP),
		.iph = L2_BUF_IP4_INIT(IPPROTO_UDP)
	};

	siov->iov_base	= buf->data;
	siov->iov_len	= sizeof(buf->data);

	mh->msg_name	= &buf->s_in;
	mh->msg_namelen	= sizeof(buf->s_in);
	mh->msg_iov	= siov;
	mh->msg_iovlen	= 1;

	tiov->iov_base	= tap_frame_base(c, &buf->taph);
}

/**
 * udp_sock6_iov_init_one() - Initialise a scatter-gather L2 buffer for IPv6
 * @c:		Execution context
 * @i:		Index of buffer to initialize
 */
static void udp_sock6_iov_init_one(const struct ctx *c, size_t i)
{
	struct msghdr *mh = &udp6_l2_mh_sock[i].msg_hdr;
	struct udp6_l2_buf_t *buf = &udp6_l2_buf[i];
	struct iovec *siov = &udp6_l2_iov_sock[i];
	struct iovec *tiov = &udp6_l2_iov_tap[i];

	*buf = (struct udp6_l2_buf_t) {
		.taph = TAP_HDR_INIT(ETH_P_IPV6),
		.ip6h = L2_BUF_IP6_INIT(IPPROTO_UDP)
	};

	siov->iov_base	= buf->data;
	siov->iov_len	= sizeof(buf->data);

	mh->msg_name	= &buf->s_in6;
	mh->msg_namelen	= sizeof(buf->s_in6);
	mh->msg_iov	= siov;
	mh->msg_iovlen	= 1;

	tiov->iov_base	= tap_frame_base(c, &buf->taph);
}

/**
 * udp_sock_iov_init() - Initialise scatter-gather L2 buffers
 * @c:		Execution context
 */
static void udp_sock_iov_init(const struct ctx *c)
{
	size_t i;

	for (i = 0; i < UDP_MAX_FRAMES; i++) {
		if (c->ifi4)
			udp_sock4_iov_init_one(c, i);
		if (c->ifi6)
			udp_sock6_iov_init_one(c, i);
	}
}

/**
 * udp_splice_new() - Create and prepare socket for "spliced" binding
 * @c:		Execution context
 * @v6:		Set for IPv6 sockets
 * @src:	Source port of original connection, host order
 * @ns:		Does the splice originate in the ns or not
 *
 * Return: prepared socket, negative error code on failure
 *
 * #syscalls:pasta getsockname
 */
int udp_splice_new(const struct ctx *c, int v6, in_port_t src, bool ns)
{
	struct epoll_event ev = { .events = EPOLLIN | EPOLLRDHUP | EPOLLHUP };
	union epoll_ref ref = { .type = EPOLL_TYPE_UDP,
				.udp = { .splice = true, .v6 = v6, .port = src }
			      };
	struct udp_splice_port *sp;
	int act, s;

	if (ns) {
		ref.udp.pif = PIF_SPLICE;
		sp = &udp_splice_ns[v6 ? V6 : V4][src];
		act = UDP_ACT_SPLICE_NS;
	} else {
		ref.udp.pif = PIF_HOST;
		sp = &udp_splice_init[v6 ? V6 : V4][src];
		act = UDP_ACT_SPLICE_INIT;
	}

	s = socket(v6 ? AF_INET6 : AF_INET, SOCK_DGRAM | SOCK_NONBLOCK,
		   IPPROTO_UDP);

	if (s > FD_REF_MAX) {
		close(s);
		return -EIO;
	}

	if (s < 0)
		return s;

	ref.fd = s;

	if (v6) {
		struct sockaddr_in6 addr6 = {
			.sin6_family = AF_INET6,
			.sin6_port = htons(src),
			.sin6_addr = IN6ADDR_LOOPBACK_INIT,
		};
		if (bind(s, (struct sockaddr *)&addr6, sizeof(addr6)))
			goto fail;
	} else {
		struct sockaddr_in addr4 = {
			.sin_family = AF_INET,
			.sin_port = htons(src),
			.sin_addr = IN4ADDR_LOOPBACK_INIT,
		};
		if (bind(s, (struct sockaddr *)&addr4, sizeof(addr4)))
			goto fail;
	}

	sp->sock = s;
	bitmap_set(udp_act[v6 ? V6 : V4][act], src);

	ev.data.u64 = ref.u64;
	epoll_ctl(c->epollfd, EPOLL_CTL_ADD, s, &ev);
	return s;

fail:
	close(s);
	return -1;
}

/**
 * struct udp_splice_new_ns_arg - Arguments for udp_splice_new_ns()
 * @c:		Execution context
 * @v6:		Set for IPv6
 * @src:	Source port of originating datagram, host order
 * @dst:	Destination port of originating datagram, host order
 * @s:		Newly created socket or negative error code
 */
struct udp_splice_new_ns_arg {
	const struct ctx *c;
	int v6;
	in_port_t src;
	int s;
};

/**
 * udp_splice_new_ns() - Enter namespace and call udp_splice_new()
 * @arg:	See struct udp_splice_new_ns_arg
 *
 * Return: 0
 */
static int udp_splice_new_ns(void *arg)
{
	struct udp_splice_new_ns_arg *a;

	a = (struct udp_splice_new_ns_arg *)arg;

	ns_enter(a->c);

	a->s = udp_splice_new(a->c, a->v6, a->src, true);

	return 0;
}

/**
 * udp_mmh_splice_port() - Is source address of message suitable for splicing?
 * @v6:		Is @sa a sockaddr_in6 (otherwise sockaddr_in)?
 * @mmh:	mmsghdr of incoming message
 *
 * Return: if @sa refers to localhost (127.0.0.1 or ::1) the port from
 *         @sa in host order, otherwise -1.
 */
static int udp_mmh_splice_port(bool v6, const struct mmsghdr *mmh)
{
	const struct sockaddr_in6 *sa6 = mmh->msg_hdr.msg_name;
	const struct sockaddr_in *sa4 = mmh->msg_hdr.msg_name;

	if (v6 && IN6_IS_ADDR_LOOPBACK(&sa6->sin6_addr))
		return ntohs(sa6->sin6_port);

	if (!v6 && IN4_IS_ADDR_LOOPBACK(&sa4->sin_addr))
		return ntohs(sa4->sin_port);

	return -1;
}

/**
 * udp_splice_sendfrom() - Send datagrams from given port to given port
 * @c:		Execution context
 * @start:	Index of first datagram in udp[46]_l2_buf
 * @n:		Number of datagrams to send
 * @src:	Datagrams will be sent from this port (on origin side)
 * @dst:	Datagrams will be send to this port (on destination side)
 * @from_pif:	pif from which the packet originated
 * @v6:		Send as IPv6?
 * @allow_new:	If true create sending socket if needed, if false discard
 *              if no sending socket is available
 * @now:	Timestamp
 */
static void udp_splice_sendfrom(const struct ctx *c, unsigned start, unsigned n,
				in_port_t src, in_port_t dst, uint8_t from_pif,
				bool v6, bool allow_new,
				const struct timespec *now)
{
	struct mmsghdr *mmh_recv, *mmh_send;
	unsigned int i;
	int s;

	if (v6) {
		mmh_recv = udp6_l2_mh_sock;
		mmh_send = udp6_mh_splice;
	} else {
		mmh_recv = udp4_l2_mh_sock;
		mmh_send = udp4_mh_splice;
	}

	if (from_pif == PIF_SPLICE) {
		src += c->udp.fwd_in.rdelta[src];
		s = udp_splice_init[v6][src].sock;
		if (s < 0 && allow_new)
			s = udp_splice_new(c, v6, src, false);

		if (s < 0)
			return;

		udp_splice_ns[v6][dst].ts = now->tv_sec;
		udp_splice_init[v6][src].ts = now->tv_sec;
	} else {
		ASSERT(from_pif == PIF_HOST);
		src += c->udp.fwd_out.rdelta[src];
		s = udp_splice_ns[v6][src].sock;
		if (s < 0 && allow_new) {
			struct udp_splice_new_ns_arg arg = {
				c, v6, src, -1,
			};

			NS_CALL(udp_splice_new_ns, &arg);
			s = arg.s;
		}
		if (s < 0)
			return;

		udp_splice_init[v6][dst].ts = now->tv_sec;
		udp_splice_ns[v6][src].ts = now->tv_sec;
	}

	for (i = start; i < start + n; i++)
		mmh_send[i].msg_hdr.msg_iov->iov_len = mmh_recv[i].msg_len;

	sendmmsg(s, mmh_send + start, n, MSG_NOSIGNAL);
}

/**
 * udp_update_hdr4() - Update headers for one IPv4 datagram
 * @c:		Execution context
 * @b:		Pointer to udp4_l2_buf to update
 * @dstport:	Destination port number
 * @datalen:	Length of UDP payload
 * @now:	Current timestamp
 *
 * Return: size of tap frame with headers
 */
static size_t udp_update_hdr4(const struct ctx *c, struct udp4_l2_buf_t *b,
			      in_port_t dstport, size_t datalen,
			      const struct timespec *now)
{
	size_t ip_len = datalen + sizeof(b->iph) + sizeof(b->uh);
	in_port_t srcport = ntohs(b->s_in.sin_port);
	struct in_addr src = b->s_in.sin_addr;

	if (!IN4_IS_ADDR_UNSPECIFIED(&c->ip4.dns_match) &&
	    IN4_ARE_ADDR_EQUAL(&src, &c->ip4.dns_host) && srcport == 53 &&
	    (udp_tap_map[V4][dstport].flags & PORT_DNS_FWD)) {
		src = c->ip4.dns_match;
	} else if (IN4_IS_ADDR_LOOPBACK(&src) ||
		   IN4_ARE_ADDR_EQUAL(&src, &c->ip4.addr_seen)) {
		udp_tap_map[V4][srcport].ts = now->tv_sec;
		udp_tap_map[V4][srcport].flags |= PORT_LOCAL;

		if (IN4_IS_ADDR_LOOPBACK(&src))
			udp_tap_map[V4][srcport].flags |= PORT_LOOPBACK;
		else
			udp_tap_map[V4][srcport].flags &= ~PORT_LOOPBACK;

		bitmap_set(udp_act[V4][UDP_ACT_TAP], srcport);

		src = c->ip4.gw;
	}

	b->iph.tot_len = htons(ip_len);
	b->iph.daddr = c->ip4.addr_seen.s_addr;
	b->iph.saddr = src.s_addr;
	b->iph.check = csum_ip4_header(b->iph.tot_len, IPPROTO_UDP,
				       src, c->ip4.addr_seen);

	b->uh.source = b->s_in.sin_port;
	b->uh.dest = htons(dstport);
	b->uh.len = htons(datalen + sizeof(b->uh));

	return tap_frame_len(c, &b->taph, ip_len);
}

/**
 * udp_update_hdr6() - Update headers for one IPv6 datagram
 * @c:		Execution context
 * @b:		Pointer to udp6_l2_buf to update
 * @dstport:	Destination port number
 * @datalen:	Length of UDP payload
 * @now:	Current timestamp
 *
 * Return: size of tap frame with headers
 */
static size_t udp_update_hdr6(const struct ctx *c, struct udp6_l2_buf_t *b,
			      in_port_t dstport, size_t datalen,
			      const struct timespec *now)
{
	const struct in6_addr *src = &b->s_in6.sin6_addr;
	const struct in6_addr *dst = &c->ip6.addr_seen;
	uint16_t payload_len = datalen + sizeof(b->uh);
	in_port_t srcport = ntohs(b->s_in6.sin6_port);

	if (IN6_IS_ADDR_LINKLOCAL(src)) {
		dst = &c->ip6.addr_ll_seen;
	} else if (!IN6_IS_ADDR_UNSPECIFIED(&c->ip6.dns_match) &&
		   IN6_ARE_ADDR_EQUAL(src, &c->ip6.dns_host) &&
		   srcport == 53 &&
		   (udp_tap_map[V4][dstport].flags & PORT_DNS_FWD)) {
		src = &c->ip6.dns_match;
	} else if (IN6_IS_ADDR_LOOPBACK(src)			||
		   IN6_ARE_ADDR_EQUAL(src, &c->ip6.addr_seen)	||
		   IN6_ARE_ADDR_EQUAL(src, &c->ip6.addr)) {
		udp_tap_map[V6][srcport].ts = now->tv_sec;
		udp_tap_map[V6][srcport].flags |= PORT_LOCAL;

		if (IN6_IS_ADDR_LOOPBACK(src))
			udp_tap_map[V6][srcport].flags |= PORT_LOOPBACK;
		else
			udp_tap_map[V6][srcport].flags &= ~PORT_LOOPBACK;

		if (IN6_ARE_ADDR_EQUAL(src, &c->ip6.addr))
			udp_tap_map[V6][srcport].flags |= PORT_GUA;
		else
			udp_tap_map[V6][srcport].flags &= ~PORT_GUA;

		bitmap_set(udp_act[V6][UDP_ACT_TAP], srcport);

		dst = &c->ip6.addr_ll_seen;

		if (IN6_IS_ADDR_LINKLOCAL(&c->ip6.gw))
			src = &c->ip6.gw;
		else
			src = &c->ip6.addr_ll;

	}

	b->ip6h.payload_len = htons(payload_len);
	b->ip6h.daddr = *dst;
	b->ip6h.saddr = *src;
	b->ip6h.version = 6;
	b->ip6h.nexthdr = IPPROTO_UDP;
	b->ip6h.hop_limit = 255;

	b->uh.source = b->s_in6.sin6_port;
	b->uh.dest = htons(dstport);
	b->uh.len = b->ip6h.payload_len;
	csum_udp6(&b->uh, src, dst, b->data, datalen);

	return tap_frame_len(c, &b->taph, payload_len + sizeof(b->ip6h));
}

/**
 * udp_tap_send() - Prepare UDP datagrams and send to tap interface
 * @c:		Execution context
 * @start:	Index of first datagram in udp[46]_l2_buf pool
 * @n:		Number of datagrams to send
 * @dstport:	Destination port number
 * @v6:		True if using IPv6
 * @now:	Current timestamp
 *
 * Return: size of tap frame with headers
 */
static void udp_tap_send(const struct ctx *c,
			 unsigned int start, unsigned int n,
			 in_port_t dstport, bool v6, const struct timespec *now)
{
	struct iovec *tap_iov;
	unsigned int i;

	if (v6)
		tap_iov = udp6_l2_iov_tap;
	else
		tap_iov = udp4_l2_iov_tap;

	for (i = start; i < start + n; i++) {
		size_t buf_len;

		if (v6)
			buf_len = udp_update_hdr6(c, &udp6_l2_buf[i], dstport,
						  udp6_l2_mh_sock[i].msg_len, now);
		else
			buf_len = udp_update_hdr4(c, &udp4_l2_buf[i], dstport,
						  udp4_l2_mh_sock[i].msg_len, now);

		tap_iov[i].iov_len = buf_len;
	}

	tap_send_frames(c, tap_iov + start, 1, n);
}

/**
 * udp_sock_handler() - Handle new data from socket
 * @c:		Execution context
 * @ref:	epoll reference
 * @events:	epoll events bitmap
 * @now:	Current timestamp
 *
 * #syscalls recvmmsg
 */
void udp_sock_handler(const struct ctx *c, union epoll_ref ref, uint32_t events,
		      const struct timespec *now)
{
	/* For not entirely clear reasons (data locality?) pasta gets
	 * better throughput if we receive tap datagrams one at a
	 * atime.  For small splice datagrams throughput is slightly
	 * better if we do batch, but it's slightly worse for large
	 * splice datagrams.  Since we don't know before we receive
	 * whether we'll use tap or splice, always go one at a time
	 * for pasta mode.
	 */
	ssize_t n = (c->mode == MODE_PASST ? UDP_MAX_FRAMES : 1);
	in_port_t dstport = ref.udp.port;
	bool v6 = ref.udp.v6;
	struct mmsghdr *mmh_recv;
	int i, m;

	if (c->no_udp || !(events & EPOLLIN))
		return;

	if (ref.udp.pif == PIF_SPLICE)
		dstport += c->udp.fwd_out.f.delta[dstport];
	else if (ref.udp.pif == PIF_HOST)
		dstport += c->udp.fwd_in.f.delta[dstport];

	if (v6) {
		mmh_recv = udp6_l2_mh_sock;
		udp6_localname.sin6_port = htons(dstport);
	} else {
		mmh_recv = udp4_l2_mh_sock;
		udp4_localname.sin_port = htons(dstport);
	}

	n = recvmmsg(ref.fd, mmh_recv, n, 0, NULL);
	if (n <= 0)
		return;

	for (i = 0; i < n; i += m) {
		int splicefrom = -1;
		m = n;

		if (ref.udp.splice) {
			splicefrom = udp_mmh_splice_port(v6, mmh_recv + i);

			for (m = 1; i + m < n; m++) {
				int p;

				p = udp_mmh_splice_port(v6, mmh_recv + i + m);
				if (p != splicefrom)
					break;
			}
		}

		if (splicefrom >= 0)
			udp_splice_sendfrom(c, i, m, splicefrom, dstport,
					    ref.udp.pif, v6, ref.udp.orig, now);
		else
			udp_tap_send(c, i, m, dstport, v6, now);
	}
}

/**
 * udp_tap_handler() - Handle packets from tap
 * @c:		Execution context
 * @pif:	pif on which the packet is arriving
 * @af:		Address family, AF_INET or AF_INET6
 * @saddr:	Source address
 * @daddr:	Destination address
 * @p:		Pool of UDP packets, with UDP headers
 * @idx:	Index of first packet to process
 * @now:	Current timestamp
 *
 * Return: count of consumed packets
 *
 * #syscalls sendmmsg
 */
int udp_tap_handler(struct ctx *c, uint8_t pif,
		    sa_family_t af, const void *saddr, const void *daddr,
		    const struct pool *p, int idx, const struct timespec *now)
{
	struct mmsghdr mm[UIO_MAXIOV];
	struct iovec m[UIO_MAXIOV];
	struct sockaddr_in6 s_in6;
	struct sockaddr_in s_in;
	const struct udphdr *uh;
	struct sockaddr *sa;
	int i, s, count = 0;
	in_port_t src, dst;
	socklen_t sl;

	(void)c;
	(void)saddr;
	(void)pif;

	uh = packet_get(p, idx, 0, sizeof(*uh), NULL);
	if (!uh)
		return 1;

	/* The caller already checks that all the messages have the same source
	 * and destination, so we can just take those from the first message.
	 */
	src = ntohs(uh->source);
	dst = ntohs(uh->dest);

	if (af == AF_INET) {
		s_in = (struct sockaddr_in) {
			.sin_family = AF_INET,
			.sin_port = uh->dest,
			.sin_addr = *(struct in_addr *)daddr,
		};

		sa = (struct sockaddr *)&s_in;
		sl = sizeof(s_in);

		if (IN4_ARE_ADDR_EQUAL(&s_in.sin_addr, &c->ip4.dns_match) &&
		    ntohs(s_in.sin_port) == 53) {
			s_in.sin_addr = c->ip4.dns_host;
			udp_tap_map[V4][src].ts = now->tv_sec;
			udp_tap_map[V4][src].flags |= PORT_DNS_FWD;
			bitmap_set(udp_act[V4][UDP_ACT_TAP], src);
		} else if (IN4_ARE_ADDR_EQUAL(&s_in.sin_addr, &c->ip4.gw) &&
			   !c->no_map_gw) {
			if (!(udp_tap_map[V4][dst].flags & PORT_LOCAL) ||
			    (udp_tap_map[V4][dst].flags & PORT_LOOPBACK))
				s_in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			else
				s_in.sin_addr = c->ip4.addr_seen;
		}

		debug("UDP from tap src=%hu dst=%hu, s=%d",
		      src, dst, udp_tap_map[V4][src].sock);
		if ((s = udp_tap_map[V4][src].sock) < 0) {
			struct in_addr bind_addr = IN4ADDR_ANY_INIT;
			union udp_epoll_ref uref = {
				.port = src,
				.pif = PIF_HOST,
			};
			const char *bind_if = NULL;

			if (!IN4_IS_ADDR_LOOPBACK(&s_in.sin_addr))
				bind_if = c->ip4.ifname_out;

			if (!IN4_IS_ADDR_LOOPBACK(&s_in.sin_addr))
				bind_addr = c->ip4.addr_out;

			s = sock_l4(c, AF_INET, IPPROTO_UDP, &bind_addr,
				    bind_if, src, uref.u32);
			if (s < 0)
				return p->count - idx;

			udp_tap_map[V4][src].sock = s;
			bitmap_set(udp_act[V4][UDP_ACT_TAP], src);
		}

		udp_tap_map[V4][src].ts = now->tv_sec;
	} else {
		s_in6 = (struct sockaddr_in6) {
			.sin6_family = AF_INET6,
			.sin6_port = uh->dest,
			.sin6_addr = *(struct in6_addr *)daddr,
		};
		const struct in6_addr *bind_addr = &in6addr_any;

		sa = (struct sockaddr *)&s_in6;
		sl = sizeof(s_in6);

		if (INANY_ARE_ADDR_EQUAL(daddr, &c->ip6.dns_match) &&
		    ntohs(s_in6.sin6_port) == 53) {
			s_in6.sin6_addr = c->ip6.dns_host;
			udp_tap_map[V6][src].ts = now->tv_sec;
			udp_tap_map[V6][src].flags |= PORT_DNS_FWD;
			bitmap_set(udp_act[V6][UDP_ACT_TAP], src);
		} else if (INANY_ARE_ADDR_EQUAL(daddr, &c->ip6.gw) &&
			   !c->no_map_gw) {
			if (!(udp_tap_map[V6][dst].flags & PORT_LOCAL) ||
			    (udp_tap_map[V6][dst].flags & PORT_LOOPBACK))
				s_in6.sin6_addr = in6addr_loopback;
			else if (udp_tap_map[V6][dst].flags & PORT_GUA)
				s_in6.sin6_addr = c->ip6.addr;
			else
				s_in6.sin6_addr = c->ip6.addr_seen;
		} else if (IN6_IS_ADDR_LINKLOCAL(&s_in6.sin6_addr)) {
			bind_addr = &c->ip6.addr_ll;
		}

		if ((s = udp_tap_map[V6][src].sock) < 0) {
			union udp_epoll_ref uref = {
				.v6 = 1,
				.port = src,
				.pif = PIF_HOST,
			};
			const char *bind_if = NULL;

			if (!IN6_IS_ADDR_LOOPBACK(&s_in6.sin6_addr))
				bind_if = c->ip6.ifname_out;

			if (!IN6_IS_ADDR_LOOPBACK(&s_in6.sin6_addr) &&
			    !IN6_IS_ADDR_LINKLOCAL(&s_in6.sin6_addr))
				bind_addr = &c->ip6.addr_out;

			s = sock_l4(c, AF_INET6, IPPROTO_UDP, bind_addr,
				    bind_if, src, uref.u32);
			if (s < 0)
				return p->count - idx;

			udp_tap_map[V6][src].sock = s;
			bitmap_set(udp_act[V6][UDP_ACT_TAP], src);
		}

		udp_tap_map[V6][src].ts = now->tv_sec;
	}

	for (i = 0; i < (int)p->count - idx; i++) {
		struct udphdr *uh_send;
		size_t len;

		uh_send = packet_get(p, idx + i, 0, sizeof(*uh), &len);
		if (!uh_send)
			return p->count - idx;

		mm[i].msg_hdr.msg_name = sa;
		mm[i].msg_hdr.msg_namelen = sl;

		if (len) {
			m[i].iov_base = (char *)(uh_send + 1);
			m[i].iov_len = len;

			mm[i].msg_hdr.msg_iov = m + i;
			mm[i].msg_hdr.msg_iovlen = 1;
		} else {
			mm[i].msg_hdr.msg_iov = NULL;
			mm[i].msg_hdr.msg_iovlen = 0;
		}

		mm[i].msg_hdr.msg_control = NULL;
		mm[i].msg_hdr.msg_controllen = 0;
		mm[i].msg_hdr.msg_flags = 0;

		count++;
	}

	count = sendmmsg(s, mm, count, MSG_NOSIGNAL);
	if (count < 0)
		return 1;

	return count;
}

/**
 * udp_sock_init() - Initialise listening sockets for a given port
 * @c:		Execution context
 * @ns:		In pasta mode, if set, bind with loopback address in namespace
 * @af:		Address family to select a specific IP version, or AF_UNSPEC
 * @addr:	Pointer to address for binding, NULL if not configured
 * @ifname:	Name of interface to bind to, NULL if not configured
 * @port:	Port, host order
 *
 * Return: 0 on (partial) success, negative error code on (complete) failure
 */
int udp_sock_init(const struct ctx *c, int ns, sa_family_t af,
		  const void *addr, const char *ifname, in_port_t port)
{
	union udp_epoll_ref uref = { .splice = (c->mode == MODE_PASTA),
				     .orig = true, .port = port };
	int s, r4 = FD_REF_MAX + 1, r6 = FD_REF_MAX + 1;

	if (ns)
		uref.pif = PIF_SPLICE;
	else
		uref.pif = PIF_HOST;

	if ((af == AF_INET || af == AF_UNSPEC) && c->ifi4) {
		uref.v6 = 0;

		if (!ns) {
			r4 = s = sock_l4(c, AF_INET, IPPROTO_UDP, addr,
					 ifname, port, uref.u32);

			udp_tap_map[V4][uref.port].sock = s < 0 ? -1 : s;
			udp_splice_init[V4][port].sock = s < 0 ? -1 : s;
		} else {
			r4 = s = sock_l4(c, AF_INET, IPPROTO_UDP,
					 &in4addr_loopback,
					 ifname, port, uref.u32);
			udp_splice_ns[V4][port].sock = s < 0 ? -1 : s;
		}
	}

	if ((af == AF_INET6 || af == AF_UNSPEC) && c->ifi6) {
		uref.v6 = 1;

		if (!ns) {
			r6 = s = sock_l4(c, AF_INET6, IPPROTO_UDP, addr,
					 ifname, port, uref.u32);

			udp_tap_map[V6][uref.port].sock = s < 0 ? -1 : s;
			udp_splice_init[V6][port].sock = s < 0 ? -1 : s;
		} else {
			r6 = s = sock_l4(c, AF_INET6, IPPROTO_UDP,
					 &in6addr_loopback,
					 ifname, port, uref.u32);
			udp_splice_ns[V6][port].sock = s < 0 ? -1 : s;
		}
	}

	if (IN_INTERVAL(0, FD_REF_MAX, r4) || IN_INTERVAL(0, FD_REF_MAX, r6))
		return 0;

	return r4 < 0 ? r4 : r6;
}

/**
 * udp_splice_iov_init() - Set up buffers and descriptors for recvmmsg/sendmmsg
 */
static void udp_splice_iov_init(void)
{
	int i;

	for (i = 0; i < UDP_MAX_FRAMES; i++) {
		struct msghdr *mh4 = &udp4_mh_splice[i].msg_hdr;
		struct msghdr *mh6 = &udp6_mh_splice[i].msg_hdr;

		mh4->msg_name = &udp4_localname;
		mh4->msg_namelen = sizeof(udp4_localname);

		mh6->msg_name = &udp6_localname;
		mh6->msg_namelen = sizeof(udp6_localname);

		udp4_iov_splice[i].iov_base = udp4_l2_buf[i].data;
		udp6_iov_splice[i].iov_base = udp6_l2_buf[i].data;

		mh4->msg_iov = &udp4_iov_splice[i];
		mh6->msg_iov = &udp6_iov_splice[i];
		mh4->msg_iovlen = mh6->msg_iovlen = 1;
	}
}

/**
 * udp_timer_one() - Handler for timed events on one port
 * @c:		Execution context
 * @v6:		Set for IPv6 connections
 * @type:	Socket type
 * @port:	Port number, host order
 * @now:	Current timestamp
 */
static void udp_timer_one(struct ctx *c, int v6, enum udp_act_type type,
			  in_port_t port, const struct timespec *now)
{
	struct udp_splice_port *sp;
	struct udp_tap_port *tp;
	int *sockp = NULL;

	switch (type) {
	case UDP_ACT_TAP:
		tp = &udp_tap_map[v6 ? V6 : V4][port];

		if (now->tv_sec - tp->ts > UDP_CONN_TIMEOUT) {
			sockp = &tp->sock;
			tp->flags = 0;
		}

		break;
	case UDP_ACT_SPLICE_INIT:
		sp = &udp_splice_init[v6 ? V6 : V4][port];

		if (now->tv_sec - sp->ts > UDP_CONN_TIMEOUT)
			sockp = &sp->sock;

		break;
	case UDP_ACT_SPLICE_NS:
		sp = &udp_splice_ns[v6 ? V6 : V4][port];

		if (now->tv_sec - sp->ts > UDP_CONN_TIMEOUT)
			sockp = &sp->sock;

		break;
	default:
		return;
	}

	if (sockp && *sockp >= 0) {
		int s = *sockp;
		*sockp = -1;
		epoll_ctl(c->epollfd, EPOLL_CTL_DEL, s, NULL);
		close(s);
		bitmap_clear(udp_act[v6 ? V6 : V4][type], port);
	}
}

/**
 * udp_port_rebind() - Rebind ports to match forward maps
 * @c:		Execution context
 * @outbound:	True to remap outbound forwards, otherwise inbound
 *
 * Must be called in namespace context if @outbound is true.
 */
static void udp_port_rebind(struct ctx *c, bool outbound)
{
	const uint8_t *fmap
		= outbound ? c->udp.fwd_out.f.map : c->udp.fwd_in.f.map;
	const uint8_t *rmap
		= outbound ? c->udp.fwd_in.f.map : c->udp.fwd_out.f.map;
	struct udp_splice_port (*socks)[NUM_PORTS]
		= outbound ? udp_splice_ns : udp_splice_init;
	unsigned port;

	for (port = 0; port < NUM_PORTS; port++) {
		if (!bitmap_isset(fmap, port)) {
			if (socks[V4][port].sock >= 0) {
				close(socks[V4][port].sock);
				socks[V4][port].sock = -1;
			}

			if (socks[V6][port].sock >= 0) {
				close(socks[V6][port].sock);
				socks[V6][port].sock = -1;
			}

			continue;
		}

		/* Don't loop back our own ports */
		if (bitmap_isset(rmap, port))
			continue;

		if ((c->ifi4 && socks[V4][port].sock == -1) ||
		    (c->ifi6 && socks[V6][port].sock == -1))
			udp_sock_init(c, outbound, AF_UNSPEC, NULL, NULL, port);
	}
}

/**
 * udp_port_rebind_outbound() - Rebind ports in namespace
 * @arg:	Execution context
 *
 * Called with NS_CALL()
 *
 * Return: 0
 */
static int udp_port_rebind_outbound(void *arg)
{
	struct ctx *c = (struct ctx *)arg;

	ns_enter(c);
	udp_port_rebind(c, true);

	return 0;
}

/**
 * udp_timer() - Scan activity bitmaps for ports with associated timed events
 * @c:		Execution context
 * @now:	Current timestamp
 */
void udp_timer(struct ctx *c, const struct timespec *now)
{
	int n, t, v6 = 0;
	unsigned int i;
	long *word, tmp;

	if (c->mode == MODE_PASTA) {
		if (c->udp.fwd_out.f.mode == FWD_AUTO) {
			fwd_scan_ports_udp(&c->udp.fwd_out.f, &c->udp.fwd_in.f,
					   &c->tcp.fwd_out, &c->tcp.fwd_in);
			NS_CALL(udp_port_rebind_outbound, c);
		}

		if (c->udp.fwd_in.f.mode == FWD_AUTO) {
			fwd_scan_ports_udp(&c->udp.fwd_in.f, &c->udp.fwd_out.f,
					   &c->tcp.fwd_in, &c->tcp.fwd_out);
			udp_port_rebind(c, false);
		}
	}

	if (!c->ifi4)
		v6 = 1;
v6:
	for (t = 0; t < UDP_ACT_TYPE_MAX; t++) {
		word = (long *)udp_act[v6 ? V6 : V4][t];
		for (i = 0; i < ARRAY_SIZE(udp_act[0][0]);
		     i += sizeof(long), word++) {
			tmp = *word;
			while ((n = ffsl(tmp))) {
				tmp &= ~(1UL << (n - 1));
				udp_timer_one(c, v6, t, i * 8 + n - 1, now);
			}
		}
	}

	if (!v6 && c->ifi6) {
		v6 = 1;
		goto v6;
	}
}

/**
 * udp_init() - Initialise per-socket data, and sockets in namespace
 * @c:		Execution context
 *
 * Return: 0
 */
int udp_init(struct ctx *c)
{
	udp_sock_iov_init(c);

	udp_invert_portmap(&c->udp.fwd_in);
	udp_invert_portmap(&c->udp.fwd_out);

	if (c->mode == MODE_PASTA) {
		udp_splice_iov_init();
		NS_CALL(udp_port_rebind_outbound, c);
	}

	return 0;
}
