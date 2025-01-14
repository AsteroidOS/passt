// SPDX-License-Identifier: GPL-2.0-or-later

/* PASST - Plug A Simple Socket Transport
 *  for qemu/UNIX domain socket mode
 *
 * PASTA - Pack A Subtle Tap Abstraction
 *  for network namespace/tap device mode
 *
 * util.c - Convenience helpers
 *
 * Copyright (c) 2020-2021 Red Hat GmbH
 * Author: Stefano Brivio <sbrivio@redhat.com>
 */

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/ethernet.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdbool.h>

#include "util.h"
#include "iov.h"
#include "passt.h"
#include "packet.h"
#include "log.h"

/**
 * sock_l4() - Create and bind socket for given L4, add to epoll list
 * @c:		Execution context
 * @af:		Address family, AF_INET or AF_INET6
 * @proto:	Protocol number
 * @bind_addr:	Address for binding, NULL for any
 * @ifname:	Interface for binding, NULL for any
 * @port:	Port, host order
 * @data:	epoll reference portion for protocol handlers
 *
 * Return: newly created socket, negative error code on failure
 */
int sock_l4(const struct ctx *c, sa_family_t af, uint8_t proto,
	    const void *bind_addr, const char *ifname, uint16_t port,
	    uint32_t data)
{
	union epoll_ref ref = { .data = data };
	struct sockaddr_in addr4 = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
		{ 0 }, { 0 },
	};
	struct sockaddr_in6 addr6 = {
		.sin6_family = AF_INET6,
		.sin6_port = htons(port),
		0, IN6ADDR_ANY_INIT, 0,
	};
	const struct sockaddr *sa;
	bool dual_stack = false;
	int fd, sl, y = 1, ret;
	struct epoll_event ev;

	switch (proto) {
	case IPPROTO_TCP:
		ref.type = EPOLL_TYPE_TCP_LISTEN;
		break;
	case IPPROTO_UDP:
		ref.type = EPOLL_TYPE_UDP;
		break;
	case IPPROTO_ICMP:
	case IPPROTO_ICMPV6:
		ref.type = EPOLL_TYPE_PING;
		break;
	default:
		return -EPFNOSUPPORT;	/* Not implemented. */
	}

	if (af == AF_UNSPEC) {
		if (!DUAL_STACK_SOCKETS || bind_addr)
			return -EINVAL;
		dual_stack = true;
		af = AF_INET6;
	}

	if (proto == IPPROTO_TCP)
		fd = socket(af, SOCK_STREAM | SOCK_NONBLOCK, proto);
	else
		fd = socket(af, SOCK_DGRAM | SOCK_NONBLOCK, proto);

	ret = -errno;
	if (fd < 0) {
		warn("L4 socket: %s", strerror(-ret));
		return ret;
	}

	if (fd > FD_REF_MAX) {
		close(fd);
		return -EBADF;
	}

	ref.fd = fd;

	if (af == AF_INET) {
		if (bind_addr)
			addr4.sin_addr = *(struct in_addr *)bind_addr;

		sa = (const struct sockaddr *)&addr4;
		sl = sizeof(addr4);
	} else {
		if (bind_addr) {
			addr6.sin6_addr = *(struct in6_addr *)bind_addr;

			if (!memcmp(bind_addr, &c->ip6.addr_ll,
			    sizeof(c->ip6.addr_ll)))
				addr6.sin6_scope_id = c->ifi6;
		}

		sa = (const struct sockaddr *)&addr6;
		sl = sizeof(addr6);

		if (!dual_stack)
			if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
				       &y, sizeof(y)))
				debug("Failed to set IPV6_V6ONLY on socket %i",
				      fd);
	}

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &y, sizeof(y)))
		debug("Failed to set SO_REUSEADDR on socket %i", fd);

	if (ifname && *ifname) {
		/* Supported since kernel version 5.7, commit c427bfec18f2
		 * ("net: core: enable SO_BINDTODEVICE for non-root users"). If
		 * it's unsupported, don't bind the socket at all, because the
		 * user might rely on this to filter incoming connections.
		 */
		if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE,
			       ifname, strlen(ifname))) {
			ret = -errno;
			warn("Can't bind %s socket for port %u to %s, closing",
			     EPOLL_TYPE_STR(proto), port, ifname);
			close(fd);
			return ret;
		}
	}

	if (bind(fd, sa, sl) < 0) {
		/* We'll fail to bind to low ports if we don't have enough
		 * capabilities, and we'll fail to bind on already bound ports,
		 * this is fine. This might also fail for ICMP because of a
		 * broken SELinux policy, see icmp_tap_handler().
		 */
		if (proto != IPPROTO_ICMP && proto != IPPROTO_ICMPV6) {
			ret = -errno;
			close(fd);
			return ret;
		}
	}

	if (proto == IPPROTO_TCP && listen(fd, 128) < 0) {
		ret = -errno;
		warn("TCP socket listen: %s", strerror(-ret));
		close(fd);
		return ret;
	}

	ev.events = EPOLLIN;
	ev.data.u64 = ref.u64;
	if (epoll_ctl(c->epollfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
		ret = -errno;
		warn("L4 epoll_ctl: %s", strerror(-ret));
		return ret;
	}

	return fd;
}

