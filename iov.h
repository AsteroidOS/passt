// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * iov.c - helpers for using (partial) iovecs.
 *
 * Copyrigh Red Hat
 * Author: Laurent Vivier <lvivier@redhat.com>
 *
 * This file also contains code originally from QEMU include/qemu/iov.h:
 *
 * Author(s):
 *  Amit Shah <amit.shah@redhat.com>
 *  Michael Tokarev <mjt@tls.msk.ru>
 */

#ifndef IOVEC_H
#define IOVEC_H

#include <unistd.h>
#include <string.h>

size_t iov_skip_bytes(const struct iovec *iov, size_t n,
		      size_t skip, size_t *offset);
size_t iov_from_buf(const struct iovec *iov, size_t iov_cnt,
                    size_t offset, const void *buf, size_t bytes);
size_t iov_to_buf(const struct iovec *iov, size_t iov_cnt,
                  size_t offset, void *buf, size_t bytes);
size_t iov_size(const struct iovec *iov, size_t iov_cnt);
unsigned iov_copy(struct iovec *dst_iov, size_t dst_iov_cnt,
		  const struct iovec *iov, size_t iov_cnt,
		  size_t offset, size_t bytes);
#endif /* IOVEC_H */
