// SPDX-License-Identifier: GPL-2.0-or-later

/* PASST - Plug A Simple Socket Transport
 *  for qemu/UNIX domain socket mode
 *
 * PASTA - Pack A Subtle Tap Abstraction
 *  for network namespace/tap device mode
 *
 * tcp.c - TCP L2-L4 translation state machine
 *
 * Copyright (c) 2020-2022 Red Hat GmbH
 * Author: Stefano Brivio <sbrivio@redhat.com>
 */

/**
 * DOC: Theory of Operation
 *
 *
 * PASST mode
 * ==========
 *
 * This implementation maps TCP traffic between a single L2 interface (tap) and
 * native TCP (L4) sockets, mimicking and reproducing as closely as possible the
 * inferred behaviour of applications running on a guest, connected via said L2
 * interface. Four connection flows are supported:
 * - from the local host to the guest behind the tap interface:
 *   - this is the main use case for proxies in service meshes
 *   - we bind to configured local ports, and relay traffic between L4 sockets
 *     with local endpoints and the L2 interface
 * - from remote hosts to the guest behind the tap interface:
 *   - this might be needed for services that need to be addressed directly,
 *     and typically configured with special port forwarding rules (which are
 *     not needed here)
 *   - we also relay traffic between L4 sockets with remote endpoints and the L2
 *     interface
 * - from the guest to the local host:
 *   - this is not observed in practice, but implemented for completeness and
 *     transparency
 * - from the guest to external hosts:
 *   - this might be needed for applications running on the guest that need to
 *     directly access internet services (e.g. NTP)
 *
 * Relevant goals are:
 * - transparency: sockets need to behave as if guest applications were running
 *   directly on the host. This is achieved by:
 *   - avoiding port and address translations whenever possible
 *   - mirroring TCP dynamics by observation of socket parameters (TCP_INFO
 *     socket option) and TCP headers of packets coming from the tap interface,
 *     reapplying those parameters in both flow directions (including TCP_MSS
 *     socket option)
 * - simplicity: only a small subset of TCP logic is implemented here and
 *   delegated as much as possible to the TCP implementations of guest and host
 *   kernel. This is achieved by:
 *   - avoiding a complete TCP stack reimplementation, with a modified TCP state
 *     machine focused on the translation of observed events instead
 *   - mirroring TCP dynamics as described above and hence avoiding the need for
 *     segmentation, explicit queueing, and reassembly of segments
 * - security:
 *   - no dynamic memory allocation is performed
 *   - TODO: synflood protection
 *
 * Portability is limited by usage of Linux-specific socket options.
 *
 *
 * Limits
 * ------
 *
 * To avoid the need for dynamic memory allocation, a maximum, reasonable amount
 * of connections is defined by TCP_MAX_CONNS (currently 128k).
 *
 * Data needs to linger on sockets as long as it's not acknowledged by the
 * guest, and is read using MSG_PEEK into preallocated static buffers sized
 * to the maximum supported window, 16 MiB ("discard" buffer, for already-sent
 * data) plus a number of maximum-MSS-sized buffers. This imposes a practical
 * limitation on window scaling, that is, the maximum factor is 256. Larger
 * factors will be accepted, but resulting, larger values are never advertised
 * to the other side, and not used while queueing data.
 *
 *
 * Ports
 * -----
 *
 * To avoid the need for ad-hoc configuration of port forwarding or allowed
 * ports, listening sockets can be opened and bound to all unbound ports on the
 * host, as far as process capabilities allow. This service needs to be started
 * after any application proxy that needs to bind to local ports. Mapped ports
 * can also be configured explicitly.
 *
 * No port translation is needed for connections initiated remotely or by the
 * local host: source port from socket is reused while establishing connections
 * to the guest.
 *
 * For connections initiated by the guest, it's not possible to force the same
 * source port as connections are established by the host kernel: that's the
 * only port translation needed.
 *
 *
 * Connection tracking and storage
 * -------------------------------
 *
 * Connections are tracked by struct tcp_tap_conn entries in the @tc
 * array, containing addresses, ports, TCP states and parameters. This
 * is statically allocated and indexed by an arbitrary connection
 * number. The array is compacted whenever a connection is closed, by
 * remapping the highest connection index in use to the one freed up.
 *
 * References used for the epoll interface report the connection index used for
 * the @tc array.
 *
 * IPv4 addresses are stored as IPv4-mapped IPv6 addresses to avoid the need for
 * separate data structures depending on the protocol version.
 *
 * - Inbound connection requests (to the guest) are mapped using the triple
 *   < source IP address, source port, destination port >
 * - Outbound connection requests (from the guest) are mapped using the triple
 *   < destination IP address, destination port, source port >
 *   where the source port is the one used by the guest, not the one used by the
 *   corresponding host socket
 *
 *
 * Initialisation
 * --------------
 *
 * Up to 2^15 + 2^14 listening sockets (excluding ephemeral ports, repeated for
 * IPv4 and IPv6) can be opened and bound to wildcard addresses. Some will fail
 * to bind (for low ports, or ports already bound, e.g. by a proxy). These are
 * added to the epoll list, with no separate storage.
 *
 *
 * Events and states
 * -----------------
 *
 * Instead of tracking connection states using a state machine, connection
 * events are used to determine state and actions for a given connection. This
 * makes the implementation simpler as most of the relevant tasks deal with
 * reactions to events, rather than state-associated actions. For user
 * convenience, approximate states are mapped in logs from events by
 * @tcp_state_str.
 *
 * The events are:
 *
 * - SOCK_ACCEPTED	connection accepted from socket, SYN sent to tap/guest
 *
 * - TAP_SYN_RCVD	tap/guest initiated connection, SYN received
 *
 * - TAP_SYN_ACK_SENT	SYN, ACK sent to tap/guest, valid for TAP_SYN_RCVD only
 *
 * - ESTABLISHED	connection established, the following events are valid:
 *
 * - SOCK_FIN_RCVD	FIN (EPOLLRDHUP) received from socket
 *
 * - SOCK_FIN_SENT	FIN (write shutdown) sent to socket
 *
 * - TAP_FIN_RCVD	FIN received from tap/guest
 *
 * - TAP_FIN_SENT	FIN sent to tap/guest
 *
 * - TAP_FIN_ACKED	ACK to FIN seen from tap/guest
 *
 * Setting any event in CONN_STATE_BITS (SOCK_ACCEPTED, TAP_SYN_RCVD,
 * ESTABLISHED) clears all the other events, as those represent the fundamental
 * connection states. No events (events == CLOSED) means the connection is
 * closed.
 *
 * Connection setup
 * ----------------
 *
 * - inbound connection (from socket to guest): on accept() from listening
 *   socket, the new socket is mapped in connection tracking table, and
 *   three-way handshake initiated towards the guest, advertising MSS and window
 *   size and scaling from socket parameters
 * - outbound connection (from guest to socket): on SYN segment from guest, a
 *   new socket is created and mapped in connection tracking table, setting
 *   MSS and window clamping from header and option of the observed SYN segment
 *
 *
 * Aging and timeout
 * -----------------
 *
 * Timeouts are implemented by means of timerfd timers, set based on flags:
 *
 * - SYN_TIMEOUT: if no ACK is received from tap/guest during handshake (flag
 *   ACK_FROM_TAP_DUE without ESTABLISHED event) within this time, reset the
 *   connection
 *
 * - ACK_TIMEOUT: if no ACK segment was received from tap/guest, after sending
 *   data (flag ACK_FROM_TAP_DUE with ESTABLISHED event), re-send data from the
 *   socket and reset sequence to what was acknowledged. If this persists for
 *   more than TCP_MAX_RETRANS times in a row, reset the connection
 *
 * - FIN_TIMEOUT: if a FIN segment was sent to tap/guest (flag ACK_FROM_TAP_DUE
 *   with TAP_FIN_SENT event), and no ACK is received within this time, reset
 *   the connection
 *
 * - FIN_TIMEOUT: if a FIN segment was acknowledged by tap/guest and a FIN
 *   segment (write shutdown) was sent via socket (events SOCK_FIN_SENT and
 *   TAP_FIN_ACKED), but no socket activity is detected from the socket within
 *   this time, reset the connection
 *
 * - ACT_TIMEOUT, in the presence of any event: if no activity is detected on
 *   either side, the connection is reset
 *
 * - ACK_INTERVAL elapsed after data segment received from tap without having
 *   sent an ACK segment, or zero-sized window advertised to tap/guest (flag
 *   ACK_TO_TAP_DUE): forcibly check if an ACK segment can be sent
 *
 *
 * Summary of data flows (with ESTABLISHED event)
 * ----------------------------------------------
 *
 * @seq_to_tap:		next sequence for packets to tap/guest
 * @seq_ack_from_tap:	last ACK number received from tap/guest
 * @seq_from_tap:	next sequence for packets from tap/guest (expected)
 * @seq_ack_to_tap:	last ACK number sent to tap/guest
 *
 * @seq_init_from_tap:	initial sequence number from tap/guest
 * @seq_init_to_tap:	initial sequence number from tap/guest
 *
 * @wnd_from_tap:	last window size received from tap, never scaled
 * @wnd_from_tap:	last window size advertised from tap, never scaled
 *
 * - from socket to tap/guest:
 *   - on new data from socket:
 *     - peek into buffer
 *     - send data to tap/guest:
 *       - starting at offset (@seq_to_tap - @seq_ack_from_tap)
 *       - in MSS-sized segments
 *       - increasing @seq_to_tap at each segment
 *       - up to window (until @seq_to_tap - @seq_ack_from_tap <= @wnd_from_tap)
 *     - on read error, send RST to tap/guest, close socket
 *     - on zero read, send FIN to tap/guest, set TAP_FIN_SENT
 *   - on ACK from tap/guest:
 *     - set @ts_ack_from_tap
 *     - check if it's the second duplicated ACK
 *     - consume buffer by difference between new ack_seq and @seq_ack_from_tap
 *     - update @seq_ack_from_tap from ack_seq in header
 *     - on two duplicated ACKs, reset @seq_to_tap to @seq_ack_from_tap, and
 *       resend with steps listed above
 *
 * - from tap/guest to socket:
 *   - on packet from tap/guest:
 *     - set @ts_tap_act
 *     - check seq from header against @seq_from_tap, if data is missing, send
 *       two ACKs with number @seq_ack_to_tap, discard packet
 *     - otherwise queue data to socket, set @seq_from_tap to seq from header
 *       plus payload length
 *     - in ESTABLISHED state, send ACK to tap as soon as we queue to the
 *       socket. In other states, query socket for TCP_INFO, set
 *       @seq_ack_to_tap to (tcpi_bytes_acked + @seq_init_from_tap) % 2^32 and
 *       send ACK to tap/guest
 *
 *
 * PASTA mode
 * ==========
 *
 * For traffic directed to TCP ports configured for mapping to the tuntap device
 * in the namespace, and for non-local traffic coming from the tuntap device,
 * the implementation is identical as the PASST mode described in the previous
 * section.
 *
 * For local traffic directed to TCP ports configured for direct mapping between
 * namespaces, see the implementation in tcp_splice.c.
 */

#include <sched.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <arpa/inet.h>

#include <linux/tcp.h> /* For struct tcp_info */

#include "checksum.h"
#include "util.h"
#include "ip.h"
#include "passt.h"
#include "tap.h"
#include "siphash.h"
#include "pcap.h"
#include "tcp_splice.h"
#include "log.h"
#include "inany.h"
#include "flow.h"

#include "flow_table.h"

/* Sides of a flow as we use them in "tap" connections */
#define	SOCKSIDE	0
#define	TAPSIDE		1

#define TCP_FRAMES_MEM			128
#define TCP_FRAMES							\
	(c->mode == MODE_PASST ? TCP_FRAMES_MEM : 1)

#define TCP_HASH_TABLE_LOAD		70		/* % */
#define TCP_HASH_TABLE_SIZE		(FLOW_MAX * 100 / TCP_HASH_TABLE_LOAD)

#define MAX_WS				8
#define MAX_WINDOW			(1 << (16 + (MAX_WS)))

/* MSS rounding: see SET_MSS() */
#define MSS_DEFAULT			536

struct tcp4_l2_head {	/* For MSS4 macro: keep in sync with tcp4_l2_buf_t */
#ifdef __AVX2__
	uint8_t pad[26];
#else
	uint8_t pad[2];
#endif
	struct tap_hdr taph;
	struct iphdr iph;
	struct tcphdr th;
#ifdef __AVX2__
} __attribute__ ((packed, aligned(32)));
#else
} __attribute__ ((packed, aligned(__alignof__(unsigned int))));
#endif

struct tcp6_l2_head {	/* For MSS6 macro: keep in sync with tcp6_l2_buf_t */
#ifdef __AVX2__
	uint8_t pad[14];
#else
	uint8_t pad[2];
#endif
	struct tap_hdr taph;
	struct ipv6hdr ip6h;
	struct tcphdr th;
#ifdef __AVX2__
} __attribute__ ((packed, aligned(32)));
#else
} __attribute__ ((packed, aligned(__alignof__(unsigned int))));
#endif

#define MSS4	ROUND_DOWN(USHRT_MAX - sizeof(struct tcp4_l2_head), 4)
#define MSS6	ROUND_DOWN(USHRT_MAX - sizeof(struct tcp6_l2_head), 4)

#define WINDOW_DEFAULT			14600		/* RFC 6928 */
#ifdef HAS_SND_WND
# define KERNEL_REPORTS_SND_WND(c)	(c->tcp.kernel_snd_wnd)
#else
# define KERNEL_REPORTS_SND_WND(c)	(0 && (c))
#endif

#define ACK_INTERVAL			10		/* ms */
#define SYN_TIMEOUT			10		/* s */
#define ACK_TIMEOUT			2
#define FIN_TIMEOUT			60
#define ACT_TIMEOUT			7200

#define LOW_RTT_TABLE_SIZE		8
#define LOW_RTT_THRESHOLD		10 /* us */

/* We need to include <linux/tcp.h> for tcpi_bytes_acked, instead of
 * <netinet/tcp.h>, but that doesn't include a definition for SOL_TCP
 */
#define SOL_TCP				IPPROTO_TCP

#define SEQ_LE(a, b)			((b) - (a) < MAX_WINDOW)
#define SEQ_LT(a, b)			((b) - (a) - 1 < MAX_WINDOW)
#define SEQ_GE(a, b)			((a) - (b) < MAX_WINDOW)
#define SEQ_GT(a, b)			((a) - (b) - 1 < MAX_WINDOW)

#define FIN		(1 << 0)
#define SYN		(1 << 1)
#define RST		(1 << 2)
#define ACK		(1 << 4)
/* Flags for internal usage */
#define DUP_ACK		(1 << 5)
#define ACK_IF_NEEDED	0		/* See tcp_send_flag() */

