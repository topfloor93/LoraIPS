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

#ifndef __DECODE_H__
#define __DECODE_H__

//#define IPQ
//#define NFQ
//#define IPFW
//#define PCAP

//#define DEBUG
#define DBG_PERF
//#define DBG_THREADS
#define COUNTERS

#include "threadvars.h"

#include "source-nfq.h"

#include "source-pcap.h"
#include "action-globals.h"

#include "decode-ethernet.h"
#include "decode-gre.h"
#include "decode-ppp.h"
#include "decode-pppoe.h"
#include "decode-sll.h"
#include "decode-ipv4.h"
#include "decode-ipv6.h"
#include "decode-icmpv4.h"
#include "decode-icmpv6.h"
#include "decode-tcp.h"
#include "decode-udp.h"
#include "decode-raw.h"
#include "decode-vlan.h"

#include "detect-reference.h"

#ifdef __SC_CUDA_SUPPORT__
#define CUDA_MAX_PAYLOAD_SIZE 1500
#endif

/* forward declaration */
struct DetectionEngineThreadCtx_;

/* Address */
typedef struct Address_ {
    char family;
    union {
        uint32_t       address_un_data32[4]; /* type-specific field */
        uint16_t       address_un_data16[8]; /* type-specific field */
        uint8_t        address_un_data8[16]; /* type-specific field */
    } address;
} Address;

#define addr_data32 address.address_un_data32
#define addr_data16 address.address_un_data16
#define addr_data8  address.address_un_data8

#define COPY_ADDRESS(a, b) do {                    \
        (b)->family = (a)->family;                 \
        (b)->addr_data32[0] = (a)->addr_data32[0]; \
        (b)->addr_data32[1] = (a)->addr_data32[1]; \
        (b)->addr_data32[2] = (a)->addr_data32[2]; \
        (b)->addr_data32[3] = (a)->addr_data32[3]; \
    } while (0)

/* Set the IPv4 addressesinto the Addrs of the Packet.
 * Make sure p->ip4h is initialized and validated.
 *
 * We set the rest of the struct to 0 so we can
 * prevent using memset. */
#define SET_IPV4_SRC_ADDR(p, a) do {                              \
        (a)->family = AF_INET;                                    \
        (a)->addr_data32[0] = (uint32_t)(p)->ip4h->ip_src.s_addr; \
        (a)->addr_data32[1] = 0;                                  \
        (a)->addr_data32[2] = 0;                                  \
        (a)->addr_data32[3] = 0;                                  \
    } while (0)

#define SET_IPV4_DST_ADDR(p, a) do {                              \
        (a)->family = AF_INET;                                    \
        (a)->addr_data32[0] = (uint32_t)(p)->ip4h->ip_dst.s_addr; \
        (a)->addr_data32[1] = 0;                                  \
        (a)->addr_data32[2] = 0;                                  \
        (a)->addr_data32[3] = 0;                                  \
    } while (0)

/* clear the address structure by setting all fields to 0 */
#define CLEAR_ADDR(a) do {       \
        (a)->family = 0;         \
        (a)->addr_data32[0] = 0; \
        (a)->addr_data32[1] = 0; \
        (a)->addr_data32[2] = 0; \
        (a)->addr_data32[3] = 0; \
    } while (0)

/* Set the IPv6 addressesinto the Addrs of the Packet.
 * Make sure p->ip6h is initialized and validated. */
#define SET_IPV6_SRC_ADDR(p, a) do {                 \
        (a)->family = AF_INET6;                      \
        (a)->addr_data32[0] = (p)->ip6h->ip6_src[0]; \
        (a)->addr_data32[1] = (p)->ip6h->ip6_src[1]; \
        (a)->addr_data32[2] = (p)->ip6h->ip6_src[2]; \
        (a)->addr_data32[3] = (p)->ip6h->ip6_src[3]; \
    } while (0)

