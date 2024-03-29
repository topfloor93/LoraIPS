/* Copyright (C) 2007-2010 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Victor Julien <victor@inliniac.net>
 */

#ifndef __FLOW_PRIVATE_H__
#define __FLOW_PRIVATE_H__

#include "flow-hash.h"
#include "flow-queue.h"

#include "util-atomic.h"

/* global flow flags */

/** Flow engine is in emergency mode. This means it doesn't have enough spare
 *  flows for new flows and/or it's memcap limit it reached. In this state the
 *  flow engine with evaluate flows with lower timeout settings. */
#define FLOW_EMERGENCY   0x01

/* Flow Time out values */
#define FLOW_DEFAULT_NEW_TIMEOUT 30
#define FLOW_DEFAULT_EST_TIMEOUT 300
#define FLOW_DEFAULT_CLOSED_TIMEOUT 0
#define FLOW_IPPROTO_TCP_NEW_TIMEOUT 30
#define FLOW_IPPROTO_TCP_EST_TIMEOUT 300
#define FLOW_IPPROTO_UDP_NEW_TIMEOUT 30
#define FLOW_IPPROTO_UDP_EST_TIMEOUT 300
#define FLOW_IPPROTO_ICMP_NEW_TIMEOUT 30
#define FLOW_IPPROTO_ICMP_EST_TIMEOUT 300

#define FLOW_DEFAULT_EMERG_NEW_TIMEOUT 10
#define FLOW_DEFAULT_EMERG_EST_TIMEOUT 100
#define FLOW_DEFAULT_EMERG_CLOSED_TIMEOUT 0
#define FLOW_IPPROTO_TCP_EMERG_NEW_TIMEOUT 10
#define FLOW_IPPROTO_TCP_EMERG_EST_TIMEOUT 100
#define FLOW_IPPROTO_UDP_EMERG_NEW_TIMEOUT 10
#define FLOW_IPPROTO_UDP_EMERG_EST_TIMEOUT 100
#define FLOW_IPPROTO_ICMP_EMERG_NEW_TIMEOUT 10
#define FLOW_IPPROTO_ICMP_EMERG_EST_TIMEOUT 100

enum {
    FLOW_PROTO_DEFAULT = 0,
    FLOW_PROTO_TCP,
    FLOW_PROTO_UDP,
    FLOW_PROTO_ICMP,

    /* should be last */
    FLOW_PROTO_MAX,
};

/*
 * Variables
 */

/** FlowProto specific timeouts and free/state functions */
FlowProto flow_proto[FLOW_PROTO_MAX];

/** spare/unused/prealloced flows live here */
FlowQueue flow_spare_q;

/** Flows in the new/unreplied state live here */
FlowQueue flow_new_q[FLOW_PROTO_MAX];

/** All "established" flows live here, the top holds the
 *  last recently used (lru) flow, so we can remove
 *  that in case of memory problems and check it for
 *  timeouts. */
FlowQueue flow_est_q[FLOW_PROTO_MAX];

/** All "closing" flows live here, the top holds the
 *  last recently used (lru) flow, so we can remove
 *  that in case of memory problems and check it for
 *  timeouts. */
FlowQueue flow_close_q[FLOW_PROTO_MAX];

FlowBucket *flow_hash;
FlowConfig flow_config;

uint8_t flow_flags;

/** flow memuse counter (atomic), for enforcing memcap limit */
SC_ATOMIC_DECLARE(unsigned int, flow_memuse);

//#define FLOWBITS_STATS
#ifdef FLOWBITS_STATS
uint32_t flowbits_memuse;
uint32_t flowbits_memuse_max;
uint32_t flowbits_added;
uint32_t flowbits_removed;
SCMutex flowbits_mutex;
#endif /* FLOWBITS_STATS */

#endif /* __FLOW_PRIVATE_H__ */