#define OPT_EOL		0
#define OPT_NOP		1
#define OPT_MSS		2
#define OPT_MSS_LEN	4
#define OPT_WS		3
#define OPT_WS_LEN	3
#define OPT_SACKP	4
#define OPT_SACK	5
#define OPT_TS		8

#define CONN_V4(conn)		(!!inany_v4(&(conn)->faddr))
#define CONN_V6(conn)		(!CONN_V4(conn))
#define CONN_IS_CLOSING(conn)						\
	((conn->events & ESTABLISHED) &&				\
	 (conn->events & (SOCK_FIN_RCVD | TAP_FIN_RCVD)))
#define CONN_HAS(conn, set)	((conn->events & (set)) == (set))

static const char *tcp_event_str[] __attribute((__unused__)) = {
	"SOCK_ACCEPTED", "TAP_SYN_RCVD", "ESTABLISHED", "TAP_SYN_ACK_SENT",

	"SOCK_FIN_RCVD", "SOCK_FIN_SENT", "TAP_FIN_RCVD", "TAP_FIN_SENT",
	"TAP_FIN_ACKED",
};

static const char *tcp_state_str[] __attribute((__unused__)) = {
	"SYN_RCVD", "SYN_SENT", "ESTABLISHED",
	"SYN_RCVD",	/* approximately maps to TAP_SYN_ACK_SENT */

	/* Passive close: */
	"CLOSE_WAIT", "CLOSE_WAIT", "LAST_ACK", "LAST_ACK", "LAST_ACK",
	/* Active close (+5): */
	"CLOSING", "FIN_WAIT_1", "FIN_WAIT_1", "FIN_WAIT_2", "TIME_WAIT",
};

static const char *tcp_flag_str[] __attribute((__unused__)) = {
	"STALLED", "LOCAL", "ACTIVE_CLOSE", "ACK_TO_TAP_DUE",
	"ACK_FROM_TAP_DUE",
};

/* Listening sockets, used for automatic port forwarding in pasta mode only */
static int tcp_sock_init_ext	[NUM_PORTS][IP_VERSIONS];
static int tcp_sock_ns		[NUM_PORTS][IP_VERSIONS];

/* Table of guest side forwarding addresses with very low RTT (assumed
 * to be local to the host), LRU
 */
static union inany_addr low_rtt_dst[LOW_RTT_TABLE_SIZE];

/**
 * tcp_buf_seq_update - Sequences to update with length of frames once sent
 * @seq:	Pointer to sequence number sent to tap-side, to be updated
 * @len:	TCP payload length
 */
struct tcp_buf_seq_update {
	uint32_t *seq;
	uint16_t len;
};

/* Static buffers */

/**
 * tcp4_l2_buf_t - Pre-cooked IPv4 packet buffers for tap connections
 * @pad:	Align TCP header to 32 bytes, for AVX2 checksum calculation only
 * @taph:	Tap-level headers (partially pre-filled)
 * @iph:	Pre-filled IP header (except for tot_len and saddr)
 * @uh:		Headroom for TCP header
 * @data:	Storage for TCP payload
 */
static struct tcp4_l2_buf_t {
#ifdef __AVX2__
	uint8_t pad[26];	/* 0, align th to 32 bytes */
#else
	uint8_t pad[2];		/*	align iph to 4 bytes	0 */
#endif
	struct tap_hdr taph;	/* 26				2 */
	struct iphdr iph;	/* 44				20 */
	struct tcphdr th;	/* 64				40 */
	uint8_t data[MSS4];	/* 84				60 */
				/* 65536			65532 */
#ifdef __AVX2__
} __attribute__ ((packed, aligned(32)))
#else
} __attribute__ ((packed, aligned(__alignof__(unsigned int))))
#endif
tcp4_l2_buf[TCP_FRAMES_MEM];

static struct tcp_buf_seq_update tcp4_l2_buf_seq_update[TCP_FRAMES_MEM];

static unsigned int tcp4_l2_buf_used;

/**
 * tcp6_l2_buf_t - Pre-cooked IPv6 packet buffers for tap connections
 * @pad:	Align IPv6 header for checksum calculation to 32B (AVX2) or 4B
 * @taph:	Tap-level headers (partially pre-filled)
 * @ip6h:	Pre-filled IP header (except for payload_len and addresses)
 * @th:		Headroom for TCP header
 * @data:	Storage for TCP payload
 */
struct tcp6_l2_buf_t {
#ifdef __AVX2__
	uint8_t pad[14];	/* 0	align ip6h to 32 bytes */
#else
	uint8_t pad[2];		/*	align ip6h to 4 bytes	0 */
#endif
	struct tap_hdr taph;	/* 14				2 */
	struct ipv6hdr ip6h;	/* 32				20 */
	struct tcphdr th;	/* 72				60 */
	uint8_t data[MSS6];	/* 92				80 */
				/* 65536			65532 */
#ifdef __AVX2__
} __attribute__ ((packed, aligned(32)))
#else
} __attribute__ ((packed, aligned(__alignof__(unsigned int))))
#endif
tcp6_l2_buf[TCP_FRAMES_MEM];

static struct tcp_buf_seq_update tcp6_l2_buf_seq_update[TCP_FRAMES_MEM];

static unsigned int tcp6_l2_buf_used;

/* recvmsg()/sendmsg() data for tap */
static char 		tcp_buf_discard		[MAX_WINDOW];
static struct iovec	iov_sock		[TCP_FRAMES_MEM + 1];

static struct iovec	tcp4_l2_iov		[TCP_FRAMES_MEM];
static struct iovec	tcp6_l2_iov		[TCP_FRAMES_MEM];
static struct iovec	tcp4_l2_flags_iov	[TCP_FRAMES_MEM];
static struct iovec	tcp6_l2_flags_iov	[TCP_FRAMES_MEM];

/* sendmsg() to socket */
static struct iovec	tcp_iov			[UIO_MAXIOV];

/**
 * tcp4_l2_flags_buf_t - IPv4 packet buffers for segments without data (flags)
 * @pad:	Align TCP header to 32 bytes, for AVX2 checksum calculation only
 * @taph:	Tap-level headers (partially pre-filled)
 * @iph:	Pre-filled IP header (except for tot_len and saddr)
 * @th:		Headroom for TCP header
 * @opts:	Headroom for TCP options
 */
static struct tcp4_l2_flags_buf_t {
#ifdef __AVX2__
	uint8_t pad[26];	/* 0, align th to 32 bytes */
#else
	uint8_t pad[2];		/*	align iph to 4 bytes	0 */
#endif
	struct tap_hdr taph;	/* 26				2 */
	struct iphdr iph;	/* 44				20 */
	struct tcphdr th;	/* 64				40 */
	char opts[OPT_MSS_LEN + OPT_WS_LEN + 1];
#ifdef __AVX2__
} __attribute__ ((packed, aligned(32)))
#else
} __attribute__ ((packed, aligned(__alignof__(unsigned int))))
#endif
tcp4_l2_flags_buf[TCP_FRAMES_MEM];

static unsigned int tcp4_l2_flags_buf_used;

/**
 * tcp6_l2_flags_buf_t - IPv6 packet buffers for segments without data (flags)
 * @pad:	Align IPv6 header for checksum calculation to 32B (AVX2) or 4B
 * @taph:	Tap-level headers (partially pre-filled)
 * @ip6h:	Pre-filled IP header (except for payload_len and addresses)
 * @th:		Headroom for TCP header
 * @opts:	Headroom for TCP options
 */
static struct tcp6_l2_flags_buf_t {
#ifdef __AVX2__
	uint8_t pad[14];	/* 0	align ip6h to 32 bytes */
#else
	uint8_t pad[2];		/*	align ip6h to 4 bytes		   0 */
#endif
	struct tap_hdr taph;	/* 14					   2 */
	struct ipv6hdr ip6h;	/* 32					  20 */
	struct tcphdr th	/* 72 */ __attribute__ ((aligned(4))); /* 60 */
	char opts[OPT_MSS_LEN + OPT_WS_LEN + 1];
#ifdef __AVX2__
} __attribute__ ((packed, aligned(32)))
#else
} __attribute__ ((packed, aligned(__alignof__(unsigned int))))
#endif
tcp6_l2_flags_buf[TCP_FRAMES_MEM];

static unsigned int tcp6_l2_flags_buf_used;

#define CONN(idx)		(&(FLOW(idx)->tcp))

/* Table for lookup from remote address, local port, remote port */
static flow_sidx_t tc_hash[TCP_HASH_TABLE_SIZE];

static_assert(ARRAY_SIZE(tc_hash) >= FLOW_MAX,
	"Safe linear probing requires hash table larger than connection table");

/* Pools for pre-opened sockets (in init) */
int init_sock_pool4		[TCP_SOCK_POOL_SIZE];
int init_sock_pool6		[TCP_SOCK_POOL_SIZE];

/**
 * tcp_conn_epoll_events() - epoll events mask for given connection state
 * @events:	Current connection events
 * @conn_flags	Connection flags
 *
 * Return: epoll events mask corresponding to implied connection state
 */
static uint32_t tcp_conn_epoll_events(uint8_t events, uint8_t conn_flags)
{
	if (!events)
		return 0;

	if (events & ESTABLISHED) {
		if (events & TAP_FIN_SENT)
			return EPOLLET;

		if (conn_flags & STALLED)
			return EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;

		return EPOLLIN | EPOLLRDHUP;
	}

	if (events == TAP_SYN_RCVD)
		return EPOLLOUT | EPOLLET | EPOLLRDHUP;

	return EPOLLRDHUP;
}

static void conn_flag_do(const struct ctx *c, struct tcp_tap_conn *conn,
			 unsigned long flag);
#define conn_flag(c, conn, flag)					\
	do {								\
		flow_trace(conn, "flag at %s:%i", __func__, __LINE__);	\
		conn_flag_do(c, conn, flag);				\
	} while (0)

/**
 * tcp_epoll_ctl() - Add/modify/delete epoll state from connection events
 * @c:		Execution context
 * @conn:	Connection pointer
 *
 * Return: 0 on success, negative error code on failure (not on deletion)
 */
static int tcp_epoll_ctl(const struct ctx *c, struct tcp_tap_conn *conn)
{
	int m = conn->in_epoll ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
	union epoll_ref ref = { .type = EPOLL_TYPE_TCP, .fd = conn->sock,
				.flowside = FLOW_SIDX(conn, SOCKSIDE) };
	struct epoll_event ev = { .data.u64 = ref.u64 };

	if (conn->events == CLOSED) {
		if (conn->in_epoll)
			epoll_ctl(c->epollfd, EPOLL_CTL_DEL, conn->sock, &ev);
		if (conn->timer != -1)
			epoll_ctl(c->epollfd, EPOLL_CTL_DEL, conn->timer, &ev);
		return 0;
	}

	ev.events = tcp_conn_epoll_events(conn->events, conn->flags);

	if (epoll_ctl(c->epollfd, m, conn->sock, &ev))
		return -errno;

	conn->in_epoll = true;

	if (conn->timer != -1) {
		union epoll_ref ref_t = { .type = EPOLL_TYPE_TCP_TIMER,
					  .fd = conn->sock,
					  .flow = FLOW_IDX(conn) };
		struct epoll_event ev_t = { .data.u64 = ref_t.u64,
					    .events = EPOLLIN | EPOLLET };

		if (epoll_ctl(c->epollfd, EPOLL_CTL_MOD, conn->timer, &ev_t))
			return -errno;
	}

	return 0;
}

/**
 * tcp_timer_ctl() - Set timerfd based on flags/events, create timerfd if needed
 * @c:		Execution context
 * @conn:	Connection pointer
 *
 * #syscalls timerfd_create timerfd_settime
 */
static void tcp_timer_ctl(const struct ctx *c, struct tcp_tap_conn *conn)
{
	struct itimerspec it = { { 0 }, { 0 } };

	if (conn->events == CLOSED)
		return;

	if (conn->timer == -1) {
		union epoll_ref ref = { .type = EPOLL_TYPE_TCP_TIMER,
					.fd = conn->sock,
					.flow = FLOW_IDX(conn) };
		struct epoll_event ev = { .data.u64 = ref.u64,
					  .events = EPOLLIN | EPOLLET };
		int fd;

		fd = timerfd_create(CLOCK_MONOTONIC, 0);
		if (fd == -1 || fd > FD_REF_MAX) {
			flow_dbg(conn, "failed to get timer: %s",
				 strerror(errno));
			if (fd > -1)
				close(fd);
			conn->timer = -1;
			return;
		}
		conn->timer = fd;

		if (epoll_ctl(c->epollfd, EPOLL_CTL_ADD, conn->timer, &ev)) {
			flow_dbg(conn, "failed to add timer: %s",
				 strerror(errno));
			close(conn->timer);
			conn->timer = -1;
			return;
		}
	}

	if (conn->flags & ACK_TO_TAP_DUE) {
		it.it_value.tv_nsec = (long)ACK_INTERVAL * 1000 * 1000;
	} else if (conn->flags & ACK_FROM_TAP_DUE) {
		if (!(conn->events & ESTABLISHED))
			it.it_value.tv_sec = SYN_TIMEOUT;
		else
			it.it_value.tv_sec = ACK_TIMEOUT;
	} else if (CONN_HAS(conn, SOCK_FIN_SENT | TAP_FIN_ACKED)) {
		it.it_value.tv_sec = FIN_TIMEOUT;
	} else {
		it.it_value.tv_sec = ACT_TIMEOUT;
	}

	flow_dbg(conn, "timer expires in %llu.%03llus",
		 (unsigned long long)it.it_value.tv_sec,
		 (unsigned long long)it.it_value.tv_nsec / 1000 / 1000);

	timerfd_settime(conn->timer, 0, &it, NULL);
}

/**
 * conn_flag_do() - Set/unset given flag, log, update epoll on STALLED flag
 * @c:		Execution context
 * @conn:	Connection pointer
 * @flag:	Flag to set, or ~flag to unset
 */
static void conn_flag_do(const struct ctx *c, struct tcp_tap_conn *conn,
			 unsigned long flag)
{
	if (flag & (flag - 1)) {
		int flag_index = fls(~flag);

		if (!(conn->flags & ~flag))
			return;

		conn->flags &= flag;
		if (flag_index >= 0)
			flow_dbg(conn, "%s dropped", tcp_flag_str[flag_index]);
	} else {
		int flag_index = fls(flag);

		if (conn->flags & flag) {
			/* Special case: setting ACK_FROM_TAP_DUE on a
			 * connection where it's already set is used to
			 * re-schedule the existing timer.
			 * TODO: define clearer semantics for timer-related
			 * flags and factor this into the logic below.
			 */
			if (flag == ACK_FROM_TAP_DUE)
				tcp_timer_ctl(c, conn);

			return;
		}

		conn->flags |= flag;
		if (flag_index >= 0)
			flow_dbg(conn, "%s", tcp_flag_str[flag_index]);
	}

	if (flag == STALLED || flag == ~STALLED)
		tcp_epoll_ctl(c, conn);

	if (flag == ACK_FROM_TAP_DUE || flag == ACK_TO_TAP_DUE		  ||
	    (flag == ~ACK_FROM_TAP_DUE && (conn->flags & ACK_TO_TAP_DUE)) ||
	    (flag == ~ACK_TO_TAP_DUE   && (conn->flags & ACK_FROM_TAP_DUE)))
		tcp_timer_ctl(c, conn);
}

