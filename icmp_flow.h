/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright Red Hat
 * Author: David Gibson <david@gibson.dropbear.id.au>
 *
 * ICMP flow tracking data structures
 */
#ifndef ICMP_FLOW_H
#define ICMP_FLOW_H

/**
 * struct icmp_ping_flow - Descriptor for a flow of ping requests/replies
 * @f:		Generic flow information
 * @seq:	Last sequence number sent to tap, host order, -1: not sent yet
 * @sock:	"ping" socket
 * @ts:		Last associated activity from tap, seconds
 * @id:		ICMP id for the flow as seen by the guest
 */
struct icmp_ping_flow {
	/* Must be first element */
	struct flow_common f;

	int seq;
	int sock;
	time_t ts;
	uint16_t id;
};

bool icmp_ping_timer(const struct ctx *c, union flow *flow,
		     const struct timespec *now);

#endif /* ICMP_FLOW_H */