#define SET_IPV6_DST_ADDR(p, a) do {                 \
        (a)->family = AF_INET6;                      \
        (a)->addr_data32[0] = (p)->ip6h->ip6_dst[0]; \
        (a)->addr_data32[1] = (p)->ip6h->ip6_dst[1]; \
        (a)->addr_data32[2] = (p)->ip6h->ip6_dst[2]; \
        (a)->addr_data32[3] = (p)->ip6h->ip6_dst[3]; \
    } while (0)

/* Set the TCP ports into the Ports of the Packet.
 * Make sure p->tcph is initialized and validated. */
#define SET_TCP_SRC_PORT(pkt, prt) do {          \
        SET_PORT(TCP_GET_SRC_PORT((pkt)), *prt); \
    } while (0)

#define SET_TCP_DST_PORT(pkt, prt) do {          \
        SET_PORT(TCP_GET_DST_PORT((pkt)), *prt); \
    } while (0)

/* Set the UDP ports into the Ports of the Packet.
 * Make sure p->udph is initialized and validated. */
#define SET_UDP_SRC_PORT(pkt, prt) do {          \
        SET_PORT(UDP_GET_SRC_PORT((pkt)), *prt); \
    } while (0)
#define SET_UDP_DST_PORT(pkt, prt) do {          \
        SET_PORT(UDP_GET_DST_PORT((pkt)), *prt); \
    } while (0)

#define GET_IPV4_SRC_ADDR_U32(p) ((p)->src.addr_data32[0])
#define GET_IPV4_DST_ADDR_U32(p) ((p)->dst.addr_data32[0])
#define GET_IPV4_SRC_ADDR_PTR(p) ((p)->src.addr_data32)
#define GET_IPV4_DST_ADDR_PTR(p) ((p)->dst.addr_data32)

#define GET_IPV6_SRC_ADDR(p) ((p)->src.addr_data32)
#define GET_IPV6_DST_ADDR(p) ((p)->dst.addr_data32)
#define GET_TCP_SRC_PORT(p)  ((p)->sp)
#define GET_TCP_DST_PORT(p)  ((p)->dp)

/* Port is just a uint16_t */
typedef uint16_t Port;
#define SET_PORT(v, p) ((p) = (v))
#define COPY_PORT(a,b) (b) = (a)

#define CMP_ADDR(a1, a2) \
    (((a1)->addr_data32[3] == (a2)->addr_data32[3] && \
      (a1)->addr_data32[2] == (a2)->addr_data32[2] && \
      (a1)->addr_data32[1] == (a2)->addr_data32[1] && \
      (a1)->addr_data32[0] == (a2)->addr_data32[0]))
#define CMP_PORT(p1, p2) \
    ((p1 == p2))

/*Given a packet pkt offset to the start of the ip header in a packet
 *We determine the ip version. */
#define IP_GET_RAW_VER(pkt) (((pkt[0] & 0xf0) >> 4))

#define PKT_IS_IPV4(p)      (((p)->ip4h != NULL))
#define PKT_IS_IPV6(p)      (((p)->ip6h != NULL))
#define PKT_IS_TCP(p)       (((p)->tcph != NULL))
#define PKT_IS_UDP(p)       (((p)->udph != NULL))
#define PKT_IS_ICMPV4(p)    (((p)->icmpv4h != NULL))
#define PKT_IS_ICMPV6(p)    (((p)->icmpv6h != NULL))
#define PKT_IS_TOSERVER(p)  (((p)->flowflags & FLOW_PKT_TOSERVER))
#define PKT_IS_TOCLIENT(p)  (((p)->flowflags & FLOW_PKT_TOCLIENT))

#define IPH_IS_VALID(p) (PKT_IS_IPV4((p)) || PKT_IS_IPV6((p)))

/* structure to store the sids/gids/etc the detection engine
 * found in this packet */
typedef struct PacketAlert_ {
    SigIntId num; /* Internal num, used for sorting */
    SigIntId order_id; /* Internal num, used for sorting */
    uint8_t action; /* Internal num, used for sorting */
    uint32_t  gid;
    uint32_t sid;
    uint8_t  rev;
    uint8_t class;
    uint8_t prio;
    char *msg;
    char *class_msg;
    Reference *references;
} PacketAlert;