static void tcp_hash_remove(const struct ctx *c,
			    const struct tcp_tap_conn *conn);

/**
 * conn_event_do() - Set and log connection events, update epoll state
 * @c:		Execution context
 * @conn:	Connection pointer
 * @event:	Connection event
 */
static void conn_event_do(const struct ctx *c, struct tcp_tap_conn *conn,
			  unsigned long event)
{
	int prev, new, num = fls(event);

	if (conn->events & event)
		return;

	prev = fls(conn->events);
	if (conn->flags & ACTIVE_CLOSE)
		prev += 5;

	if ((conn->events & ESTABLISHED) && (conn->events != ESTABLISHED))
		prev++;		/* i.e. SOCK_FIN_RCVD, not TAP_SYN_ACK_SENT */

	if (event == CLOSED || (event & CONN_STATE_BITS))
		conn->events = event;
	else
		conn->events |= event;

	new = fls(conn->events);

	if ((conn->events & ESTABLISHED) && (conn->events != ESTABLISHED)) {
		num++;
		new++;
	}
	if (conn->flags & ACTIVE_CLOSE)
		new += 5;

	if (prev != new)
		flow_dbg(conn, "%s: %s -> %s",
			 num == -1 	       ? "CLOSED" : tcp_event_str[num],
			 prev == -1	       ? "CLOSED" : tcp_state_str[prev],
			 (new == -1 || num == -1) ? "CLOSED" : tcp_state_str[new]);
	else
		flow_dbg(conn, "%s",
			 num == -1 	       ? "CLOSED" : tcp_event_str[num]);

	if (event == CLOSED)
		tcp_hash_remove(c, conn);
	else if ((event == TAP_FIN_RCVD) && !(conn->events & SOCK_FIN_RCVD))
		conn_flag(c, conn, ACTIVE_CLOSE);
	else
		tcp_epoll_ctl(c, conn);

	if (CONN_HAS(conn, SOCK_FIN_SENT | TAP_FIN_ACKED))
		tcp_timer_ctl(c, conn);
}

#define conn_event(c, conn, event)					\
	do {								\
		flow_trace(conn, "event at %s:%i", __func__, __LINE__);	\
		conn_event_do(c, conn, event);				\
	} while (0)

/**
 * tcp_rtt_dst_low() - Check if low RTT was seen for connection endpoint
 * @conn:	Connection pointer
 *
 * Return: 1 if destination is in low RTT table, 0 otherwise
 */
static int tcp_rtt_dst_low(const struct tcp_tap_conn *conn)
{
	int i;

	for (i = 0; i < LOW_RTT_TABLE_SIZE; i++)
		if (inany_equals(&conn->faddr, low_rtt_dst + i))
			return 1;

	return 0;
}

/**
 * tcp_rtt_dst_check() - Check tcpi_min_rtt, insert endpoint in table if low
 * @conn:	Connection pointer
 * @tinfo:	Pointer to struct tcp_info for socket
 */
static void tcp_rtt_dst_check(const struct tcp_tap_conn *conn,
			      const struct tcp_info *tinfo)
{
#ifdef HAS_MIN_RTT
	int i, hole = -1;

	if (!tinfo->tcpi_min_rtt ||
	    (int)tinfo->tcpi_min_rtt > LOW_RTT_THRESHOLD)
		return;

	for (i = 0; i < LOW_RTT_TABLE_SIZE; i++) {
		if (inany_equals(&conn->faddr, low_rtt_dst + i))
			return;
		if (hole == -1 && INANY_IS_ADDR_UNSPECIFIED(low_rtt_dst + i))
			hole = i;
	}

	/* Keep gcc 12 happy: this won't actually happen because the table is
	 * guaranteed to have a hole, see the second memcpy() below.
	 */
	if (hole == -1)
		return;

	low_rtt_dst[hole++] = conn->faddr;
	if (hole == LOW_RTT_TABLE_SIZE)
		hole = 0;
	inany_from_af(low_rtt_dst + hole, AF_INET6, &in6addr_any);
#else
	(void)conn;
	(void)tinfo;
#endif /* HAS_MIN_RTT */
}

/**
 * tcp_get_sndbuf() - Get, scale SO_SNDBUF between thresholds (1 to 0.5 usage)
 * @conn:	Connection pointer
 */
static void tcp_get_sndbuf(struct tcp_tap_conn *conn)
{
	int s = conn->sock, sndbuf;
	socklen_t sl;
	uint64_t v;

	sl = sizeof(sndbuf);
	if (getsockopt(s, SOL_SOCKET, SO_SNDBUF, &sndbuf, &sl)) {
		SNDBUF_SET(conn, WINDOW_DEFAULT);
		return;
	}

	v = sndbuf;
	if (v >= SNDBUF_BIG)
		v /= 2;
	else if (v > SNDBUF_SMALL)
		v -= v * (v - SNDBUF_SMALL) / (SNDBUF_BIG - SNDBUF_SMALL) / 2;

	SNDBUF_SET(conn, MIN(INT_MAX, v));
}

/**
 * tcp_sock_set_bufsize() - Set SO_RCVBUF and SO_SNDBUF to maximum values
 * @s:		Socket, can be -1 to avoid check in the caller
 */
static void tcp_sock_set_bufsize(const struct ctx *c, int s)
{
	int v = INT_MAX / 2; /* Kernel clamps and rounds, no need to check */

	if (s == -1)
		return;

	if (!c->low_rmem && setsockopt(s, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v)))
		trace("TCP: failed to set SO_RCVBUF to %i", v);

	if (!c->low_wmem && setsockopt(s, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v)))
		trace("TCP: failed to set SO_SNDBUF to %i", v);
}

/**
 * tcp_update_check_tcp4() - Update TCP checksum from stored one
 * @iph:	IPv4 header
 * @th:		TCP header followed by TCP payload
 */
static void tcp_update_check_tcp4(const struct iphdr *iph, struct tcphdr *th)
{
	uint16_t tlen = ntohs(iph->tot_len) - sizeof(struct iphdr);
	struct in_addr saddr = { .s_addr = iph->saddr };
	struct in_addr daddr = { .s_addr = iph->daddr };
	uint32_t sum = proto_ipv4_header_psum(tlen, IPPROTO_TCP, saddr, daddr);

	th->check = 0;
	th->check = csum(th, tlen, sum);
}

/**
 * tcp_update_check_tcp6() - Calculate TCP checksum for IPv6
 * @ip6h:	IPv6 header
 * @th:		TCP header followed by TCP payload
 */
static void tcp_update_check_tcp6(struct ipv6hdr *ip6h, struct tcphdr *th)
{
	uint16_t payload_len = ntohs(ip6h->payload_len);
	uint32_t sum = proto_ipv6_header_psum(payload_len, IPPROTO_TCP,
					      &ip6h->saddr, &ip6h->daddr);

	th->check = 0;
	th->check = csum(th, payload_len, sum);
}

/**
 * tcp_update_l2_buf() - Update L2 buffers with Ethernet and IPv4 addresses
 * @eth_d:	Ethernet destination address, NULL if unchanged
 * @eth_s:	Ethernet source address, NULL if unchanged
 */
void tcp_update_l2_buf(const unsigned char *eth_d, const unsigned char *eth_s)
{
	int i;

	for (i = 0; i < TCP_FRAMES_MEM; i++) {
		struct tcp4_l2_flags_buf_t *b4f = &tcp4_l2_flags_buf[i];
		struct tcp6_l2_flags_buf_t *b6f = &tcp6_l2_flags_buf[i];
		struct tcp4_l2_buf_t *b4 = &tcp4_l2_buf[i];
		struct tcp6_l2_buf_t *b6 = &tcp6_l2_buf[i];

		eth_update_mac(&b4->taph.eh, eth_d, eth_s);
		eth_update_mac(&b6->taph.eh, eth_d, eth_s);
		eth_update_mac(&b4f->taph.eh, eth_d, eth_s);
		eth_update_mac(&b6f->taph.eh, eth_d, eth_s);
	}
}

/**
 * tcp_sock4_iov_init() - Initialise scatter-gather L2 buffers for IPv4 sockets
 * @c:		Execution context
 */
static void tcp_sock4_iov_init(const struct ctx *c)
{
	struct iphdr iph = L2_BUF_IP4_INIT(IPPROTO_TCP);
	struct iovec *iov;
	int i;

	for (i = 0; i < ARRAY_SIZE(tcp4_l2_buf); i++) {
		tcp4_l2_buf[i] = (struct tcp4_l2_buf_t) {
			.taph = TAP_HDR_INIT(ETH_P_IP),
			.iph = iph,
			.th = { .doff = sizeof(struct tcphdr) / 4, .ack = 1 }
		};
	}

	for (i = 0; i < ARRAY_SIZE(tcp4_l2_flags_buf); i++) {
		tcp4_l2_flags_buf[i] = (struct tcp4_l2_flags_buf_t) {
			.taph = TAP_HDR_INIT(ETH_P_IP),
			.iph = L2_BUF_IP4_INIT(IPPROTO_TCP)
		};
	}

	for (i = 0, iov = tcp4_l2_iov; i < TCP_FRAMES_MEM; i++, iov++)
		iov->iov_base = tap_frame_base(c, &tcp4_l2_buf[i].taph);

	for (i = 0, iov = tcp4_l2_flags_iov; i < TCP_FRAMES_MEM; i++, iov++)
		iov->iov_base = tap_frame_base(c, &tcp4_l2_flags_buf[i].taph);
}

/**
 * tcp_sock6_iov_init() - Initialise scatter-gather L2 buffers for IPv6 sockets
 * @c:		Execution context
 */
static void tcp_sock6_iov_init(const struct ctx *c)
{
	struct iovec *iov;
	int i;

	for (i = 0; i < ARRAY_SIZE(tcp6_l2_buf); i++) {
		tcp6_l2_buf[i] = (struct tcp6_l2_buf_t) {
			.taph = TAP_HDR_INIT(ETH_P_IPV6),
			.ip6h = L2_BUF_IP6_INIT(IPPROTO_TCP),
			.th = { .doff = sizeof(struct tcphdr) / 4, .ack = 1 }
		};
	}

	for (i = 0; i < ARRAY_SIZE(tcp6_l2_flags_buf); i++) {
		tcp6_l2_flags_buf[i] = (struct tcp6_l2_flags_buf_t) {
			.taph = TAP_HDR_INIT(ETH_P_IPV6),
			.ip6h = L2_BUF_IP6_INIT(IPPROTO_TCP)
		};
	}

	for (i = 0, iov = tcp6_l2_iov; i < TCP_FRAMES_MEM; i++, iov++)
		iov->iov_base = tap_frame_base(c, &tcp6_l2_buf[i].taph);

	for (i = 0, iov = tcp6_l2_flags_iov; i < TCP_FRAMES_MEM; i++, iov++)
		iov->iov_base = tap_frame_base(c, &tcp6_l2_flags_buf[i].taph);
}

/**
 * tcp_opt_get() - Get option, and value if any, from TCP header
 * @opts:	Pointer to start of TCP options in header
 * @len:	Length of buffer, excluding TCP header -- NOT checked here!
 * @type_find:	Option type to look for
 * @optlen_set:	Optional, filled with option length if passed
 * @value_set:	Optional, set to start of option value if passed
 *
 * Return: option value, meaningful for up to 4 bytes, -1 if not found
 */
static int tcp_opt_get(const char *opts, size_t len, uint8_t type_find,
		       uint8_t *optlen_set, const char **value_set)
{
	uint8_t type, optlen;

	if (!opts || !len)
		return -1;

	for (; len >= 2; opts += optlen, len -= optlen) {
		switch (*opts) {
		case OPT_EOL:
			return -1;
		case OPT_NOP:
			optlen = 1;
			break;
		default:
			type = *(opts++);

			if (*(uint8_t *)opts < 2 || *(uint8_t *)opts > len)
				return -1;

			optlen = *(opts++) - 2;
			len -= 2;

			if (type != type_find)
				break;

			if (optlen_set)
				*optlen_set = optlen;
			if (value_set)
				*value_set = opts;

			switch (optlen) {
			case 0:
				return 0;
			case 1:
				return *opts;
			case 2:
				return ntohs(*(uint16_t *)opts);
			default:
				return ntohl(*(uint32_t *)opts);
			}
		}
	}

	return -1;
}

/**
 * tcp_hash_match() - Check if a connection entry matches address and ports
 * @conn:	Connection entry to match against
 * @faddr:	Guest side forwarding address
 * @eport:	Guest side endpoint port
 * @fport:	Guest side forwarding port
 *
 * Return: 1 on match, 0 otherwise
 */
static int tcp_hash_match(const struct tcp_tap_conn *conn,
			  const union inany_addr *faddr,
			  in_port_t eport, in_port_t fport)
{
	if (inany_equals(&conn->faddr, faddr) &&
	    conn->eport == eport && conn->fport == fport)
		return 1;

	return 0;
}

/**
 * tcp_hash() - Calculate hash value for connection given address and ports
 * @c:		Execution context
 * @faddr:	Guest side forwarding address
 * @eport:	Guest side endpoint port
 * @fport:	Guest side forwarding port
 *
 * Return: hash value, needs to be adjusted for table size
 */
static uint64_t tcp_hash(const struct ctx *c, const union inany_addr *faddr,
			 in_port_t eport, in_port_t fport)
{
	struct siphash_state state = SIPHASH_INIT(c->hash_secret);

	inany_siphash_feed(&state, faddr);
	return siphash_final(&state, 20, (uint64_t)eport << 16 | fport);
}

/**
 * tcp_conn_hash() - Calculate hash bucket of an existing connection
 * @c:		Execution context
 * @conn:	Connection
 *
 * Return: hash value, needs to be adjusted for table size
 */
static uint64_t tcp_conn_hash(const struct ctx *c,
			      const struct tcp_tap_conn *conn)
{
	return tcp_hash(c, &conn->faddr, conn->eport, conn->fport);
}

/**
 * tcp_hash_probe() - Find hash bucket for a connection
 * @c:		Execution context
 * @conn:	Connection to find bucket for
 *
 * Return: If @conn is in the table, its current bucket, otherwise a suitable
 *         free bucket for it.
 */