/**
 * sock_probe_mem() - Check if setting high SO_SNDBUF and SO_RCVBUF is allowed
 * @c:		Execution context
 */
void sock_probe_mem(struct ctx *c)
{
	int v = INT_MAX / 2, s;
	socklen_t sl;

	if ((s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
		c->low_wmem = c->low_rmem = 1;
		return;
	}

	sl = sizeof(v);
	if (setsockopt(s, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v))	||
	    getsockopt(s, SOL_SOCKET, SO_SNDBUF, &v, &sl) ||
	    (size_t)v < SNDBUF_BIG)
		c->low_wmem = 1;

	v = INT_MAX / 2;
	if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v))	||
	    getsockopt(s, SOL_SOCKET, SO_RCVBUF, &v, &sl) ||
	    (size_t)v < RCVBUF_BIG)
		c->low_rmem = 1;

	close(s);
}


/**
 * timespec_diff_ms() - Report difference in milliseconds between two timestamps
 * @a:		Minuend timestamp
 * @b:		Subtrahend timestamp
 *
 * Return: difference in milliseconds
 */
int timespec_diff_ms(const struct timespec *a, const struct timespec *b)
{
	if (a->tv_nsec < b->tv_nsec) {
		return (b->tv_nsec - a->tv_nsec) / 1000000 +
		       (a->tv_sec - b->tv_sec - 1) * 1000;
	}

	return (a->tv_nsec - b->tv_nsec) / 1000000 +
	       (a->tv_sec - b->tv_sec) * 1000;
}

/**
 * bitmap_set() - Set single bit in bitmap
 * @map:	Pointer to bitmap
 * @bit:	Bit number to set
 */
void bitmap_set(uint8_t *map, int bit)
{
	unsigned long *word = (unsigned long *)map + BITMAP_WORD(bit);

	*word |= BITMAP_BIT(bit);
}

/**
 * bitmap_clear() - Clear single bit in bitmap
 * @map:	Pointer to bitmap
 * @bit:	Bit number to clear
 */
void bitmap_clear(uint8_t *map, int bit)
{
	unsigned long *word = (unsigned long *)map + BITMAP_WORD(bit);

	*word &= ~BITMAP_BIT(bit);
}

/**
 * bitmap_isset() - Check for set bit in bitmap
 * @map:	Pointer to bitmap
 * @bit:	Bit number to check
 *
 * Return: one if given bit is set, zero if it's not
 */
int bitmap_isset(const uint8_t *map, int bit)
{
	const unsigned long *word
		= (const unsigned long *)map + BITMAP_WORD(bit);

	return !!(*word & BITMAP_BIT(bit));
}