#define PACKET_ALERT_MAX 256

typedef struct PacketAlerts_ {
    uint16_t cnt;
    PacketAlert alerts[PACKET_ALERT_MAX];
} PacketAlerts;

#define PACKET_DECODER_EVENT_MAX 16

typedef struct PacketDecoderEvents_ {
    uint8_t cnt;
    uint8_t events[PACKET_DECODER_EVENT_MAX];
} PacketDecoderEvents;

typedef struct PktVar_ {
    char *name;
    struct PktVar_ *next; /* right now just implement this as a list,
                           * in the long run we have thing of something
                           * faster. */
    uint8_t *value;
    uint16_t value_len;
} PktVar;

/* forward declartion since Packet struct definition requires this */
struct PacketQueue_;

/* sizes of the members:
 * src: 17 bytes
 * dst: 17 bytes
 * sp/type: 1 byte
 * dp/code: 1 byte
 * proto: 1 byte
 * recurs: 1 byte
 *
 * sum of above: 38 bytes
 *
 * flow ptr: 4/8 bytes
 * flags: 1 byte
 * flowflags: 1 byte
 *
 * sum of above 44/48 bytes
 */
typedef struct Packet_
{
    /* Addresses, Ports and protocol
     * these are on top so we can use
     * the Packet as a hash key */
    Address src;
    Address dst;
    union {
        Port sp;
        uint8_t type;
    };
    union {
        Port dp;
        uint8_t code;
    };
    uint8_t proto;
    /* make sure we can't be attacked on when the tunneled packet
     * has the exact same tuple as the lower levels */
    uint8_t recursion_level;

    /* Pkt Flags */
    uint8_t flags;
    /* flow */
    uint8_t flowflags;
    struct Flow_ *flow;

    struct timeval ts;

    union {
        /* nfq stuff */
#ifdef NFQ
        NFQPacketVars nfq_v;
#endif /* NFQ */

        /** libpcap vars: shared by Pcap Live mode and Pcap File mode */
        PcapPacketVars pcap_v;
    };

    /** data linktype in host order */
    int datalink;

    /* IPS action to take */
    uint8_t action;

    /* pkt vars */
    PktVar *pktvar;

    /* header pointers */
    EthernetHdr *ethh;

    IPV4Hdr *ip4h;
    IPV4Vars ip4vars;
    IPV4Cache ip4c;

    IPV6Hdr *ip6h;
    IPV6Vars ip6vars;
    IPV6Cache ip6c;
    IPV6ExtHdrs ip6eh;

    TCPHdr *tcph;
    TCPVars tcpvars;
    TCPCache tcpc;

    UDPHdr *udph;
    UDPVars udpvars;
    UDPCache udpc;

    ICMPV4Hdr *icmpv4h;
    ICMPV4Cache icmpv4c;
    ICMPV4Vars icmpv4vars;

    ICMPV6Hdr *icmpv6h;
    ICMPV6Cache icmpv6c;
    ICMPV6Vars icmpv6vars;

    PPPHdr *ppph;
    PPPOESessionHdr *pppoesh;
    PPPOEDiscoveryHdr *pppoedh;

    GREHdr *greh;

    VLANHdr *vlanh;

    /* ptr to the payload of the packet
     * with it's length. */
    uint8_t *payload;
    uint16_t payload_len;

    /* storage: maximum ip packet size + link header */
    uint8_t pkt[IPV6_HEADER_LEN + 65536 + 28];
    uint32_t pktlen;

    PacketAlerts alerts;

    /** packet number in the pcap file, matches wireshark */
    uint64_t pcap_cnt;

    /* ready to set verdict counter, only set in root */
    uint8_t rtv_cnt;
    /* tunnel packet ref count */
    uint8_t tpr_cnt;
    SCMutex mutex_rtv_cnt;
    /* tunnel stuff */
    uint8_t tunnel_proto;
    /* tunnel XXX convert to bitfield*/
    char tunnel_pkt;
    char tunnel_verdicted;

    /* decoder events */
    PacketDecoderEvents events;

    /* double linked list ptrs */
    struct Packet_ *next;
    struct Packet_ *prev;

    /* tunnel/encapsulation handling */
    struct Packet_ *root; /* in case of tunnel this is a ptr
                           * to the 'real' packet, the one we
                           * need to set the verdict on --
                           * It should always point to the lowest
                           * packet in a encapsulated packet */

    /* required for cuda support */
#ifdef __SC_CUDA_SUPPORT__
    /* indicates if the cuda mpm would be conducted or a normal cpu mpm would
     * be conduced on this packet.  If it is set to 0, the cpu mpm; else cuda mpm */
    uint8_t cuda_mpm_enabled;
    /* indicates if the cuda mpm has finished running the mpm and processed the
     * results for this packet, assuming if cuda_mpm_enabled has been set for this
     * packet */
    uint16_t cuda_done;
    /* used by the detect thread and the cuda mpm dispatcher thread.  The detect
     * thread would wait on this cond var, if the cuda mpm dispatcher thread
     * still hasn't processed the packet.  The dispatcher would use this cond
     * to inform the detect thread(in case it is waiting on this packet), once
     * the dispatcher is done processing the packet results */
    SCMutex cuda_mutex;
    SCCondT cuda_cond;
    /* the extra 1 in the 1481, is to hold the no_of_matches from the mpm run */
    uint16_t mpm_offsets[CUDA_MAX_PAYLOAD_SIZE + 1];
#endif
} Packet;