static inline unsigned tcp_hash_probe(const struct ctx *c,
				      const struct tcp_tap_conn *conn)
{
	flow_sidx_t sidx = FLOW_SIDX(conn, TAPSIDE);
	unsigned b = tcp_conn_hash(c, conn) % TCP_HASH_TABLE_SIZE;

	/* Linear probing */
	while (!flow_sidx_eq(tc_hash[b], FLOW_SIDX_NONE) &&
	       !flow_sidx_eq(tc_hash[b], sidx))
		b = mod_sub(b, 1, TCP_HASH_TABLE_SIZE);

	return b;
}

/**
 * tcp_hash_insert() - Insert connection into hash table, chain link
 * @c:		Execution context
 * @conn:	Connection pointer
 */
static void tcp_hash_insert(const struct ctx *c, struct tcp_tap_conn *conn)
{
	unsigned b = tcp_hash_probe(c, conn);

	tc_hash[b] = FLOW_SIDX(conn, TAPSIDE);
	flow_dbg(conn, "hash table insert: sock %i, bucket: %u", conn->sock, b);
}

/**
 * tcp_hash_remove() - Drop connection from hash table, chain unlink
 * @c:		Execution context
 * @conn:	Connection pointer
 */
static void tcp_hash_remove(const struct ctx *c,
			    const struct tcp_tap_conn *conn)
{
	unsigned b = tcp_hash_probe(c, conn), s;
	union flow *flow = flow_at_sidx(tc_hash[b]);

	if (!flow)
		return; /* Redundant remove */

	flow_dbg(conn, "hash table remove: sock %i, bucket: %u", conn->sock, b);

	/* Scan the remainder of the cluster */
	for (s = mod_sub(b, 1, TCP_HASH_TABLE_SIZE);
	     (flow = flow_at_sidx(tc_hash[s]));
	     s = mod_sub(s, 1, TCP_HASH_TABLE_SIZE)) {
		unsigned h = tcp_conn_hash(c, &flow->tcp) % TCP_HASH_TABLE_SIZE;

		if (!mod_between(h, s, b, TCP_HASH_TABLE_SIZE)) {
			/* tc_hash[s] can live in tc_hash[b]'s slot */
			debug("hash table remove: shuffle %u -> %u", s, b);
			tc_hash[b] = tc_hash[s];
			b = s;
		}
	}

	tc_hash[b] = FLOW_SIDX_NONE;
}

/**
 * tcp_hash_lookup() - Look up connection given remote address and ports
 * @c:		Execution context
 * @af:		Address family, AF_INET or AF_INET6
 * @faddr:	Guest side forwarding address (guest remote address)
 * @eport:	Guest side endpoint port (guest local port)
 * @fport:	Guest side forwarding port (guest remote port)
 *
 * Return: connection pointer, if found, -ENOENT otherwise
 */
static struct tcp_tap_conn *tcp_hash_lookup(const struct ctx *c,
					    sa_family_t af, const void *faddr,
					    in_port_t eport, in_port_t fport)
{
	union inany_addr aany;
	union flow *flow;
	unsigned b;

	inany_from_af(&aany, af, faddr);

	b = tcp_hash(c, &aany, eport, fport) % TCP_HASH_TABLE_SIZE;
	while ((flow = flow_at_sidx(tc_hash[b])) &&
	       !tcp_hash_match(&flow->tcp, &aany, eport, fport))
		b = mod_sub(b, 1, TCP_HASH_TABLE_SIZE);

	return &flow->tcp;
}

/**
 * tcp_flow_defer() - Deferred per-flow handling (clean up closed connections)
 * @flow:	Flow table entry for this connection
 *
 * Return: true if the flow is ready to free, false otherwise
 */
bool tcp_flow_defer(union flow *flow)
{
	const struct tcp_tap_conn *conn = &flow->tcp;

	if (flow->tcp.events != CLOSED)
		return false;

	close(conn->sock);
	if (conn->timer != -1)
		close(conn->timer);

	return true;
}

static void tcp_rst_do(struct ctx *c, struct tcp_tap_conn *conn);
#define tcp_rst(c, conn)						\
	do {								\
		flow_dbg((conn), "TCP reset at %s:%i", __func__, __LINE__); \
		tcp_rst_do(c, conn);					\
	} while (0)

/**
 * tcp_l2_flags_buf_flush() - Send out buffers for segments with no data (flags)
 * @c:		Execution context
 */
static void tcp_l2_flags_buf_flush(const struct ctx *c)
{
	tap_send_frames(c, tcp6_l2_flags_iov, 1, tcp6_l2_flags_buf_used);
	tcp6_l2_flags_buf_used = 0;

	tap_send_frames(c, tcp4_l2_flags_iov, 1, tcp4_l2_flags_buf_used);
	tcp4_l2_flags_buf_used = 0;
}

/**
 * tcp_l2_data_buf_flush() - Send out buffers for segments with data
 * @c:		Execution context
 */
static void tcp_l2_data_buf_flush(const struct ctx *c)
{
	unsigned i;
	size_t m;

	m = tap_send_frames(c, tcp6_l2_iov, 1, tcp6_l2_buf_used);
	for (i = 0; i < m; i++)
		*tcp6_l2_buf_seq_update[i].seq += tcp6_l2_buf_seq_update[i].len;
	tcp6_l2_buf_used = 0;

	m = tap_send_frames(c, tcp4_l2_iov, 1, tcp4_l2_buf_used);
	for (i = 0; i < m; i++)
		*tcp4_l2_buf_seq_update[i].seq += tcp4_l2_buf_seq_update[i].len;
	tcp4_l2_buf_used = 0;
}

/**
 * tcp_defer_handler() - Handler for TCP deferred tasks
 * @c:		Execution context
 */
/* cppcheck-suppress [constParameterPointer, unmatchedSuppression] */
void tcp_defer_handler(struct ctx *c)
{
	tcp_l2_flags_buf_flush(c);
	tcp_l2_data_buf_flush(c);
}

/**
 * tcp_fill_header() - Fill the TCP header fields for a given TCP segment.
 *
 * @th:		Pointer to the TCP header structure
 * @conn:	Pointer to the TCP connection structure
 * @seq:	Sequence number
 */
static void tcp_fill_header(struct tcphdr *th,
			       const struct tcp_tap_conn *conn, uint32_t seq)
{
	th->source = htons(conn->fport);
	th->dest = htons(conn->eport);
	th->seq = htonl(seq);
	th->ack_seq = htonl(conn->seq_ack_to_tap);
	if (conn->events & ESTABLISHED)	{
		th->window = htons(conn->wnd_to_tap);
	} else {
		unsigned wnd = conn->wnd_to_tap << conn->ws_to_tap;

		th->window = htons(MIN(wnd, USHRT_MAX));
	}
}

/**
 * tcp_fill_headers4() - Fill 802.3, IPv4, TCP headers in pre-cooked buffers
 * @c:		Execution context
 * @conn:	Connection pointer
 * @iph:	Pointer to IPv4 header
 * @th:		Pointer to TCP header
 * @plen:	Payload length (including TCP header options)
 * @check:	Checksum, if already known
 * @seq:	Sequence number for this segment
 *
 * Return: The total length of the IPv4 packet, host order
 */
static size_t tcp_fill_headers4(const struct ctx *c,
				const struct tcp_tap_conn *conn,
				struct iphdr *iph, struct tcphdr *th,
				size_t plen, const uint16_t *check,
				uint32_t seq)
{
	size_t ip_len = plen + sizeof(struct iphdr) + sizeof(struct tcphdr);
	const struct in_addr *a4 = inany_v4(&conn->faddr);

	ASSERT(a4);

	iph->tot_len = htons(ip_len);
	iph->saddr = a4->s_addr;
	iph->daddr = c->ip4.addr_seen.s_addr;

	iph->check = check ? *check :
			     csum_ip4_header(iph->tot_len, IPPROTO_TCP,
					     *a4, c->ip4.addr_seen);

	tcp_fill_header(th, conn, seq);

	tcp_update_check_tcp4(iph, th);

	return ip_len;
}

/**
 * tcp_fill_headers6() - Fill 802.3, IPv6, TCP headers in pre-cooked buffers
 * @c:		Execution context
 * @conn:	Connection pointer
 * @ip6h:	Pointer to IPv6 header
 * @th:		Pointer to TCP header
 * @plen:	Payload length (including TCP header options)
 * @check:	Checksum, if already known
 * @seq:	Sequence number for this segment
 *
 * Return: The total length of the IPv6 packet, host order
 */
static size_t tcp_fill_headers6(const struct ctx *c,
				const struct tcp_tap_conn *conn,
				struct ipv6hdr *ip6h, struct tcphdr *th,
				size_t plen, uint32_t seq)
{
	size_t ip_len = plen + sizeof(struct ipv6hdr) + sizeof(struct tcphdr);

	ip6h->payload_len = htons(plen + sizeof(struct tcphdr));
	ip6h->saddr = conn->faddr.a6;
	if (IN6_IS_ADDR_LINKLOCAL(&ip6h->saddr))
		ip6h->daddr = c->ip6.addr_ll_seen;
	else
		ip6h->daddr = c->ip6.addr_seen;

	ip6h->hop_limit = 255;
	ip6h->version = 6;
	ip6h->nexthdr = IPPROTO_TCP;

	ip6h->flow_lbl[0] = (conn->sock >> 16) & 0xf;
	ip6h->flow_lbl[1] = (conn->sock >> 8) & 0xff;
	ip6h->flow_lbl[2] = (conn->sock >> 0) & 0xff;

	tcp_fill_header(th, conn, seq);

	tcp_update_check_tcp6(ip6h, th);

	return ip_len;
}

/**
 * tcp_l2_buf_fill_headers() - Fill 802.3, IP, TCP headers in pre-cooked buffers
 * @c:		Execution context
 * @conn:	Connection pointer
 * @p:		Pointer to any type of TCP pre-cooked buffer
 * @plen:	Payload length (including TCP header options)
 * @check:	Checksum, if already known
 * @seq:	Sequence number for this segment
 *
 * Return: frame length including L2 headers, host order
 */
static size_t tcp_l2_buf_fill_headers(const struct ctx *c,
				      const struct tcp_tap_conn *conn,
				      void *p, size_t plen,
				      const uint16_t *check, uint32_t seq)
{
	const struct in_addr *a4 = inany_v4(&conn->faddr);
	size_t ip_len, tlen;

	if (a4) {
		struct tcp4_l2_buf_t *b = (struct tcp4_l2_buf_t *)p;

		ip_len = tcp_fill_headers4(c, conn, &b->iph, &b->th, plen,
					   check, seq);

		tlen = tap_frame_len(c, &b->taph, ip_len);
	} else {
		struct tcp6_l2_buf_t *b = (struct tcp6_l2_buf_t *)p;

		ip_len = tcp_fill_headers6(c, conn, &b->ip6h, &b->th, plen,
					   seq);

		tlen = tap_frame_len(c, &b->taph, ip_len);
	}

	return tlen;
}

/**
 * tcp_update_seqack_wnd() - Update ACK sequence and window to guest/tap
 * @c:		Execution context
 * @conn:	Connection pointer
 * @force_seq:	Force ACK sequence to latest segment, instead of checking socket
 * @tinfo:	tcp_info from kernel, can be NULL if not pre-fetched
 *
 * Return: 1 if sequence or window were updated, 0 otherwise
 */
static int tcp_update_seqack_wnd(const struct ctx *c, struct tcp_tap_conn *conn,
				 int force_seq, struct tcp_info *tinfo)
{
	uint32_t prev_wnd_to_tap = conn->wnd_to_tap << conn->ws_to_tap;
	uint32_t prev_ack_to_tap = conn->seq_ack_to_tap;
	/* cppcheck-suppress [ctunullpointer, unmatchedSuppression] */
	socklen_t sl = sizeof(*tinfo);
	struct tcp_info tinfo_new;
	uint32_t new_wnd_to_tap = prev_wnd_to_tap;
	int s = conn->sock;

#ifndef HAS_BYTES_ACKED
	(void)force_seq;

	conn->seq_ack_to_tap = conn->seq_from_tap;
	if (SEQ_LT(conn->seq_ack_to_tap, prev_ack_to_tap))
		conn->seq_ack_to_tap = prev_ack_to_tap;
#else
	if ((unsigned)SNDBUF_GET(conn) < SNDBUF_SMALL || tcp_rtt_dst_low(conn)
	    || CONN_IS_CLOSING(conn) || (conn->flags & LOCAL) || force_seq) {
		conn->seq_ack_to_tap = conn->seq_from_tap;
	} else if (conn->seq_ack_to_tap != conn->seq_from_tap) {
		if (!tinfo) {
			tinfo = &tinfo_new;
			if (getsockopt(s, SOL_TCP, TCP_INFO, tinfo, &sl))
				return 0;
		}

		conn->seq_ack_to_tap = tinfo->tcpi_bytes_acked +
				       conn->seq_init_from_tap;

		if (SEQ_LT(conn->seq_ack_to_tap, prev_ack_to_tap))
			conn->seq_ack_to_tap = prev_ack_to_tap;
	}
#endif /* !HAS_BYTES_ACKED */

	if (!KERNEL_REPORTS_SND_WND(c)) {
		tcp_get_sndbuf(conn);
		new_wnd_to_tap = MIN(SNDBUF_GET(conn), MAX_WINDOW);
		conn->wnd_to_tap = MIN(new_wnd_to_tap >> conn->ws_to_tap,
				       USHRT_MAX);
		goto out;
	}

	if (!tinfo) {
		if (prev_wnd_to_tap > WINDOW_DEFAULT) {
			goto out;
}
		tinfo = &tinfo_new;
		if (getsockopt(s, SOL_TCP, TCP_INFO, tinfo, &sl)) {
			goto out;
}
	}

#ifdef HAS_SND_WND
	if ((conn->flags & LOCAL) || tcp_rtt_dst_low(conn)) {
		new_wnd_to_tap = tinfo->tcpi_snd_wnd;
	} else {
		tcp_get_sndbuf(conn);
		new_wnd_to_tap = MIN((int)tinfo->tcpi_snd_wnd,
				     SNDBUF_GET(conn));
	}
#endif

	new_wnd_to_tap = MIN(new_wnd_to_tap, MAX_WINDOW);
	if (!(conn->events & ESTABLISHED))
		new_wnd_to_tap = MAX(new_wnd_to_tap, WINDOW_DEFAULT);

	conn->wnd_to_tap = MIN(new_wnd_to_tap >> conn->ws_to_tap, USHRT_MAX);

	/* Certain cppcheck versions, e.g. 2.12.0 have a bug where they think
	 * the MIN() above restricts conn->wnd_to_tap to be zero.  That's
	 * clearly incorrect, but until the bug is fixed, work around it.
	 *   https://bugzilla.redhat.com/show_bug.cgi?id=2240705
	 *   https://sourceforge.net/p/cppcheck/discussion/general/thread/f5b1a00646/
	 */
	/* cppcheck-suppress [knownConditionTrueFalse, unmatchedSuppression] */
	if (!conn->wnd_to_tap)
		conn_flag(c, conn, ACK_TO_TAP_DUE);

out:
	return new_wnd_to_tap       != prev_wnd_to_tap ||
	       conn->seq_ack_to_tap != prev_ack_to_tap;
}

