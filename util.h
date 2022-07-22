/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (c) 2021 Red Hat GmbH
 * Author: Stefano Brivio <sbrivio@redhat.com>
 */

#ifndef UTIL_H
#define UTIL_H

void err(const char *format, ...);
void warn(const char *format, ...);
void info(const char *format, ...);
void debug(const char *format, ...);

extern int log_trace;
void trace_init(int enable);
#define trace(format, ...)						\
	do {								\
		if (log_trace)						\
			debug(format, ##__VA_ARGS__);			\
	} while (0)

#ifndef SECCOMP_RET_KILL_PROCESS
#define SECCOMP_RET_KILL_PROCESS	SECCOMP_RET_KILL
#endif
#ifndef ETH_MAX_MTU
#define ETH_MAX_MTU			USHRT_MAX
#endif
#ifndef ETH_MIN_MTU
#define ETH_MIN_MTU			68
#endif

#ifndef MIN
#define MIN(x, y)		(((x) < (y)) ? (x) : (y))
#endif
#ifndef MAX
#define MAX(x, y)		(((x) > (y)) ? (x) : (y))
#endif

#define DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))
#define ROUND_DOWN(x, y)	((x) & ~((y) - 1))
#define ROUND_UP(x, y)		(((x) + (y) - 1) & ~((y) - 1))

#define BIT(n)			(1UL << (n))
#define BITMAP_BIT(n)		(BIT((n) % (sizeof(long) * 8)))
#define BITMAP_WORD(n)		(n / (sizeof(long) * 8))

#define SWAP(a, b)							\
	do {								\
		__typeof__(a) __x = (a); (a) = (b); (b) = __x;		\
	} while (0)							\

#define STRINGIFY(x)	#x
#define STR(x)		STRINGIFY(x)

#ifdef P_tmpdir
#define TMPDIR		P_tmpdir
#else
#define TMPDIR		"/tmp"
#endif

#define FWRITE(path, buf, str)						\
	do {								\
		int flags = O_WRONLY | O_CLOEXEC;			\
		int fd = open(path, flags);				\
									\
		if (fd < 0 ||						\
		    write(fd, buf, strlen(buf)) != (int)strlen(buf))	\
			warn(str);					\
		if (fd >= 0)						\
			close(fd);					\
	} while (0)

#define V4		0
#define V6		1
#define IP_VERSIONS	2

#define ARRAY_SIZE(a)		((int)(sizeof(a) / sizeof((a)[0])))

#define IN_INTERVAL(a, b, x)	((x) >= (a) && (x) <= (b))
#define FD_PROTO(x, proto)						\
	(IN_INTERVAL(c->proto.fd_min, c->proto.fd_max, (x)))

#define PORT_EPHEMERAL_MIN	((1 << 15) + (1 << 14))		/* RFC 6335 */
#define PORT_IS_EPHEMERAL(port) ((port) >= PORT_EPHEMERAL_MIN)

#define MAC_ZERO		((uint8_t [ETH_ALEN]){ 0 })
#define MAC_IS_ZERO(addr)	(!memcmp((addr), MAC_ZERO, ETH_ALEN))

#define NS_FN_STACK_SIZE	(RLIMIT_STACK_VAL * 1024 / 4)
#define NS_CALL(fn, arg)						\
	do {								\
		char ns_fn_stack[NS_FN_STACK_SIZE];			\
									\
		clone((fn), ns_fn_stack + sizeof(ns_fn_stack) / 2,	\
		      CLONE_VM | CLONE_VFORK | CLONE_FILES | SIGCHLD,	\
		      (void *)(arg));					\
	} while (0)


#ifdef __has_warning
# if __has_warning("-Wstringop-overread")
#  define PRAGMA_STRINGOP_OVERREAD_IGNORE				\
     _Pragma("GCC diagnostic ignored \"-Wstringop-overread\"")
#  define PRAGMA_STRINGOP_OVERREAD_IGNORE_POP				\
     _Pragma("GCC diagnostic pop")
# else
#  define PRAGMA_STRINGOP_OVERREAD_IGNORE
#  define PRAGMA_STRINGOP_OVERREAD_IGNORE_POP
# endif
#else
# if defined(__GNUC__) && __GNUC__ >= 11
#  define PRAGMA_STRINGOP_OVERREAD_IGNORE				\
     _Pragma("GCC diagnostic ignored \"-Wstringop-overread\"")
#  define PRAGMA_STRINGOP_OVERREAD_IGNORE_POP				\
     _Pragma("GCC diagnostic pop")
# else
#  define PRAGMA_STRINGOP_OVERREAD_IGNORE
#  define PRAGMA_STRINGOP_OVERREAD_IGNORE_POP
# endif
#endif

#if __BYTE_ORDER == __BIG_ENDIAN
#define L2_BUF_ETH_IP4_INIT						\
	{								\
		.h_dest		= { 0 },				\
		.h_source	= { 0 },				\
		.h_proto	= ETH_P_IP,				\
	}
#else
#define L2_BUF_ETH_IP4_INIT						\
	{								\
		.h_dest		= { 0 },				\
		.h_source	= { 0 },				\
		.h_proto	= __bswap_constant_16(ETH_P_IP),	\
	}
#endif

#if __BYTE_ORDER == __BIG_ENDIAN
#define L2_BUF_ETH_IP6_INIT						\
	{								\
		.h_dest		= { 0 },				\
		.h_source	= { 0 },				\
		.h_proto	= ETH_P_IPV6,				\
	}
#else
#define L2_BUF_ETH_IP6_INIT						\
	{								\
		.h_dest		= { 0 },				\
		.h_source	= { 0 },				\
		.h_proto	= __bswap_constant_16(ETH_P_IPV6),	\
	}
#endif

#define L2_BUF_IP4_INIT(proto)						\
	{								\
		.version	= 4,					\
		.ihl		= 5,					\
		.tos		= 0,					\
		.tot_len	= 0,					\
		.id		= 0,					\
		.frag_off	= 0,					\
		.ttl		= 255,					\
		.protocol	= (proto),				\
		.saddr		= 0,					\
		.daddr		= 0,					\
	}

#define L2_BUF_IP6_INIT(proto)						\
	{								\
		.priority	= 0,					\
		.version	= 6,					\
		.flow_lbl	= { 0 },				\
		.payload_len	= 0,					\
		.nexthdr	= (proto),				\
		.hop_limit	= 255,					\
		.saddr		= IN6ADDR_ANY_INIT,			\
		.daddr		= IN6ADDR_ANY_INIT,			\
	}

#define RCVBUF_BIG		(2UL * 1024 * 1024)
#define SNDBUF_BIG		(4UL * 1024 * 1024)
#define SNDBUF_SMALL		(128UL * 1024)

#include <net/if.h>
#include <limits.h>
#include <stdarg.h>

#include "packet.h"

struct ctx;

struct ipv6hdr {
#pragma GCC diagnostic ignored "-Wpedantic"
#if __BYTE_ORDER == __BIG_ENDIAN
	uint8_t			version:4,
				priority:4;
#else
	uint8_t			priority:4,
				version:4;
#endif
#pragma GCC diagnostic pop
	uint8_t			flow_lbl[3];

	uint16_t		payload_len;
	uint8_t			nexthdr;
	uint8_t			hop_limit;

	struct in6_addr		saddr;
	struct in6_addr		daddr;
};

struct ipv6_opt_hdr {
	uint8_t			nexthdr;
	uint8_t			hdrlen;
	/*
	 * TLV encoded option data follows.
	 */
} __attribute__((packed));	/* required for some archs */

__attribute__ ((weak)) int ffsl(long int i) { return __builtin_ffsl(i); }
void __openlog(const char *ident, int option, int facility);
void passt_vsyslog(int pri, const char *format, va_list ap);
void __setlogmask(int mask);
char *ipv6_l4hdr(const struct pool *p, int index, size_t offset, uint8_t *proto,
		 size_t *dlen);
int sock_l4(const struct ctx *c, int af, uint8_t proto,
	    const void *bind_addr, uint16_t port, uint32_t data);
void sock_probe_mem(struct ctx *c);
int timespec_diff_ms(const struct timespec *a, const struct timespec *b);
void bitmap_set(uint8_t *map, int bit);
void bitmap_clear(uint8_t *map, int bit);
int bitmap_isset(const uint8_t *map, int bit);
char *line_read(char *buf, size_t len, int fd);
void procfs_scan_listen(struct ctx *c, uint8_t proto, int ip_version, int ns,
			uint8_t *map, uint8_t *exclude);
void drop_caps(void);
void check_root(struct ctx *c);
int ns_enter(const struct ctx *c);
void write_pidfile(int fd, pid_t pid);
int __daemon(int pidfile_fd, int devnull_fd);
int fls(unsigned long x);

#endif /* UTIL_H */