typedef struct PacketQueue_ {
    Packet *top;
    Packet *bot;
    uint16_t len;
    SCMutex mutex_q;
    SCCondT cond_q;
#ifdef DBG_PERF
    uint16_t dbg_maxlen;
#endif /* DBG_PERF */
} PacketQueue;

/** \brief Specific ctx for AL proto detection */
typedef struct AlpProtoDetectDirectionThread_ {
    MpmThreadCtx mpm_ctx;
    PatternMatcherQueue pmq;
} AlpProtoDetectDirectionThread;

/** \brief Specific ctx for AL proto detection */
typedef struct AlpProtoDetectThreadCtx_ {
    AlpProtoDetectDirectionThread toserver;
    AlpProtoDetectDirectionThread toclient;
} AlpProtoDetectThreadCtx;

/** \brief Structure to hold thread specific data for all decode modules */
typedef struct DecodeThreadVars_
{
    /** Specific context for udp protocol detection (here atm) */
    AlpProtoDetectThreadCtx udp_dp_ctx;

    /** stats/counters */
    uint16_t counter_pkts;
    uint16_t counter_pkts_per_sec;
    uint16_t counter_bytes;
    uint16_t counter_bytes_per_sec;
    uint16_t counter_mbit_per_sec;
    uint16_t counter_ipv4;
    uint16_t counter_ipv6;
    uint16_t counter_eth;
    uint16_t counter_sll;
    uint16_t counter_raw;
    uint16_t counter_tcp;
    uint16_t counter_udp;
    uint16_t counter_icmpv4;
    uint16_t counter_icmpv6;
    uint16_t counter_ppp;
    uint16_t counter_gre;
    uint16_t counter_vlan;
    uint16_t counter_pppoe;
    uint16_t counter_avg_pkt_size;
    uint16_t counter_max_pkt_size;

    /** frag stats - defrag runs in the context of the decoder. */
    uint16_t counter_defrag_ipv4_fragments;
    uint16_t counter_defrag_ipv4_reassembled;
    uint16_t counter_defrag_ipv4_timeouts;
    uint16_t counter_defrag_ipv6_fragments;
    uint16_t counter_defrag_ipv6_reassembled;
    uint16_t counter_defrag_ipv6_timeouts;
} DecodeThreadVars;

/**
 *  \brief reset these to -1(indicates that the packet is fresh from the queue)
 */