/**
 * tcp_update_seqack_from_tap() - ACK number from tap and related flags/counters
 * @c:		Execution context
 * @conn:	Connection pointer
 * @seq		Current ACK sequence, host order
 */
static void tcp_update_seqack_from_tap(const struct ctx *c,
				       struct tcp_tap_conn *conn, uint32_t seq)
{
	if (seq == conn->seq_to_tap)
		conn_flag(c, conn, ~ACK_FROM_TAP_DUE);

	if (SEQ_GT(seq, conn->seq_ack_from_tap)) {
		/* Forward progress, but more data to acknowledge: reschedule */
		if (SEQ_LT(seq, conn->seq_to_tap))
			conn_flag(c, conn, ACK_FROM_TAP_DUE);

		conn->retrans = 0;
		conn->seq_ack_from_tap = seq;
	}
}

/**
 * tcp_send_flag() - Send segment with flags to tap (no payload)
 * @c:		Execution context
 * @conn:	Connection pointer
 * @flags:	TCP flags: if not set, send segment only if ACK is due
 *
 * Return: negative error code on connection reset, 0 otherwise
 */
static int tcp_send_flag(struct ctx *c, struct tcp_tap_conn *conn, int flags)
{
	struct tcp4_l2_flags_buf_t *b4 = NULL;
	struct tcp6_l2_flags_buf_t *b6 = NULL;
	struct tcp_info tinfo = { 0 };
	socklen_t sl = sizeof(tinfo);
	int s = conn->sock;
	size_t optlen = 0;
	struct iovec *iov;
	struct tcphdr *th;
	char *data;
	void *p;

	if (SEQ_GE(conn->seq_ack_to_tap, conn->seq_from_tap) &&
	    !flags && conn->wnd_to_tap)
		return 0;

	if (getsockopt(s, SOL_TCP, TCP_INFO, &tinfo, &sl)) {
		conn_event(c, conn, CLOSED);
		return -ECONNRESET;
	}

#ifdef HAS_SND_WND
	if (!c->tcp.kernel_snd_wnd && tinfo.tcpi_snd_wnd)
		c->tcp.kernel_snd_wnd = 1;
#endif

	if (!(conn->flags & LOCAL))
		tcp_rtt_dst_check(conn, &tinfo);

	if (!tcp_update_seqack_wnd(c, conn, flags, &tinfo) && !flags)
		return 0;

	if (CONN_V4(conn)) {
		iov = tcp4_l2_flags_iov    + tcp4_l2_flags_buf_used;
		p = b4 = tcp4_l2_flags_buf + tcp4_l2_flags_buf_used++;
		th = &b4->th;

		/* gcc 11.2 would complain on data = (char *)(th + 1); */
		data = b4->opts;
	} else {
		iov = tcp6_l2_flags_iov    + tcp6_l2_flags_buf_used;
		p = b6 = tcp6_l2_flags_buf + tcp6_l2_flags_buf_used++;
		th = &b6->th;
		data = b6->opts;
	}

	if (flags & SYN) {
		int mss;

		/* Options: MSS, NOP and window scale (8 bytes) */
		optlen = OPT_MSS_LEN + 1 + OPT_WS_LEN;

		*data++ = OPT_MSS;
		*data++ = OPT_MSS_LEN;

		if (c->mtu == -1) {
			mss = tinfo.tcpi_snd_mss;
		} else {
			mss = c->mtu - sizeof(struct tcphdr);
			if (CONN_V4(conn))
				mss -= sizeof(struct iphdr);
			else
				mss -= sizeof(struct ipv6hdr);

			if (c->low_wmem &&
			    !(conn->flags & LOCAL) && !tcp_rtt_dst_low(conn))
				mss = MIN(mss, PAGE_SIZE);
			else if (mss > PAGE_SIZE)
				mss = ROUND_DOWN(mss, PAGE_SIZE);
		}
		*(uint16_t *)data = htons(MIN(USHRT_MAX, mss));

		data += OPT_MSS_LEN - 2;

		conn->ws_to_tap = MIN(MAX_WS, tinfo.tcpi_snd_wscale);

		*data++ = OPT_NOP;
		*data++ = OPT_WS;
		*data++ = OPT_WS_LEN;
		*data++ = conn->ws_to_tap;
	} else if (!(flags & RST)) {
		flags |= ACK;
	}

	th->doff = (sizeof(*th) + optlen) / 4;

	th->ack = !!(flags & ACK);
	th->rst = !!(flags & RST);
	th->syn = !!(flags & SYN);
	th->fin = !!(flags & FIN);

	iov->iov_len = tcp_l2_buf_fill_headers(c, conn, p, optlen,
					       NULL, conn->seq_to_tap);

	if (th->ack) {
		if (SEQ_GE(conn->seq_ack_to_tap, conn->seq_from_tap))
			conn_flag(c, conn, ~ACK_TO_TAP_DUE);
		else
			conn_flag(c, conn, ACK_TO_TAP_DUE);
	}

	if (th->fin)
		conn_flag(c, conn, ACK_FROM_TAP_DUE);

	/* RFC 793, 3.1: "[...] and the first data octet is ISN+1." */
	if (th->fin || th->syn)
		conn->seq_to_tap++;

	if (CONN_V4(conn)) {
		if (flags & DUP_ACK) {
			memcpy(b4 + 1, b4, sizeof(*b4));
			(iov + 1)->iov_len = iov->iov_len;
			tcp4_l2_flags_buf_used++;
		}

		if (tcp4_l2_flags_buf_used > ARRAY_SIZE(tcp4_l2_flags_buf) - 2)
			tcp_l2_flags_buf_flush(c);
	} else {
		if (flags & DUP_ACK) {
			memcpy(b6 + 1, b6, sizeof(*b6));
			(iov + 1)->iov_len = iov->iov_len;
			tcp6_l2_flags_buf_used++;
		}

		if (tcp6_l2_flags_buf_used > ARRAY_SIZE(tcp6_l2_flags_buf) - 2)
			tcp_l2_flags_buf_flush(c);
	}

	return 0;
}

/**
 * tcp_rst_do() - Reset a tap connection: send RST segment to tap, close socket
 * @c:		Execution context
 * @conn:	Connection pointer
 */
static void tcp_rst_do(struct ctx *c, struct tcp_tap_conn *conn)
{
	if (conn->events == CLOSED)
		return;

	if (!tcp_send_flag(c, conn, RST))
		conn_event(c, conn, CLOSED);
}

/**
 * tcp_get_tap_ws() - Get Window Scaling option for connection from tap/guest
 * @conn:	Connection pointer
 * @opts:	Pointer to start of TCP options
 * @optlen:	Bytes in options: caller MUST ensure available length
 */
static void tcp_get_tap_ws(struct tcp_tap_conn *conn,
			   const char *opts, size_t optlen)
{
	int ws = tcp_opt_get(opts, optlen, OPT_WS, NULL, NULL);

	if (ws >= 0 && ws <= TCP_WS_MAX)
		conn->ws_from_tap = ws;
	else
		conn->ws_from_tap = 0;
}

/**
 * tcp_tap_window_update() - Process an updated window from tap side
 * @conn:	Connection pointer
 * @window:	Window value, host order, unscaled
 */
static void tcp_tap_window_update(struct tcp_tap_conn *conn, unsigned wnd)
{
	wnd = MIN(MAX_WINDOW, wnd << conn->ws_from_tap);
	conn->wnd_from_tap = MIN(wnd >> conn->ws_from_tap, USHRT_MAX);

	/* FIXME: reflect the tap-side receiver's window back to the sock-side
	 * sender by adjusting SO_RCVBUF? */
}

/**
 * tcp_seq_init() - Calculate initial sequence number according to RFC 6528
 * @c:		Execution context
 * @conn:	TCP connection, with faddr, fport and eport populated
 * @now:	Current timestamp
 */
static void tcp_seq_init(const struct ctx *c, struct tcp_tap_conn *conn,
			 const struct timespec *now)
{
	struct siphash_state state = SIPHASH_INIT(c->hash_secret);
	union inany_addr aany;
	uint64_t hash;
	uint32_t ns;

	if (CONN_V4(conn))
		inany_from_af(&aany, AF_INET, &c->ip4.addr);
	else
		inany_from_af(&aany, AF_INET6, &c->ip6.addr);

	inany_siphash_feed(&state, &conn->faddr);
	inany_siphash_feed(&state, &aany);
	hash = siphash_final(&state, 36,
			     (uint64_t)conn->fport << 16 | conn->eport);

	/* 32ns ticks, overflows 32 bits every 137s */
	ns = (now->tv_sec * 1000000000 + now->tv_nsec) >> 5;

	conn->seq_to_tap = ((uint32_t)(hash >> 32) ^ (uint32_t)hash) + ns;
}

/**
 * tcp_conn_pool_sock() - Get socket for new connection from pre-opened pool
 * @pool:	Pool of pre-opened sockets
 *
 * Return: socket number if available, negative code if pool is empty
 */
int tcp_conn_pool_sock(int pool[])
{
	int s = -1, i;

	for (i = 0; i < TCP_SOCK_POOL_SIZE; i++) {
		SWAP(s, pool[i]);
		if (s >= 0)
			return s;
	}
	return -1;
}

/**
 * tcp_conn_new_sock() - Open and prepare new socket for connection
 * @c:		Execution context
 * @af:		Address family
 *
 * Return: socket number on success, negative code if socket creation failed
 */
static int tcp_conn_new_sock(const struct ctx *c, sa_family_t af)
{
	int s;

	s = socket(af, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);

	if (s > FD_REF_MAX) {
		close(s);
		return -EIO;
	}

	if (s < 0)
		return -errno;

	tcp_sock_set_bufsize(c, s);

	return s;
}

/**
 * tcp_conn_sock() - Obtain a connectable socket in the host/init namespace
 * @c:		Execution context
 * @af:		Address family (AF_INET or AF_INET6)
 *
 * Return: Socket fd on success, -errno on failure
 */
int tcp_conn_sock(const struct ctx *c, sa_family_t af)
{
	int *pool = af == AF_INET6 ? init_sock_pool6 : init_sock_pool4;
	int s;

	if ((s = tcp_conn_pool_sock(pool)) >= 0)
		return s;

	/* If the pool is empty we just open a new one without refilling the
	 * pool to keep latency down.
	 */
	if ((s = tcp_conn_new_sock(c, af)) >= 0)
		return s;

	err("TCP: Unable to open socket for new connection: %s",
	    strerror(-s));
	return -1;
}

/**
 * tcp_conn_tap_mss() - Get MSS value advertised by tap/guest
 * @conn:	Connection pointer
 * @opts:	Pointer to start of TCP options
 * @optlen:	Bytes in options: caller MUST ensure available length
 *
 * Return: clamped MSS value
 */
static uint16_t tcp_conn_tap_mss(const struct tcp_tap_conn *conn,
				 const char *opts, size_t optlen)
{
	unsigned int mss;
	int ret;

	if ((ret = tcp_opt_get(opts, optlen, OPT_MSS, NULL, NULL)) < 0)
		mss = MSS_DEFAULT;
	else
		mss = ret;

	if (CONN_V4(conn))
		mss = MIN(MSS4, mss);
	else
		mss = MIN(MSS6, mss);

	return MIN(mss, USHRT_MAX);
}

/**
 * tcp_bind_outbound() - Bind socket to outbound address and interface if given
 * @c:		Execution context
 * @s:		Outbound TCP socket
 * @af:		Address family
 */
static void tcp_bind_outbound(const struct ctx *c, int s, sa_family_t af)
{
	if (af == AF_INET) {
		if (!IN4_IS_ADDR_UNSPECIFIED(&c->ip4.addr_out)) {
			struct sockaddr_in addr4 = {
				.sin_family = AF_INET,
				.sin_port = 0,
				.sin_addr = c->ip4.addr_out,
			};

			if (bind(s, (struct sockaddr *)&addr4, sizeof(addr4))) {
				debug("Can't bind IPv4 TCP socket address: %s",
				      strerror(errno));
			}
		}

		if (*c->ip4.ifname_out) {
			if (setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE,
				       c->ip4.ifname_out,
				       strlen(c->ip4.ifname_out))) {
				debug("Can't bind IPv4 TCP socket to interface:"
				      " %s", strerror(errno));
			}
		}
	} else if (af == AF_INET6) {
		if (!IN6_IS_ADDR_UNSPECIFIED(&c->ip6.addr_out)) {
			struct sockaddr_in6 addr6 = {
				.sin6_family = AF_INET6,
				.sin6_port = 0,
				.sin6_addr = c->ip6.addr_out,
			};

			if (bind(s, (struct sockaddr *)&addr6, sizeof(addr6))) {
				debug("Can't bind IPv6 TCP socket address: %s",
				      strerror(errno));
			}
		}

		if (*c->ip6.ifname_out) {
			if (setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE,
				       c->ip6.ifname_out,
				       strlen(c->ip6.ifname_out))) {
				debug("Can't bind IPv6 TCP socket to interface:"
				      " %s", strerror(errno));
			}
		}
	}
}

/**
 * tcp_conn_from_tap() - Handle connection request (SYN segment) from tap
 * @c:		Execution context
 * @af:		Address family, AF_INET or AF_INET6
 * @saddr:	Source address, pointer to in_addr or in6_addr
 * @daddr:	Destination address, pointer to in_addr or in6_addr
 * @th:		TCP header from tap: caller MUST ensure it's there
 * @opts:	Pointer to start of options
 * @optlen:	Bytes in options: caller MUST ensure available length
 * @now:	Current timestamp
 */