/**
 * bitmap_or() - Logical disjunction (OR) of two bitmaps
 * @dst:	Pointer to result bitmap
 * @size:	Size of bitmaps, in bytes
 * @a:		First operand
 * @b:		Second operand
 */
void bitmap_or(uint8_t *dst, size_t size, const uint8_t *a, const uint8_t *b)
{
	unsigned long *dw = (unsigned long *)dst;
	unsigned long *aw = (unsigned long *)a;
	unsigned long *bw = (unsigned long *)b;
	size_t i;

	for (i = 0; i < size / sizeof(long); i++, dw++, aw++, bw++)
		*dw = *aw | *bw;

	for (i = size / sizeof(long) * sizeof(long); i < size; i++)
		dst[i] = a[i] | b[i];
}

/*
 * ns_enter() - Enter configured user (unless already joined) and network ns
 * @c:		Execution context
 *
 * Won't return on failure
 *
 * #syscalls:pasta setns
 */
void ns_enter(const struct ctx *c)
{
	if (setns(c->pasta_netns_fd, CLONE_NEWNET))
		die("setns() failed entering netns: %s", strerror(errno));
}

/**
 * ns_is_init() - Is the caller running in the "init" user namespace?
 *
 * Return: true if caller is in init, false otherwise, won't return on failure
 */
bool ns_is_init(void)
{
	const char root_uid_map[] = "         0          0 4294967295\n";
	char buf[sizeof(root_uid_map)] = { 0 };
	bool ret = true;
	int fd;

	if ((fd = open("/proc/self/uid_map", O_RDONLY | O_CLOEXEC)) < 0) {
		die("Can't determine if we're in init namespace: %s",
		    strerror(errno));
	}

	if (read(fd, buf, sizeof(root_uid_map)) != sizeof(root_uid_map) - 1 ||
	    strncmp(buf, root_uid_map, sizeof(root_uid_map)))
		ret = false;

	close(fd);
	return ret;
}

/**
 * struct open_in_ns_args - Parameters for do_open_in_ns()
 * @c:		Execution context
 * @fd:		Filled in with return value from open()
 * @err:	Filled in with errno if open() failed
 * @path:	Path to open
 * @flags:	open() flags
 */
struct open_in_ns_args {
	const struct ctx *c;
	int fd;
	int err;
	const char *path;
	int flags;
};

/**
 * do_open_in_ns() - Enter namespace and open a file
 * @arg:	See struct open_in_ns_args
 *
 * Must be called via NS_CALL()
 */
static int do_open_in_ns(void *arg)
{
	struct open_in_ns_args *a = (struct open_in_ns_args *)arg;

	ns_enter(a->c);

	a->fd = open(a->path, a->flags);
	a->err = errno;

	return 0;
}

/**
 * open_in_ns() - open() within the pasta namespace
 * @c:		Execution context
 * @path:	Path to open
 * @flags:	open() flags
 *
 * Return: fd of open()ed file or -1 on error, errno is set to indicate error
 */
int open_in_ns(const struct ctx *c, const char *path, int flags)
{
	struct open_in_ns_args arg = {
		.c = c, .path = path, .flags = flags,
	};

	NS_CALL(do_open_in_ns, &arg);
	errno = arg.err;
	return arg.fd;
}

/**
 * pid_file() - Write PID to file, if requested to do so, and close it
 * @fd:		Open PID file descriptor, closed on exit, -1 to skip writing it
 * @pid:	PID value to write
 */
void write_pidfile(int fd, pid_t pid)
{
	char pid_buf[12];
	int n;

	if (fd == -1)
		return;

	n = snprintf(pid_buf, sizeof(pid_buf), "%i\n", pid);

	if (write(fd, pid_buf, n) < 0) {
		perror("PID file write");
		exit(EXIT_FAILURE);
	}

	close(fd);
}