#define PACKET_RESET_CHECKSUMS(p) do { \
        (p)->ip4c.comp_csum = -1;      \
        (p)->tcpc.comp_csum = -1;      \
        (p)->udpc.comp_csum = -1;      \
        (p)->icmpv4c.comp_csum = -1;   \
        (p)->icmpv6c.comp_csum = -1;   \
    } while (0)

/**
 *  \brief Initialize a packet structure for use.
 */
#ifndef __SC_CUDA_SUPPORT__
#define PACKET_INITIALIZE(p) { \
    memset((p), 0x00, sizeof(Packet)); \
    SCMutexInit(&(p)->mutex_rtv_cnt, NULL); \
    PACKET_RESET_CHECKSUMS((p)); \
}
#else
#define PACKET_INITIALIZE(p) { \
    memset((p), 0x00, sizeof(Packet)); \
    SCMutexInit(&(p)->mutex_rtv_cnt, NULL); \
    PACKET_RESET_CHECKSUMS((p)); \
    SCMutexInit(&(p)->cuda_mutex, NULL); \
    SCCondInit(&(p)->cuda_cond, NULL); \
}
#endif


/**
 *  \brief Recycle a packet structure for reuse.
 *  \todo the mutex destroy & init is necessary because of the memset, reconsider
 */
#define PACKET_DO_RECYCLE(p) do {               \
        CLEAR_ADDR(&(p)->src);                  \
        CLEAR_ADDR(&(p)->dst);                  \
        (p)->sp = 0;                            \
        (p)->dp = 0;                            \
        (p)->proto = 0;                         \
        (p)->recursion_level = 0;               \
        (p)->flags = 0;                         \
        (p)->flowflags = 0;                     \
        (p)->flow = NULL;                       \
        (p)->ts.tv_sec = 0;                     \
        (p)->ts.tv_usec = 0;                    \
        (p)->datalink = 0;                      \
        (p)->action = 0;                        \
        if ((p)->pktvar != NULL) {              \
            PktVarFree((p)->pktvar);            \
            (p)->pktvar = NULL;                 \
        }                                       \
        (p)->ethh = NULL;                       \
        if ((p)->ip4h != NULL) {                \
            CLEAR_IPV4_PACKET((p));             \
        }                                       \
        if ((p)->ip6h != NULL) {                \
            CLEAR_IPV6_PACKET((p));             \
        }                                       \
        if ((p)->tcph != NULL) {                \
            CLEAR_TCP_PACKET((p));              \
        }                                       \
        if ((p)->udph != NULL) {                \
            CLEAR_UDP_PACKET((p));              \
        }                                       \
        if ((p)->icmpv4h != NULL) {             \
            CLEAR_ICMPV4_PACKET((p));           \
        }                                       \
        if ((p)->icmpv6h != NULL) {             \
            CLEAR_ICMPV6_PACKET((p));           \
        }                                       \
        (p)->ppph = NULL;                       \
        (p)->pppoesh = NULL;                    \
        (p)->pppoedh = NULL;                    \
        (p)->greh = NULL;                       \
        (p)->vlanh = NULL;                      \
        (p)->payload = NULL;                    \
        (p)->payload_len = 0;                   \
        (p)->pktlen = 0;                        \
        (p)->alerts.cnt = 0;                    \
        (p)->next = NULL;                       \
        (p)->prev = NULL;                       \
        (p)->rtv_cnt = 0;                       \
        (p)->tpr_cnt = 0;                       \
        SCMutexDestroy(&(p)->mutex_rtv_cnt);    \
        SCMutexInit(&(p)->mutex_rtv_cnt, NULL); \
        (p)->tunnel_proto = 0;                  \
        (p)->tunnel_pkt = 0;                    \
        (p)->tunnel_verdicted = 0;              \
        (p)->events.cnt = 0;                    \
        (p)->root = NULL;                       \
        PACKET_RESET_CHECKSUMS((p));            \
    } while (0)