static void tcp_conn_from_tap(struct ctx *c, sa_family_t af,
			      const void *saddr, const void *daddr,
			      const struct tcphdr *th, const char *opts,
			      size_t optlen, const struct timespec *now)
{
	in_port_t srcport = ntohs(th->source);
	in_port_t dstport = ntohs(th->dest);
	struct sockaddr_in addr4 = {
		.sin_family = AF_INET,
		.sin_port = htons(dstport),
		.sin_addr = *(struct in_addr *)daddr,
	};
	struct sockaddr_in6 addr6 = {
		.sin6_family = AF_INET6,
		.sin6_port = htons(dstport),
		.sin6_addr = *(struct in6_addr *)daddr,
	};
	const struct sockaddr *sa;
	struct tcp_tap_conn *conn;
	union flow *flow;
	int s = -1, mss;
	socklen_t sl;

	if (!(flow = flow_alloc()))
		return;

	if (af == AF_INET) {
		if (IN4_IS_ADDR_UNSPECIFIED(saddr) ||
		    IN4_IS_ADDR_BROADCAST(saddr) ||
		    IN4_IS_ADDR_MULTICAST(saddr) || srcport == 0 ||
		    IN4_IS_ADDR_UNSPECIFIED(daddr) ||
		    IN4_IS_ADDR_BROADCAST(daddr) ||
		    IN4_IS_ADDR_MULTICAST(daddr) || dstport == 0) {
			char sstr[INET_ADDRSTRLEN], dstr[INET_ADDRSTRLEN];

			debug("Invalid endpoint in TCP SYN: %s:%hu -> %s:%hu",
			      inet_ntop(AF_INET, saddr, sstr, sizeof(sstr)),
			      srcport,
			      inet_ntop(AF_INET, daddr, dstr, sizeof(dstr)),
			      dstport);
			goto cancel;
		}
	} else if (af == AF_INET6) {
		if (INANY_IS_ADDR_UNSPECIFIED(saddr) ||
		    IN6_IS_ADDR_MULTICAST(saddr) || srcport == 0 ||
		    INANY_IS_ADDR_UNSPECIFIED(daddr) ||
		    IN6_IS_ADDR_MULTICAST(daddr) || dstport == 0) {
			char sstr[INET6_ADDRSTRLEN], dstr[INET6_ADDRSTRLEN];

			debug("Invalid endpoint in TCP SYN: %s:%hu -> %s:%hu",
			      inet_ntop(AF_INET6, saddr, sstr, sizeof(sstr)),
			      srcport,
			      inet_ntop(AF_INET6, daddr, dstr, sizeof(dstr)),
			      dstport);
			goto cancel;
		}
	}

	if ((s = tcp_conn_sock(c, af)) < 0)
		goto cancel;

	if (!c->no_map_gw) {
		if (af == AF_INET && IN4_ARE_ADDR_EQUAL(daddr, &c->ip4.gw))
			addr4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (af == AF_INET6 && INANY_ARE_ADDR_EQUAL(daddr, &c->ip6.gw))
			addr6.sin6_addr	= in6addr_loopback;
	}

	if (af == AF_INET6 && IN6_IS_ADDR_LINKLOCAL(&addr6.sin6_addr)) {
		struct sockaddr_in6 addr6_ll = {
			.sin6_family = AF_INET6,
			.sin6_addr = c->ip6.addr_ll,
			.sin6_scope_id = c->ifi6,
		};
		if (bind(s, (struct sockaddr *)&addr6_ll, sizeof(addr6_ll)))
			goto cancel;
	}

	conn = FLOW_START(flow, FLOW_TCP, tcp, TAPSIDE);
	conn->sock = s;
	conn->timer = -1;
	conn_event(c, conn, TAP_SYN_RCVD);

	conn->wnd_to_tap = WINDOW_DEFAULT;

	mss = tcp_conn_tap_mss(conn, opts, optlen);
	if (setsockopt(s, SOL_TCP, TCP_MAXSEG, &mss, sizeof(mss)))
		flow_trace(conn, "failed to set TCP_MAXSEG on socket %i", s);
	MSS_SET(conn, mss);

	tcp_get_tap_ws(conn, opts, optlen);

	/* RFC 7323, 2.2: first value is not scaled. Also, don't clamp yet, to
	 * avoid getting a zero scale just because we set a small window now.
	 */
	if (!(conn->wnd_from_tap = (htons(th->window) >> conn->ws_from_tap)))
		conn->wnd_from_tap = 1;

	inany_from_af(&conn->faddr, af, daddr);

	if (af == AF_INET) {
		sa = (struct sockaddr *)&addr4;
		sl = sizeof(addr4);
	} else {
		sa = (struct sockaddr *)&addr6;
		sl = sizeof(addr6);
	}

	conn->fport = dstport;
	conn->eport = srcport;

	conn->seq_init_from_tap = ntohl(th->seq);
	conn->seq_from_tap = conn->seq_init_from_tap + 1;
	conn->seq_ack_to_tap = conn->seq_from_tap;

	tcp_seq_init(c, conn, now);
	conn->seq_ack_from_tap = conn->seq_to_tap;

	tcp_hash_insert(c, conn);

	if (!bind(s, sa, sl)) {
		tcp_rst(c, conn);	/* Nobody is listening then */
		return;
	}
	if (errno != EADDRNOTAVAIL && errno != EACCES)
		conn_flag(c, conn, LOCAL);

	if ((af == AF_INET &&  !IN4_IS_ADDR_LOOPBACK(&addr4.sin_addr)) ||
	    (af == AF_INET6 && !IN6_IS_ADDR_LOOPBACK(&addr6.sin6_addr) &&
			       !IN6_IS_ADDR_LINKLOCAL(&addr6.sin6_addr)))
		tcp_bind_outbound(c, s, af);

	if (connect(s, sa, sl)) {
		if (errno != EINPROGRESS) {
			tcp_rst(c, conn);
			return;
		}

		tcp_get_sndbuf(conn);
	} else {
		tcp_get_sndbuf(conn);

		if (tcp_send_flag(c, conn, SYN | ACK))
			return;

		conn_event(c, conn, TAP_SYN_ACK_SENT);
	}

	tcp_epoll_ctl(c, conn);
	return;

cancel:
	if (s >= 0)
		close(s);
	flow_alloc_cancel(flow);
}

/**
 * tcp_sock_consume() - Consume (discard) data from buffer
 * @conn:	Connection pointer
 * @ack_seq:	ACK sequence, host order
 *
 * Return: 0 on success, negative error code from recv() on failure
 */
#ifdef VALGRIND
/* valgrind doesn't realise that passing a NULL buffer to recv() is ok if using
 * MSG_TRUNC.  We have a suppression for this in the tests, but it relies on
 * valgrind being able to see the tcp_sock_consume() stack frame, which it won't
 * if this gets inlined.  This has a single caller making it a likely inlining
 * candidate, and certain compiler versions will do so even at -O0.
 */
 __attribute__((noinline))
#endif /* VALGRIND */
static int tcp_sock_consume(const struct tcp_tap_conn *conn, uint32_t ack_seq)
{
	/* Simply ignore out-of-order ACKs: we already consumed the data we
	 * needed from the buffer, and we won't rewind back to a lower ACK
	 * sequence.
	 */
	if (SEQ_LE(ack_seq, conn->seq_ack_from_tap))
		return 0;

	/* cppcheck-suppress [nullPointer, unmatchedSuppression] */
	if (recv(conn->sock, NULL, ack_seq - conn->seq_ack_from_tap,
		 MSG_DONTWAIT | MSG_TRUNC) < 0)
		return -errno;

	return 0;
}

/**
 * tcp_data_to_tap() - Finalise (queue) highest-numbered scatter-gather buffer
 * @c:		Execution context
 * @conn:	Connection pointer
 * @plen:	Payload length at L4
 * @no_csum:	Don't compute IPv4 checksum, use the one from previous buffer
 * @seq:	Sequence number to be sent
 */
static void tcp_data_to_tap(const struct ctx *c, struct tcp_tap_conn *conn,
			    ssize_t plen, int no_csum, uint32_t seq)
{
	uint32_t *seq_update = &conn->seq_to_tap;
	struct iovec *iov;

	if (CONN_V4(conn)) {
		struct tcp4_l2_buf_t *b = &tcp4_l2_buf[tcp4_l2_buf_used];
		const uint16_t *check = no_csum ? &(b - 1)->iph.check : NULL;

		tcp4_l2_buf_seq_update[tcp4_l2_buf_used].seq = seq_update;
		tcp4_l2_buf_seq_update[tcp4_l2_buf_used].len = plen;

		iov = tcp4_l2_iov + tcp4_l2_buf_used++;
		iov->iov_len = tcp_l2_buf_fill_headers(c, conn, b, plen,
						       check, seq);
		if (tcp4_l2_buf_used > ARRAY_SIZE(tcp4_l2_buf) - 1)
			tcp_l2_data_buf_flush(c);
	} else if (CONN_V6(conn)) {
		struct tcp6_l2_buf_t *b = &tcp6_l2_buf[tcp6_l2_buf_used];

		tcp6_l2_buf_seq_update[tcp6_l2_buf_used].seq = seq_update;
		tcp6_l2_buf_seq_update[tcp6_l2_buf_used].len = plen;

		iov = tcp6_l2_iov + tcp6_l2_buf_used++;
		iov->iov_len = tcp_l2_buf_fill_headers(c, conn, b, plen,
						       NULL, seq);
		if (tcp6_l2_buf_used > ARRAY_SIZE(tcp6_l2_buf) - 1)
			tcp_l2_data_buf_flush(c);
	}
}

/**
 * tcp_data_from_sock() - Handle new data from socket, queue to tap, in window
 * @c:		Execution context
 * @conn:	Connection pointer
 *
 * Return: negative on connection reset, 0 otherwise
 *
 * #syscalls recvmsg
 */
static int tcp_data_from_sock(struct ctx *c, struct tcp_tap_conn *conn)
{
	uint32_t wnd_scaled = conn->wnd_from_tap << conn->ws_from_tap;
	int fill_bufs, send_bufs = 0, last_len, iov_rem = 0;
	int sendlen, len, plen, v4 = CONN_V4(conn);
	int s = conn->sock, i, ret = 0;
	struct msghdr mh_sock = { 0 };
	uint16_t mss = MSS_GET(conn);
	uint32_t already_sent, seq;
	struct iovec *iov;

	already_sent = conn->seq_to_tap - conn->seq_ack_from_tap;

	if (SEQ_LT(already_sent, 0)) {
		/* RFC 761, section 2.1. */
		flow_trace(conn, "ACK sequence gap: ACK for %u, sent: %u",
			   conn->seq_ack_from_tap, conn->seq_to_tap);
		conn->seq_to_tap = conn->seq_ack_from_tap;
		already_sent = 0;
	}

	if (!wnd_scaled || already_sent >= wnd_scaled) {
		conn_flag(c, conn, STALLED);
		conn_flag(c, conn, ACK_FROM_TAP_DUE);
		return 0;
	}

	/* Set up buffer descriptors we'll fill completely and partially. */
	fill_bufs = DIV_ROUND_UP(wnd_scaled - already_sent, mss);
	if (fill_bufs > TCP_FRAMES) {
		fill_bufs = TCP_FRAMES;
		iov_rem = 0;
	} else {
		iov_rem = (wnd_scaled - already_sent) % mss;
	}

	mh_sock.msg_iov = iov_sock;
	mh_sock.msg_iovlen = fill_bufs + 1;

	iov_sock[0].iov_base = tcp_buf_discard;
	iov_sock[0].iov_len = already_sent;

	if (( v4 && tcp4_l2_buf_used + fill_bufs > ARRAY_SIZE(tcp4_l2_buf)) ||
	    (!v4 && tcp6_l2_buf_used + fill_bufs > ARRAY_SIZE(tcp6_l2_buf))) {
		tcp_l2_data_buf_flush(c);

		/* Silence Coverity CWE-125 false positive */
		tcp4_l2_buf_used = tcp6_l2_buf_used = 0;
	}

	for (i = 0, iov = iov_sock + 1; i < fill_bufs; i++, iov++) {
		if (v4)
			iov->iov_base = &tcp4_l2_buf[tcp4_l2_buf_used + i].data;
		else
			iov->iov_base = &tcp6_l2_buf[tcp6_l2_buf_used + i].data;
		iov->iov_len = mss;
	}
	if (iov_rem)
		iov_sock[fill_bufs].iov_len = iov_rem;

	/* Receive into buffers, don't dequeue until acknowledged by guest. */
	do
		len = recvmsg(s, &mh_sock, MSG_PEEK);
	while (len < 0 && errno == EINTR);

	if (len < 0)
		goto err;

	if (!len) {
		if ((conn->events & (SOCK_FIN_RCVD | TAP_FIN_SENT)) == SOCK_FIN_RCVD) {
			if ((ret = tcp_send_flag(c, conn, FIN | ACK))) {
				tcp_rst(c, conn);
				return ret;
			}

			conn_event(c, conn, TAP_FIN_SENT);
		}

		return 0;
	}

	sendlen = len - already_sent;
	if (sendlen <= 0) {
		conn_flag(c, conn, STALLED);
		return 0;
	}

	conn_flag(c, conn, ~STALLED);

	send_bufs = DIV_ROUND_UP(sendlen, mss);
	last_len = sendlen - (send_bufs - 1) * mss;

	/* Likely, some new data was acked too. */
	tcp_update_seqack_wnd(c, conn, 0, NULL);

	/* Finally, queue to tap */
	plen = mss;
	seq = conn->seq_to_tap;
	for (i = 0; i < send_bufs; i++) {
		int no_csum = i && i != send_bufs - 1 && tcp4_l2_buf_used;

		if (i == send_bufs - 1)
			plen = last_len;

		tcp_data_to_tap(c, conn, plen, no_csum, seq);
		seq += plen;
	}

	conn_flag(c, conn, ACK_FROM_TAP_DUE);

	return 0;

err:
	if (errno != EAGAIN && errno != EWOULDBLOCK) {
		ret = -errno;
		tcp_rst(c, conn);
	}

	return ret;
}

/**
 * tcp_data_from_tap() - tap/guest data for established connection
 * @c:		Execution context
 * @conn:	Connection pointer
 * @p:		Pool of TCP packets, with TCP headers
 * @idx:	Index of first data packet in pool
 *
 * #syscalls sendmsg
 *
 * Return: count of consumed packets
 */