/**
 * __daemon() - daemon()-like function writing PID file before parent exits
 * @pidfile_fd:	Open PID file descriptor
 * @devnull_fd:	Open file descriptor for /dev/null
 *
 * Return: child PID on success, won't return on failure
 */
int __daemon(int pidfile_fd, int devnull_fd)
{
	pid_t pid = fork();

	if (pid == -1) {
		perror("fork");
		exit(EXIT_FAILURE);
	}

	if (pid) {
		write_pidfile(pidfile_fd, pid);
		exit(EXIT_SUCCESS);
	}

	errno = 0;

	setsid();

	dup2(devnull_fd, STDIN_FILENO);
	dup2(devnull_fd, STDOUT_FILENO);
	dup2(devnull_fd, STDERR_FILENO);
	close(devnull_fd);

	if (errno)
		exit(EXIT_FAILURE);

	return 0;
}

/**
 * fls() - Find last (most significant) bit set in word
 * @x:		Word
 *
 * Return: position of most significant bit set, starting from 0, -1 if none
 */
int fls(unsigned long x)
{
	int y = 0;

	if (!x)
		return -1;

	while (x >>= 1)
		y++;

	return y;
}

/**
 * write_file() - Replace contents of file with a string
 * @path:	File to write
 * @buf:	String to write
 *
 * Return: 0 on success, -1 on any error
 */
int write_file(const char *path, const char *buf)
{
	int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
	size_t len = strlen(buf);

	if (fd < 0) {
		warn("Could not open %s: %s", path, strerror(errno));
		return -1;
	}

	while (len) {
		ssize_t rc = write(fd, buf, len);

		if (rc <= 0) {
			warn("Couldn't write to %s: %s", path, strerror(errno));
			break;
		}

		buf += rc;
		len -= rc;
	}

	close(fd);
	return len == 0 ? 0 : -1;
}

#ifdef __ia64__
/* Needed by do_clone() below: glibc doesn't export the prototype of __clone2(),
 * use the description from clone(2).
 */
int __clone2(int (*fn)(void *), void *stack_base, size_t stack_size, int flags,
	     void *arg, ... /* pid_t *parent_tid, struct user_desc *tls,
	     pid_t *child_tid */ );
#endif

/**
 * do_clone() - Wrapper of __clone2() for ia64, clone() for other architectures
 * @fn:		Entry point for child
 * @stack_area:	Stack area for child: we'll point callees to the middle of it
 * @stack_size:	Total size of stack area, passed to callee divided by two
 * @flags:	clone() system call flags
 * @arg:	Argument to @fn
 *
 * Return: thread ID of child, -1 on failure
 */
int do_clone(int (*fn)(void *), char *stack_area, size_t stack_size, int flags,
	     void *arg)
{
#ifdef __ia64__
	return __clone2(fn, stack_area + stack_size / 2, stack_size / 2,
			flags, arg);
#else
	return clone(fn, stack_area + stack_size / 2, flags, arg);
#endif
}

/* write_remainder() - write the tail of an IO vector to an fd
 * @fd:		File descriptor
 * @iov:	IO vector
 * @iovcnt:	Number of entries in @iov
 * @skip:	Number of bytes of the vector to skip writing
 *
 * Return: 0 on success, -1 on error (with errno set)
 *
 * #syscalls write writev
 */
int write_remainder(int fd, const struct iovec *iov, int iovcnt, size_t skip)
{
	int i;
	size_t offset;

	while ((i = iov_skip_bytes(iov, iovcnt, skip, &offset)) < iovcnt) {
		ssize_t rc;

		if (offset) {
			rc = write(fd, (char *)iov[i].iov_base + offset,
				   iov[i].iov_len - offset);
		} else {
			rc = writev(fd, &iov[i], iovcnt - i);
		}

		if (rc < 0)
			return -1;

		skip += rc;
	}

	return 0;
}