#ifndef __SC_CUDA_SUPPORT__
#define PACKET_RECYCLE(p) PACKET_DO_RECYCLE((p))
#else
#define PACKET_RECYCLE(p) do {                  \
    PACKET_DO_RECYCLE((p));                     \
    SCMutexDestroy(&(p)->cuda_mutex);           \
    SCCondDestroy(&(p)->cuda_cond);             \
    SCMutexInit(&(p)->cuda_mutex, NULL);        \
    SCCondInit(&(p)->cuda_cond, NULL);          \
    PACKET_RESET_CHECKSUMS((p));                \
} while(0)
#endif

/**
 *  \brief Cleanup a packet so that we can free it. No memset needed..
 */
#ifndef __SC_CUDA_SUPPORT__
#define PACKET_CLEANUP(p) do {                  \
        if ((p)->pktvar != NULL) {              \
            PktVarFree((p)->pktvar);            \
        }                                       \
        SCMutexDestroy(&(p)->mutex_rtv_cnt);    \
    } while (0)
#else
#define PACKET_CLEANUP(p) do {                  \
    if ((p)->pktvar != NULL) {                  \
        PktVarFree((p)->pktvar);                \
    }                                           \
    SCMutexDestroy(&(p)->mutex_rtv_cnt);        \
    SCMutexDestroy(&(p)->cuda_mutex);           \
    SCCondDestroy(&(p)->cuda_cond);             \
} while(0)
#endif


/* macro's for setting the action
 * handle the case of a root packet
 * for tunnels */
#define ACCEPT_PACKET(p)       ((p)->root ? ((p)->root->action = ACTION_ACCEPT) : ((p)->action = ACTION_ACCEPT))
#define DROP_PACKET(p)         ((p)->root ? ((p)->root->action = ACTION_DROP) : ((p)->action = ACTION_DROP))
#define REJECT_PACKET(p)       ((p)->root ? ((p)->root->action = ACTION_REJECT) : ((p)->action = ACTION_REJECT))
#define REJECT_PACKET_DST(p)   ((p)->root ? ((p)->root->action = ACTION_REJECT_DST) : ((p)->action = ACTION_REJECT_DST))
#define REJECT_PACKET_BOTH(p)  ((p)->root ? ((p)->root->action = ACTION_REJECT_BOTH) : ((p)->action = ACTION_REJECT_BOTH))

#define TUNNEL_INCR_PKT_RTV(p) do {                                                 \
        SCMutexLock((p)->root ? &(p)->root->mutex_rtv_cnt : &(p)->mutex_rtv_cnt);   \
        ((p)->root ? (p)->root->rtv_cnt++ : (p)->rtv_cnt++);                        \
        SCMutexUnlock((p)->root ? &(p)->root->mutex_rtv_cnt : &(p)->mutex_rtv_cnt); \
    } while (0)

#define TUNNEL_INCR_PKT_TPR(p) do {                                                 \
        SCMutexLock((p)->root ? &(p)->root->mutex_rtv_cnt : &(p)->mutex_rtv_cnt);   \
        ((p)->root ? (p)->root->tpr_cnt++ : (p)->tpr_cnt++);                        \
        SCMutexUnlock((p)->root ? &(p)->root->mutex_rtv_cnt : &(p)->mutex_rtv_cnt); \
    } while (0)

#define TUNNEL_DECR_PKT_TPR(p) do {                                                 \
        SCMutexLock((p)->root ? &(p)->root->mutex_rtv_cnt : &(p)->mutex_rtv_cnt);   \
        ((p)->root ? (p)->root->tpr_cnt-- : (p)->tpr_cnt--);                        \
        SCMutexUnlock((p)->root ? &(p)->root->mutex_rtv_cnt : &(p)->mutex_rtv_cnt); \
    } while (0)

#define TUNNEL_DECR_PKT_TPR_NOLOCK(p) do {                   \
        ((p)->root ? (p)->root->tpr_cnt-- : (p)->tpr_cnt--); \
    } while (0)

#define TUNNEL_PKT_RTV(p)             ((p)->root ? (p)->root->rtv_cnt : (p)->rtv_cnt)
#define TUNNEL_PKT_TPR(p)             ((p)->root ? (p)->root->tpr_cnt : (p)->tpr_cnt)