static int tcp_data_from_tap(struct ctx *c, struct tcp_tap_conn *conn,
			      const struct pool *p, int idx)
{
	int i, iov_i, ack = 0, fin = 0, retr = 0, keep = -1, partial_send = 0;
	uint16_t max_ack_seq_wnd = conn->wnd_from_tap;
	uint32_t max_ack_seq = conn->seq_ack_from_tap;
	uint32_t seq_from_tap = conn->seq_from_tap;
	struct msghdr mh = { .msg_iov = tcp_iov };
	size_t len;
	ssize_t n;

	if (conn->events == CLOSED)
		return p->count - idx;

	ASSERT(conn->events & ESTABLISHED);

	for (i = idx, iov_i = 0; i < (int)p->count; i++) {
		uint32_t seq, seq_offset, ack_seq;
		const struct tcphdr *th;
		char *data;
		size_t off;

		th = packet_get(p, i, 0, sizeof(*th), &len);
		if (!th)
			return -1;
		len += sizeof(*th);

		off = th->doff * 4UL;
		if (off < sizeof(*th) || off > len)
			return -1;

		if (th->rst) {
			conn_event(c, conn, CLOSED);
			return 1;
		}

		len -= off;
		data = packet_get(p, i, off, len, NULL);
		if (!data)
			continue;

		seq = ntohl(th->seq);
		ack_seq = ntohl(th->ack_seq);

		if (th->ack) {
			ack = 1;

			if (SEQ_GE(ack_seq, conn->seq_ack_from_tap) &&
			    SEQ_GE(ack_seq, max_ack_seq)) {
				/* Fast re-transmit */
				retr = !len && !th->fin &&
				       ack_seq == max_ack_seq &&
				       ntohs(th->window) == max_ack_seq_wnd;

				max_ack_seq_wnd = ntohs(th->window);
				max_ack_seq = ack_seq;
			}
		}

		if (th->fin)
			fin = 1;

		if (!len)
			continue;

		seq_offset = seq_from_tap - seq;
		/* Use data from this buffer only in these two cases:
		 *
		 *      , seq_from_tap           , seq_from_tap
		 * |--------| <-- len            |--------| <-- len
		 * '----' <-- offset             ' <-- offset
		 * ^ seq                         ^ seq
		 *    (offset >= 0, seq + len > seq_from_tap)
		 *
		 * discard in these two cases:
		 *          , seq_from_tap                , seq_from_tap
		 * |--------| <-- len            |--------| <-- len
		 * '--------' <-- offset            '-----| <- offset
		 * ^ seq                            ^ seq
		 *    (offset >= 0, seq + len <= seq_from_tap)
		 *
		 * keep, look for another buffer, then go back, in this case:
		 *      , seq_from_tap
		 *          |--------| <-- len
		 *      '===' <-- offset
		 *          ^ seq
		 *    (offset < 0)
		 */
		if (SEQ_GE(seq_offset, 0) && SEQ_LE(seq + len, seq_from_tap))
			continue;

		if (SEQ_LT(seq_offset, 0)) {
			if (keep == -1)
				keep = i;
			continue;
		}

		tcp_iov[iov_i].iov_base = data + seq_offset;
		tcp_iov[iov_i].iov_len = len - seq_offset;
		seq_from_tap += tcp_iov[iov_i].iov_len;
		iov_i++;

		if (keep == i)
			keep = -1;

		if (keep != -1)
			i = keep - 1;
	}

	/* On socket flush failure, pretend there was no ACK, try again later */
	if (ack && !tcp_sock_consume(conn, max_ack_seq))
		tcp_update_seqack_from_tap(c, conn, max_ack_seq);

	tcp_tap_window_update(conn, max_ack_seq_wnd);

	if (retr) {
		flow_trace(conn,
			   "fast re-transmit, ACK: %u, previous sequence: %u",
			   max_ack_seq, conn->seq_to_tap);
		conn->seq_to_tap = max_ack_seq;
		tcp_data_from_sock(c, conn);
	}

	if (!iov_i)
		goto out;

	mh.msg_iovlen = iov_i;
eintr:
	n = sendmsg(conn->sock, &mh, MSG_DONTWAIT | MSG_NOSIGNAL);
	if (n < 0) {
		if (errno == EPIPE) {
			/* Here's the wrap, said the tap.
			 * In my pocket, said the socket.
			 *   Then swiftly looked away and left.
			 */
			conn->seq_from_tap = seq_from_tap;
			tcp_send_flag(c, conn, ACK);
		}

		if (errno == EINTR)
			goto eintr;

		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			tcp_send_flag(c, conn, ACK_IF_NEEDED);
			return p->count - idx;

		}
		return -1;
	}

	if (n < (int)(seq_from_tap - conn->seq_from_tap)) {
		partial_send = 1;
		conn->seq_from_tap += n;
		tcp_send_flag(c, conn, ACK_IF_NEEDED);
	} else {
		conn->seq_from_tap += n;
	}

out:
	if (keep != -1) {
		/* We use an 8-bit approximation here: the associated risk is
		 * that we skip a duplicate ACK on 8-bit sequence number
		 * collision. Fast retransmit is a SHOULD in RFC 5681, 3.2.
		 */
		if (conn->seq_dup_ack_approx != (conn->seq_from_tap & 0xff)) {
			conn->seq_dup_ack_approx = conn->seq_from_tap & 0xff;
			tcp_send_flag(c, conn, ACK | DUP_ACK);
		}
		return p->count - idx;
	}

	if (ack && conn->events & TAP_FIN_SENT &&
	    conn->seq_ack_from_tap == conn->seq_to_tap)
		conn_event(c, conn, TAP_FIN_ACKED);

	if (fin && !partial_send) {
		conn->seq_from_tap++;

		conn_event(c, conn, TAP_FIN_RCVD);
	} else {
		tcp_send_flag(c, conn, ACK_IF_NEEDED);
	}

	return p->count - idx;
}

/**
 * tcp_conn_from_sock_finish() - Complete connection setup after connect()
 * @c:		Execution context
 * @conn:	Connection pointer
 * @th:		TCP header of SYN, ACK segment: caller MUST ensure it's there
 * @opts:	Pointer to start of options
 * @optlen:	Bytes in options: caller MUST ensure available length
 */
static void tcp_conn_from_sock_finish(struct ctx *c, struct tcp_tap_conn *conn,
				      const struct tcphdr *th,
				      const char *opts, size_t optlen)
{
	tcp_tap_window_update(conn, ntohs(th->window));
	tcp_get_tap_ws(conn, opts, optlen);

	/* First value is not scaled */
	if (!(conn->wnd_from_tap >>= conn->ws_from_tap))
		conn->wnd_from_tap = 1;

	MSS_SET(conn, tcp_conn_tap_mss(conn, opts, optlen));

	conn->seq_init_from_tap = ntohl(th->seq) + 1;
	conn->seq_from_tap = conn->seq_init_from_tap;
	conn->seq_ack_to_tap = conn->seq_from_tap;

	conn_event(c, conn, ESTABLISHED);

	/* The client might have sent data already, which we didn't
	 * dequeue waiting for SYN,ACK from tap -- check now.
	 */
	tcp_data_from_sock(c, conn);
	tcp_send_flag(c, conn, ACK);
}

/**
 * tcp_tap_handler() - Handle packets from tap and state transitions
 * @c:		Execution context
 * @pif:	pif on which the packet is arriving
 * @af:		Address family, AF_INET or AF_INET6
 * @saddr:	Source address
 * @daddr:	Destination address
 * @p:		Pool of TCP packets, with TCP headers
 * @idx:	Index of first packet in pool to process
 * @now:	Current timestamp
 *
 * Return: count of consumed packets
 */
int tcp_tap_handler(struct ctx *c, uint8_t pif, sa_family_t af,
		    const void *saddr, const void *daddr,
		    const struct pool *p, int idx, const struct timespec *now)
{
	struct tcp_tap_conn *conn;
	const struct tcphdr *th;
	size_t optlen, len;
	const char *opts;
	int ack_due = 0;
	int count;

	(void)pif;

	th = packet_get(p, idx, 0, sizeof(*th), &len);
	if (!th)
		return 1;
	len += sizeof(*th);

	optlen = th->doff * 4UL - sizeof(*th);
	/* Static checkers might fail to see this: */
	optlen = MIN(optlen, ((1UL << 4) /* from doff width */ - 6) * 4UL);
	opts = packet_get(p, idx, sizeof(*th), optlen, NULL);

	conn = tcp_hash_lookup(c, af, daddr, ntohs(th->source), ntohs(th->dest));

	/* New connection from tap */
	if (!conn) {
		if (opts && th->syn && !th->ack)
			tcp_conn_from_tap(c, af, saddr, daddr, th,
					  opts, optlen, now);
		return 1;
	}

	flow_trace(conn, "packet length %zu from tap", len);

	if (th->rst) {
		conn_event(c, conn, CLOSED);
		return 1;
	}

	if (th->ack && !(conn->events & ESTABLISHED))
		tcp_update_seqack_from_tap(c, conn, ntohl(th->ack_seq));

	/* Establishing connection from socket */
	if (conn->events & SOCK_ACCEPTED) {
		if (th->syn && th->ack && !th->fin) {
			tcp_conn_from_sock_finish(c, conn, th, opts, optlen);
			return 1;
		}

		goto reset;
	}

	/* Establishing connection from tap */
	if (conn->events & TAP_SYN_RCVD) {
		if (!(conn->events & TAP_SYN_ACK_SENT))
			goto reset;

		conn_event(c, conn, ESTABLISHED);

		if (th->fin) {
			conn->seq_from_tap++;

			shutdown(conn->sock, SHUT_WR);
			tcp_send_flag(c, conn, ACK);
			conn_event(c, conn, SOCK_FIN_SENT);

			return 1;
		}

		if (!th->ack)
			goto reset;

		tcp_tap_window_update(conn, ntohs(th->window));

		tcp_data_from_sock(c, conn);

		if (p->count - idx == 1)
			return 1;
	}

	/* Established connections not accepting data from tap */
	if (conn->events & TAP_FIN_RCVD) {
		tcp_update_seqack_from_tap(c, conn, ntohl(th->ack_seq));

		if (conn->events & SOCK_FIN_RCVD &&
		    conn->seq_ack_from_tap == conn->seq_to_tap)
			conn_event(c, conn, CLOSED);

		return 1;
	}

	/* Established connections accepting data from tap */
	count = tcp_data_from_tap(c, conn, p, idx);
	if (count == -1)
		goto reset;

	conn_flag(c, conn, ~STALLED);

	if (conn->seq_ack_to_tap != conn->seq_from_tap)
		ack_due = 1;

	if ((conn->events & TAP_FIN_RCVD) && !(conn->events & SOCK_FIN_SENT)) {
		shutdown(conn->sock, SHUT_WR);
		conn_event(c, conn, SOCK_FIN_SENT);
		tcp_send_flag(c, conn, ACK);
		ack_due = 0;
	}

	if (ack_due)
		conn_flag(c, conn, ACK_TO_TAP_DUE);

	return count;

reset:
	/* Something's gone wrong, so reset the connection.  We discard
	 * remaining packets in the batch, since they'd be invalidated when our
	 * RST is received, even if otherwise good.
	 */
	tcp_rst(c, conn);
	return p->count - idx;
}

/**
 * tcp_connect_finish() - Handle completion of connect() from EPOLLOUT event
 * @c:		Execution context
 * @conn:	Connection pointer
 */
static void tcp_connect_finish(struct ctx *c, struct tcp_tap_conn *conn)
{
	socklen_t sl;
	int so;

	sl = sizeof(so);
	if (getsockopt(conn->sock, SOL_SOCKET, SO_ERROR, &so, &sl) || so) {
		tcp_rst(c, conn);
		return;
	}

	if (tcp_send_flag(c, conn, SYN | ACK))
		return;

	conn_event(c, conn, TAP_SYN_ACK_SENT);
	conn_flag(c, conn, ACK_FROM_TAP_DUE);
}

/**
 * tcp_snat_inbound() - Translate source address for inbound data if needed
 * @c:		Execution context
 * @addr:	Source address of inbound packet/connection
 */
static void tcp_snat_inbound(const struct ctx *c, union inany_addr *addr)
{
	struct in_addr *addr4 = inany_v4(addr);

	if (addr4) {
		if (IN4_IS_ADDR_LOOPBACK(addr4) ||
		    IN4_IS_ADDR_UNSPECIFIED(addr4) ||
		    IN4_ARE_ADDR_EQUAL(addr4, &c->ip4.addr_seen))
			*addr4 = c->ip4.gw;
	} else {
		struct in6_addr *addr6 = &addr->a6;

		if (IN6_IS_ADDR_LOOPBACK(addr6) ||
		    IN6_ARE_ADDR_EQUAL(addr6, &c->ip6.addr_seen) ||
		    IN6_ARE_ADDR_EQUAL(addr6, &c->ip6.addr)) {
			if (IN6_IS_ADDR_LINKLOCAL(&c->ip6.gw))
				*addr6 = c->ip6.gw;
			else
				*addr6 = c->ip6.addr_ll;
		}
	}
}

/**
 * tcp_tap_conn_from_sock() - Initialize state for non-spliced connection
 * @c:		Execution context
 * @dstport:	Destination port for connection (host side)
 * @flow:	flow to initialise
 * @s:		Accepted socket
 * @sa:		Peer socket address (from accept())
 * @now:	Current timestamp
 */
static void tcp_tap_conn_from_sock(struct ctx *c, in_port_t dstport,
				   union flow *flow, int s,
				   const union sockaddr_inany *sa,
				   const struct timespec *now)
{
	struct tcp_tap_conn *conn = FLOW_START(flow, FLOW_TCP, tcp, SOCKSIDE);

	conn->sock = s;
	conn->timer = -1;
	conn->ws_to_tap = conn->ws_from_tap = 0;
	conn_event(c, conn, SOCK_ACCEPTED);

	inany_from_sockaddr(&conn->faddr, &conn->fport, sa);
	conn->eport = dstport + c->tcp.fwd_in.delta[dstport];

	tcp_snat_inbound(c, &conn->faddr);

	tcp_seq_init(c, conn, now);
	tcp_hash_insert(c, conn);

	conn->seq_ack_from_tap = conn->seq_to_tap;

	conn->wnd_from_tap = WINDOW_DEFAULT;

	tcp_send_flag(c, conn, SYN);
	conn_flag(c, conn, ACK_FROM_TAP_DUE);

	tcp_get_sndbuf(conn);
}

/**
 * tcp_listen_handler() - Handle new connection request from listening socket
 * @c:		Execution context
 * @ref:	epoll reference of listening socket
 * @now:	Current timestamp
 */
void tcp_listen_handler(struct ctx *c, union epoll_ref ref,
			const struct timespec *now)
{
	union sockaddr_inany sa;
	socklen_t sl = sizeof(sa);
	union flow *flow;
	int s;

	if (c->no_tcp || !(flow = flow_alloc()))
		return;

	s = accept4(ref.fd, &sa.sa, &sl, SOCK_NONBLOCK);
	if (s < 0)
		goto cancel;

	if (sa.sa_family == AF_INET) {
		const struct in_addr *addr = &sa.sa4.sin_addr;
		in_port_t port = sa.sa4.sin_port;

		if (IN4_IS_ADDR_UNSPECIFIED(addr) ||
		    IN4_IS_ADDR_BROADCAST(addr) ||
		    IN4_IS_ADDR_MULTICAST(addr) || port == 0) {
			char str[INET_ADDRSTRLEN];

			err("Invalid endpoint from TCP accept(): %s:%hu",
			    inet_ntop(AF_INET, addr, str, sizeof(str)), port);
			goto cancel;
		}
	} else if (sa.sa_family == AF_INET6) {
		const struct in6_addr *addr = &sa.sa6.sin6_addr;
		in_port_t port = sa.sa6.sin6_port;

		if (IN6_IS_ADDR_UNSPECIFIED(addr) ||
		    IN6_IS_ADDR_MULTICAST(addr) || port == 0) {
			char str[INET6_ADDRSTRLEN];

			err("Invalid endpoint from TCP accept(): %s:%hu",
			    inet_ntop(AF_INET6, addr, str, sizeof(str)), port);
			goto cancel;
		}
	}

	if (tcp_splice_conn_from_sock(c, ref.tcp_listen.pif,
				      ref.tcp_listen.port, flow, s, &sa))
		return;

	tcp_tap_conn_from_sock(c, ref.tcp_listen.port, flow, s, &sa, now);
	return;

cancel:
	flow_alloc_cancel(flow);
}