#define IS_TUNNEL_ROOT_PKT(p)  (((p)->root == NULL && (p)->tunnel_pkt == 1))
#define IS_TUNNEL_PKT(p)       (((p)->tunnel_pkt == 1))
#define SET_TUNNEL_PKT(p)      ((p)->tunnel_pkt = 1)


void DecodeRegisterPerfCounters(DecodeThreadVars *, ThreadVars *);
Packet *PacketPseudoPktSetup(Packet *parent, uint8_t *pkt, uint16_t len, uint8_t proto);
Packet *PacketGetFromQueueOrAlloc(void);

DecodeThreadVars *DecodeThreadVarsAlloc();

/* decoder functions */
void DecodeEthernet(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodeSll(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodePPP(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodePPPOESession(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodePPPOEDiscovery(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodeTunnel(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodeRaw(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodeIPV4(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodeIPV6(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodeICMPV4(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodeICMPV6(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodeTCP(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodeUDP(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodeGRE(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);
void DecodeVLAN(ThreadVars *, DecodeThreadVars *, Packet *, uint8_t *, uint16_t, PacketQueue *);

void AddressDebugPrint(Address *);

/** \brief Set the No payload inspection Flag for the packet.
 *
 * \param p Packet to set the flag in
 */
#define DecodeSetNoPayloadInspectionFlag(p) do { \
        (p)->flags |= PKT_NOPAYLOAD_INSPECTION;  \
    } while (0)

/** \brief Set the No packet inspection Flag for the packet.
 *
 * \param p Packet to set the flag in
 */
#define DecodeSetNoPacketInspectionFlag(p) do { \
        (p)->flags |= PKT_NOPACKET_INSPECTION;  \
    } while (0)


#define DECODER_SET_EVENT(p, e) do { \
    if ((p)->events.cnt < PACKET_DECODER_EVENT_MAX) { \
        (p)->events.events[(p)->events.cnt] = e; \
        (p)->events.cnt++; \
    } \
} while(0)

#define DECODER_ISSET_EVENT(p, e) ({ \
    int r = 0; \
    uint8_t u; \
    for (u = 0; u < (p)->events.cnt; u++) { \
        if ((p)->events.events[u] == (e)) { \
            r = 1; \
            break; \
        } \
    } \
    r; \
})

/* older libcs don't contain a def for IPPROTO_DCCP
 * inside of <netinet/in.h>
 * if it isn't defined let's define it here.
 */
#ifndef IPPROTO_DCCP
#define IPPROTO_DCCP 33
#endif

/* pcap provides this, but we don't want to depend on libpcap */
#ifndef DLT_EN10MB
#define DLT_EN10MB 1
#endif

/* taken from pcap's bpf.h */
#ifndef DLT_RAW
#ifdef __OpenBSD__
#define DLT_RAW     14  /* raw IP */
#else
#define DLT_RAW     12  /* raw IP */
#endif
#endif

/** libpcap shows us the way to linktype codes
 * \todo we need more & maybe put them in a separate file? */
#define LINKTYPE_ETHERNET   DLT_EN10MB
#define LINKTYPE_LINUX_SLL  113
#define LINKTYPE_PPP        9
#define LINKTYPE_RAW        DLT_RAW
#define PPP_OVER_GRE        11
#define VLAN_OVER_GRE       13

/*Packet Flags*/
#define PKT_NOPACKET_INSPECTION         0x01    /**< Flag to indicate that packet header or contents should not be inspected*/
#define PKT_NOPAYLOAD_INSPECTION        0x02    /**< Flag to indicate that packet contents should not be inspected*/
#define PKT_ALLOC                       0x04    /**< Packet was alloc'd this run, needs to be freed */
#define PKT_HAS_TAG                     0x08    /**< Packet has matched a tag */
#define PKT_STREAM_ADD                  0x10    /**< Packet payload was added to reassembled stream */
#define PKT_STREAM_EOF                  0x20    /**< Stream is in eof state */

#endif /* __DECODE_H__ */