/**
 * tcp_timer_handler() - timerfd events: close, send ACK, retransmit, or reset
 * @c:		Execution context
 * @ref:	epoll reference of timer (not connection)
 *
 * #syscalls timerfd_gettime
 */
void tcp_timer_handler(struct ctx *c, union epoll_ref ref)
{
	struct itimerspec check_armed = { { 0 }, { 0 } };
	struct tcp_tap_conn *conn = CONN(ref.flow);

	if (c->no_tcp)
		return;

	/* We don't reset timers on ~ACK_FROM_TAP_DUE, ~ACK_TO_TAP_DUE. If the
	 * timer is currently armed, this event came from a previous setting,
	 * and we just set the timer to a new point in the future: discard it.
	 */
	timerfd_gettime(conn->timer, &check_armed);
	if (check_armed.it_value.tv_sec || check_armed.it_value.tv_nsec)
		return;

	if (conn->flags & ACK_TO_TAP_DUE) {
		tcp_send_flag(c, conn, ACK_IF_NEEDED);
		tcp_timer_ctl(c, conn);
	} else if (conn->flags & ACK_FROM_TAP_DUE) {
		if (!(conn->events & ESTABLISHED)) {
			flow_dbg(conn, "handshake timeout");
			tcp_rst(c, conn);
		} else if (CONN_HAS(conn, SOCK_FIN_SENT | TAP_FIN_ACKED)) {
			flow_dbg(conn, "FIN timeout");
			tcp_rst(c, conn);
		} else if (conn->retrans == TCP_MAX_RETRANS) {
			flow_dbg(conn, "retransmissions count exceeded");
			tcp_rst(c, conn);
		} else {
			flow_dbg(conn, "ACK timeout, retry");
			conn->retrans++;
			conn->seq_to_tap = conn->seq_ack_from_tap;
			tcp_data_from_sock(c, conn);
			tcp_timer_ctl(c, conn);
		}
	} else {
		struct itimerspec new = { { 0 }, { ACT_TIMEOUT, 0 } };
		struct itimerspec old = { { 0 }, { 0 } };

		/* Activity timeout: if it was already set, reset the
		 * connection, otherwise, it was a left-over from ACK_TO_TAP_DUE
		 * or ACK_FROM_TAP_DUE, so just set the long timeout in that
		 * case. This avoids having to preemptively reset the timer on
		 * ~ACK_TO_TAP_DUE or ~ACK_FROM_TAP_DUE.
		 */
		timerfd_settime(conn->timer, 0, &new, &old);
		if (old.it_value.tv_sec == ACT_TIMEOUT) {
			flow_dbg(conn, "activity timeout");
			tcp_rst(c, conn);
		}
	}
}

/**
 * tcp_sock_handler() - Handle new data from non-spliced socket
 * @c:		Execution context
 * @ref:	epoll reference
 * @events:	epoll events bitmap
 */
void tcp_sock_handler(struct ctx *c, union epoll_ref ref, uint32_t events)
{
	struct tcp_tap_conn *conn = CONN(ref.flowside.flow);

	ASSERT(conn->f.type == FLOW_TCP);
	ASSERT(ref.flowside.side == SOCKSIDE);

	if (conn->events == CLOSED)
		return;

	if (events & EPOLLERR) {
		tcp_rst(c, conn);
		return;
	}

	if ((conn->events & TAP_FIN_SENT) && (events & EPOLLHUP)) {
		conn_event(c, conn, CLOSED);
		return;
	}

	if (conn->events & ESTABLISHED) {
		if (CONN_HAS(conn, SOCK_FIN_SENT | TAP_FIN_ACKED))
			conn_event(c, conn, CLOSED);

		if (events & (EPOLLRDHUP | EPOLLHUP))
			conn_event(c, conn, SOCK_FIN_RCVD);

		if (events & EPOLLIN)
			tcp_data_from_sock(c, conn);

		if (events & EPOLLOUT)
			tcp_update_seqack_wnd(c, conn, 0, NULL);

		return;
	}

	/* EPOLLHUP during handshake: reset */
	if (events & EPOLLHUP) {
		tcp_rst(c, conn);
		return;
	}

	/* Data during handshake tap-side: check later */
	if (conn->events & SOCK_ACCEPTED)
		return;

	if (conn->events == TAP_SYN_RCVD) {
		if (events & EPOLLOUT)
			tcp_connect_finish(c, conn);
		/* Data? Check later */
	}
}

/**
 * tcp_sock_init_af() - Initialise listening socket for a given af and port
 * @c:		Execution context
 * @af:		Address family to listen on
 * @port:	Port, host order
 * @addr:	Pointer to address for binding, NULL if not configured
 * @ifname:	Name of interface to bind to, NULL if not configured
 *
 * Return: fd for the new listening socket, negative error code on failure
 */
static int tcp_sock_init_af(const struct ctx *c, sa_family_t af, in_port_t port,
			    const void *addr, const char *ifname)
{
	union tcp_listen_epoll_ref tref = {
		.port = port,
		.pif = PIF_HOST,
	};
	int s;

	s = sock_l4(c, af, IPPROTO_TCP, addr, ifname, port, tref.u32);

	if (c->tcp.fwd_in.mode == FWD_AUTO) {
		if (af == AF_INET  || af == AF_UNSPEC)
			tcp_sock_init_ext[port][V4] = s < 0 ? -1 : s;
		if (af == AF_INET6 || af == AF_UNSPEC)
			tcp_sock_init_ext[port][V6] = s < 0 ? -1 : s;
	}

	if (s < 0)
		return s;

	tcp_sock_set_bufsize(c, s);
	return s;
}

/**
 * tcp_sock_init() - Create listening sockets for a given host ("inbound") port
 * @c:		Execution context
 * @af:		Address family to select a specific IP version, or AF_UNSPEC
 * @addr:	Pointer to address for binding, NULL if not configured
 * @ifname:	Name of interface to bind to, NULL if not configured
 * @port:	Port, host order
 *
 * Return: 0 on (partial) success, negative error code on (complete) failure
 */
int tcp_sock_init(const struct ctx *c, sa_family_t af, const void *addr,
		  const char *ifname, in_port_t port)
{
	int r4 = FD_REF_MAX + 1, r6 = FD_REF_MAX + 1;

	if (af == AF_UNSPEC && c->ifi4 && c->ifi6)
		/* Attempt to get a dual stack socket */
		if (tcp_sock_init_af(c, AF_UNSPEC, port, addr, ifname) >= 0)
			return 0;

	/* Otherwise create a socket per IP version */
	if ((af == AF_INET  || af == AF_UNSPEC) && c->ifi4)
		r4 = tcp_sock_init_af(c, AF_INET, port, addr, ifname);

	if ((af == AF_INET6 || af == AF_UNSPEC) && c->ifi6)
		r6 = tcp_sock_init_af(c, AF_INET6, port, addr, ifname);

	if (IN_INTERVAL(0, FD_REF_MAX, r4) || IN_INTERVAL(0, FD_REF_MAX, r6))
		return 0;

	return r4 < 0 ? r4 : r6;
}

/**
 * tcp_ns_sock_init4() - Init socket to listen for outbound IPv4 connections
 * @c:		Execution context
 * @port:	Port, host order
 */
static void tcp_ns_sock_init4(const struct ctx *c, in_port_t port)
{
	union tcp_listen_epoll_ref tref = {
		.port = port,
		.pif = PIF_SPLICE,
	};
	int s;

	ASSERT(c->mode == MODE_PASTA);

	s = sock_l4(c, AF_INET, IPPROTO_TCP, &in4addr_loopback, NULL, port,
		    tref.u32);
	if (s >= 0)
		tcp_sock_set_bufsize(c, s);
	else
		s = -1;

	if (c->tcp.fwd_out.mode == FWD_AUTO)
		tcp_sock_ns[port][V4] = s;
}

/**
 * tcp_ns_sock_init6() - Init socket to listen for outbound IPv6 connections
 * @c:		Execution context
 * @port:	Port, host order
 */
static void tcp_ns_sock_init6(const struct ctx *c, in_port_t port)
{
	union tcp_listen_epoll_ref tref = {
		.port = port,
		.pif = PIF_SPLICE,
	};
	int s;

	ASSERT(c->mode == MODE_PASTA);

	s = sock_l4(c, AF_INET6, IPPROTO_TCP, &in6addr_loopback, NULL, port,
		    tref.u32);
	if (s >= 0)
		tcp_sock_set_bufsize(c, s);
	else
		s = -1;

	if (c->tcp.fwd_out.mode == FWD_AUTO)
		tcp_sock_ns[port][V6] = s;
}

/**
 * tcp_ns_sock_init() - Init socket to listen for spliced outbound connections
 * @c:		Execution context
 * @port:	Port, host order
 */
void tcp_ns_sock_init(const struct ctx *c, in_port_t port)
{
	if (c->ifi4)
		tcp_ns_sock_init4(c, port);
	if (c->ifi6)
		tcp_ns_sock_init6(c, port);
}

/**
 * tcp_ns_socks_init() - Bind sockets in namespace for outbound connections
 * @arg:	Execution context
 *
 * Return: 0
 */
static int tcp_ns_socks_init(void *arg)
{
	const struct ctx *c = (const struct ctx *)arg;
	unsigned port;

	ns_enter(c);

	for (port = 0; port < NUM_PORTS; port++) {
		if (!bitmap_isset(c->tcp.fwd_out.map, port))
			continue;

		tcp_ns_sock_init(c, port);
	}

	return 0;
}

/**
 * tcp_sock_refill_pool() - Refill one pool of pre-opened sockets
 * @c:		Execution context
 * @pool:	Pool of sockets to refill
 * @af:		Address family to use
 *
 * Return: 0 on success, negative error code if there was at least one error
 */
int tcp_sock_refill_pool(const struct ctx *c, int pool[], sa_family_t af)
{
	int i;

	for (i = 0; i < TCP_SOCK_POOL_SIZE; i++) {
		int fd;

		if (pool[i] >= 0)
			continue;

		if ((fd = tcp_conn_new_sock(c, af)) < 0)
			return fd;

		pool[i] = fd;
	}

	return 0;
}

/**
 * tcp_sock_refill_init() - Refill pools of pre-opened sockets in init ns
 * @c:		Execution context
 */
static void tcp_sock_refill_init(const struct ctx *c)
{
	if (c->ifi4) {
		int rc = tcp_sock_refill_pool(c, init_sock_pool4, AF_INET);
		if (rc < 0)
			warn("TCP: Error refilling IPv4 host socket pool: %s",
			     strerror(-rc));
	}
	if (c->ifi6) {
		int rc = tcp_sock_refill_pool(c, init_sock_pool6, AF_INET6);
		if (rc < 0)
			warn("TCP: Error refilling IPv6 host socket pool: %s",
			     strerror(-rc));
	}
}

/**
 * tcp_init() - Get initial sequence, hash secret, initialise per-socket data
 * @c:		Execution context
 *
 * Return: 0, doesn't return on failure
 */
int tcp_init(struct ctx *c)
{
	unsigned b;

	for (b = 0; b < TCP_HASH_TABLE_SIZE; b++)
		tc_hash[b] = FLOW_SIDX_NONE;

	if (c->ifi4)
		tcp_sock4_iov_init(c);

	if (c->ifi6)
		tcp_sock6_iov_init(c);

	memset(init_sock_pool4,		0xff,	sizeof(init_sock_pool4));
	memset(init_sock_pool6,		0xff,	sizeof(init_sock_pool6));
	memset(tcp_sock_init_ext,	0xff,	sizeof(tcp_sock_init_ext));
	memset(tcp_sock_ns,		0xff,	sizeof(tcp_sock_ns));

	tcp_sock_refill_init(c);

	if (c->mode == MODE_PASTA) {
		tcp_splice_init(c);

		NS_CALL(tcp_ns_socks_init, c);
	}

	return 0;
}

/**
 * tcp_port_rebind() - Rebind ports to match forward maps
 * @c:		Execution context
 * @outbound:	True to remap outbound forwards, otherwise inbound
 *
 * Must be called in namespace context if @outbound is true.
 */
static void tcp_port_rebind(struct ctx *c, bool outbound)
{
	const uint8_t *fmap = outbound ? c->tcp.fwd_out.map : c->tcp.fwd_in.map;
	const uint8_t *rmap = outbound ? c->tcp.fwd_in.map : c->tcp.fwd_out.map;
	int (*socks)[IP_VERSIONS] = outbound ? tcp_sock_ns : tcp_sock_init_ext;
	unsigned port;

	for (port = 0; port < NUM_PORTS; port++) {
		if (!bitmap_isset(fmap, port)) {
			if (socks[port][V4] >= 0) {
				close(socks[port][V4]);
				socks[port][V4] = -1;
			}

			if (socks[port][V6] >= 0) {
				close(socks[port][V6]);
				socks[port][V6] = -1;
			}

			continue;
		}

		/* Don't loop back our own ports */
		if (bitmap_isset(rmap, port))
			continue;

		if ((c->ifi4 && socks[port][V4] == -1) ||
		    (c->ifi6 && socks[port][V6] == -1)) {
			if (outbound)
				tcp_ns_sock_init(c, port);
			else
				tcp_sock_init(c, AF_UNSPEC, NULL, NULL, port);
		}
	}
}

/**
 * tcp_port_rebind_outbound() - Rebind ports in namespace
 * @arg:	Execution context
 *
 * Called with NS_CALL()
 *
 * Return: 0
 */
static int tcp_port_rebind_outbound(void *arg)
{
	struct ctx *c = (struct ctx *)arg;

	ns_enter(c);
	tcp_port_rebind(c, true);

	return 0;
}

/**
 * tcp_timer() - Periodic tasks: port detection, closed connections, pool refill
 * @c:		Execution context
 * @now:	Current timestamp
 */
void tcp_timer(struct ctx *c, const struct timespec *now)
{
	(void)now;

	if (c->mode == MODE_PASTA) {
		if (c->tcp.fwd_out.mode == FWD_AUTO) {
			fwd_scan_ports_tcp(&c->tcp.fwd_out, &c->tcp.fwd_in);
			NS_CALL(tcp_port_rebind_outbound, c);
		}

		if (c->tcp.fwd_in.mode == FWD_AUTO) {
			fwd_scan_ports_tcp(&c->tcp.fwd_in, &c->tcp.fwd_out);
			tcp_port_rebind(c, false);
		}
	}

	tcp_sock_refill_init(c);
	if (c->mode == MODE_PASTA)
		tcp_splice_refill(c);
}
