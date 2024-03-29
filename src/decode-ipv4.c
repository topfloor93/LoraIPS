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
 * \author Brian Rectanus <brectanu@gmail.com>
 *
 * Decode IPv4
 */

#include "suricata-common.h"
#include "packet-queue.h"
#include "decode.h"
#include "decode-ipv4.h"
#include "decode-events.h"
#include "defrag.h"
#include "util-unittest.h"
#include "util-debug.h"

/* Generic validation
 *
 * [--type--][--len---]
 *
 * \todo This function needs removed in favor of specific validation.
 *
 * See: RFC 791
 */
static int IPV4OptValidateGeneric(Packet *p, const IPV4Opt *o) {
    switch (o->type) {
        /* See: RFC 4782 */
        case IPV4_OPT_QS:
            if (p->IPV4_OPTS[p->IPV4_OPTS_CNT].len < IPV4_OPT_QS_MIN) {
                DECODER_SET_EVENT(p,IPV4_OPT_INVALID_LEN);
                return -1;
            }
            break;
        /* See: RFC 1108 */
        case IPV4_OPT_SEC:
            if (p->IPV4_OPTS[p->IPV4_OPTS_CNT].len != IPV4_OPT_SEC_LEN) {
                DECODER_SET_EVENT(p,IPV4_OPT_INVALID_LEN);
                return -1;
            }
            break;
        case IPV4_OPT_SID:
            if (p->IPV4_OPTS[p->IPV4_OPTS_CNT].len != IPV4_OPT_SID_LEN) {
                DECODER_SET_EVENT(p,IPV4_OPT_INVALID_LEN);
                return -1;
            }
            break;
        /* See: RFC 2113 */
        case IPV4_OPT_RTRALT:
            if (p->IPV4_OPTS[p->IPV4_OPTS_CNT].len != IPV4_OPT_RTRALT_LEN) {
                DECODER_SET_EVENT(p,IPV4_OPT_INVALID_LEN);
                return -1;
            }
            break;
        default:
            /* Should never get here unless there is a coding error */
            DECODER_SET_EVENT(p,IPV4_OPT_UNKNOWN);
            return -1;
    }

    return 0;
}

/* Validate route type options
 *
 * [--type--][--len---][--ptr---][address1]...[addressN]
 *
 * See: RFC 791
 */
static int IPV4OptValidateRoute(Packet *p, const IPV4Opt *o) {
    uint8_t ptr;

    /* Check length */
    if (o->len < IPV4_OPT_ROUTE_MIN) {
        DECODER_SET_EVENT(p,IPV4_OPT_INVALID_LEN);
        return -1;
    }

    /* Data is required */
    if (o->data == NULL) {
        DECODER_SET_EVENT(p,IPV4_OPT_MALFORMED);
        return -1;
    }
    ptr = *o->data;

    /* Address pointer is 1 based and points at least after type+len+ptr,
     * must be a incremented by 4 bytes (address size) and cannot extend
     * past option length.
     */
    if ((ptr < 4) || (ptr % 4) || (ptr > o->len + 1)) {
        DECODER_SET_EVENT(p,IPV4_OPT_MALFORMED);
        return -1;
    }

    return 0;
}

/* Validate timestamp type options
 *
 * [--type--][--len---][--ptr---][ovfl][flag][rec1----...]...[recN----...]
 * NOTE: rec could be 4 (ts only) or 8 (ip+ts) bytes in length.
 *
 * See: RFC 781
 */
static int IPV4OptValidateTimestamp(Packet *p, const IPV4Opt *o) {
    uint8_t ptr;
    uint8_t flag;
    uint8_t rec_size;

    /* Check length */
    if (o->len < IPV4_OPT_TS_MIN) {
        DECODER_SET_EVENT(p,IPV4_OPT_INVALID_LEN);
        return -1;
    }

    /* Data is required */
    if (o->data == NULL) {
        DECODER_SET_EVENT(p,IPV4_OPT_MALFORMED);
        return -1;
    }
    ptr = *o->data;

    /* We need the flag to determine what is in the option payload */
    if (ptr < 5) {
        DECODER_SET_EVENT(p,IPV4_OPT_MALFORMED);
        return -1;
    }
    flag = *(o->data + 3) & 0x00ff;

    /* A flag of 1|3 means we have both the ip+ts in each record */
    rec_size = ((flag == 1) || (flag == 3)) ? 8 : 4;

    /* Address pointer is 1 based and points at least after
     * type+len+ptr+ovfl+flag, must be incremented by by the rec_size
     * and cannot extend past option length.
     */
    if (((ptr - 5) % rec_size) || (ptr > o->len + 1)) {
        DECODER_SET_EVENT(p,IPV4_OPT_MALFORMED);
        return -1;
    }

    return 0;
}

/* Validate CIPSO option
 *
 * [--type--][--len---][--doi---][tags--...]
 *
 * See: draft-ietf-cipso-ipsecurity-01.txt
 * See: FIPS 188 (tags 6 & 7)
 */
static int IPV4OptValidateCIPSO(Packet *p, const IPV4Opt *o) {
    uint32_t doi;
    uint8_t *tag;
    uint16_t len;

    /* Check length */
    if (o->len < IPV4_OPT_CIPSO_MIN) {
        DECODER_SET_EVENT(p,IPV4_OPT_INVALID_LEN);
        return -1;
    }

    /* Data is required */
    if (o->data == NULL) {
        DECODER_SET_EVENT(p,IPV4_OPT_MALFORMED);
        return -1;
    }
    doi = *o->data;
    tag = o->data + 4;
    len = o->len - 1 - 1 - 4; /* Length of tags after header */


    /* Domain of Interest (DOI) of 0 is reserved and thus invalid */
    /** \todo Aparently a DOI of zero is fine in practice - verify. */
    if (doi == 0) {
#if 0
        DECODER_SET_EVENT(p,IPV4_OPT_MALFORMED);
        return -1;
#endif
    }

    /* NOTE: We know len has passed min tests prior to this call */

    /* Check that tags are formatted correctly
     * [-ttype--][--tlen--][-tagdata-...]
     */
    while (len) {
        uint8_t ttype;
        uint8_t tlen;

        /* Tag header must fit within option length */
        if (len < 2) {
            //printf("CIPSO tag header too large %" PRIu16 " < 2\n", len);
            DECODER_SET_EVENT(p,IPV4_OPT_MALFORMED);
            return -1;
        }

        /* Tag header is type+len */
        ttype = *(tag++);
        tlen = *(tag++);

        /* Tag length must fit within the option length */
        if (tlen > len) {
            //printf("CIPSO tag len too large %" PRIu8 " > %" PRIu16 "\n", tlen, len);
            DECODER_SET_EVENT(p,IPV4_OPT_MALFORMED);
            return -1;
        }

        switch(ttype) {
            case 0:
                /* Tag type 0 is reserved and thus invalid */
                /** \todo Wireshark marks this a padding, but spec says reserved. */
                DECODER_SET_EVENT(p,IPV4_OPT_MALFORMED);
                return -1;
            case 1:
            case 2:
            case 5:
            case 6:
            case 7:
                /* Tag is at least 4 and at most the remainder of option len */
                if ((tlen < 4) || (tlen > len)) {
                    //printf("CIPSO tag %" PRIu8 " bad tlen=%" PRIu8 " len=%" PRIu8 "\n", ttype, tlen, len);
                    DECODER_SET_EVENT(p,IPV4_OPT_MALFORMED);
                    return -1;
                }

                /* The alignment octet is always 0 except tag
                 * type 7, which has no such field.
                 */
                if ((ttype != 7) && (*tag != 0)) {
                    //printf("CIPSO tag %" PRIu8 " ao=%" PRIu8 "\n", ttype, tlen);
                    DECODER_SET_EVENT(p,IPV4_OPT_MALFORMED);
                    return -1;
                }

                /* Skip the rest of the tag payload */
                tag += tlen - 2;
                len -= tlen;

                continue;
            default:
                //printf("CIPSO tag %" PRIu8 " unknown tag\n", ttype);
                DECODER_SET_EVENT(p,IPV4_OPT_MALFORMED);
                /** \todo May not want to return error here on unknown tag type (at least not for 3|4) */
                return -1;
        }
    }

    return 0;
}

/**
 * Decode/Validate IPv4 Options.
 */
static int DecodeIPV4Options(ThreadVars *tv, Packet *p, uint8_t *pkt, uint16_t len)
{
    uint16_t plen = len;

    p->IPV4_OPTS_CNT = 0;

#ifdef DEBUG
    if (SCLogDebugEnabled()) {
        uint16_t i;
        char buf[256] = "";
        int offset = 0;

        for (i = 0; i < len; i++) {
            offset += snprintf(buf + offset, (sizeof(buf) - offset), "%02" PRIx8 " ", pkt[i]);
        }
        SCLogDebug("IPV4OPTS: { %s}", buf);
    }
#endif

    /* Options length must be padded to 8byte boundary */
    if (plen % 8) {
        DECODER_SET_EVENT(p,IPV4_OPT_PAD_REQUIRED);
        /* Warn - we can keep going */
    }

    while (plen)
    {
        /* single byte options */
        if (*pkt == IPV4_OPT_EOL) {
            /** \todo What if more data exist after EOL (possible covert channel or data leakage)? */
            SCLogDebug("IPV4OPT %" PRIu16 " len 1 @ %" PRIu16 "/%" PRIu16 "",
                   *pkt, (len - plen), (len - 1));
            break;
        } else if (*pkt == IPV4_OPT_NOP) {
            SCLogDebug("IPV4OPT %" PRIu16 " len 1 @ %" PRIu16 "/%" PRIu16 "",
                   *pkt, (len - plen), (len - 1));
            pkt++;
            plen--;

        /* multibyte options */
        } else {
            if (plen < 2) {
                /** \todo What if padding is non-zero (possible covert channel or data leakage)? */
                /** \todo Spec seems to indicate EOL required if there is padding */
                DECODER_SET_EVENT(p,IPV4_OPT_EOL_REQUIRED);
                break;
            }

            /* Option length is too big for packet */
            if (*(pkt+1) > plen) {
                DECODER_SET_EVENT(p,IPV4_OPT_INVALID_LEN);
                return -1;
            }

            p->IPV4_OPTS[p->IPV4_OPTS_CNT].type = *pkt;
            p->IPV4_OPTS[p->IPV4_OPTS_CNT].len  = *(pkt+1);
            if (plen > 2)
                p->IPV4_OPTS[p->IPV4_OPTS_CNT].data = (pkt+2);
            else
                p->IPV4_OPTS[p->IPV4_OPTS_CNT].data = NULL;

            SCLogDebug("IPV4OPT %" PRIu16 " len %" PRIu16 " @ %" PRIu16 "/%" PRIu16 "",
                   p->IPV4_OPTS[p->IPV4_OPTS_CNT].type, p->IPV4_OPTS[p->IPV4_OPTS_CNT].len,
                   (len - plen), (len - 1));

            /* we already know that the total options len is valid,
             * so here the len of the specific option must be bad.
             * Also check for invalid lengths 0 and 1. */
            if (p->IPV4_OPTS[p->IPV4_OPTS_CNT].len > plen ||
                p->IPV4_OPTS[p->IPV4_OPTS_CNT].len < 2) {
                DECODER_SET_EVENT(p,IPV4_OPT_INVALID_LEN);
                return -1;
            }

            /* we are parsing the most commonly used opts to prevent
             * us from having to walk the opts list for these all the
             * time. */
            /** \todo Figure out which IP options are more common and list them first */
            switch (p->IPV4_OPTS[p->IPV4_OPTS_CNT].type) {
                case IPV4_OPT_TS:
                    if (p->ip4vars.o_ts != NULL) {
                        DECODER_SET_EVENT(p,IPV4_OPT_DUPLICATE);
                        /* Warn - we can keep going */
                        break;
                    } else if (IPV4OptValidateTimestamp(p,&p->IPV4_OPTS[p->IPV4_OPTS_CNT])) {
                        return -1;
                    }
                    p->ip4vars.o_ts = &p->IPV4_OPTS[p->IPV4_OPTS_CNT];
                    break;
                case IPV4_OPT_RR:
                    if (p->ip4vars.o_rr != NULL) {
                        DECODER_SET_EVENT(p,IPV4_OPT_DUPLICATE);
                        /* Warn - we can keep going */
                        break;
                    } else if (IPV4OptValidateRoute(p,&p->IPV4_OPTS[p->IPV4_OPTS_CNT]) != 0) {
                        return -1;
                    }
                    p->ip4vars.o_rr = &p->IPV4_OPTS[p->IPV4_OPTS_CNT];
                    break;
                case IPV4_OPT_QS:
                    if (p->ip4vars.o_qs != NULL) {
                        DECODER_SET_EVENT(p,IPV4_OPT_DUPLICATE);
                        /* Warn - we can keep going */
                        break;
                    } else if (IPV4OptValidateGeneric(p, &p->IPV4_OPTS[p->IPV4_OPTS_CNT])) {
                        return -1;
                    }
                    p->ip4vars.o_qs = &p->IPV4_OPTS[p->IPV4_OPTS_CNT];
                    break;
                case IPV4_OPT_SEC:
                    if (p->ip4vars.o_sec != NULL) {
                        DECODER_SET_EVENT(p,IPV4_OPT_DUPLICATE);
                        /* Warn - we can keep going */
                        break;
                    } else if (IPV4OptValidateGeneric(p, &p->IPV4_OPTS[p->IPV4_OPTS_CNT])) {
                        return -1;
                    }
                    p->ip4vars.o_sec = &p->IPV4_OPTS[p->IPV4_OPTS_CNT];
                    break;
                case IPV4_OPT_LSRR:
                    if (p->ip4vars.o_lsrr != NULL) {
                        DECODER_SET_EVENT(p,IPV4_OPT_DUPLICATE);
                        /* Warn - we can keep going */
                        break;
                    } else if (IPV4OptValidateRoute(p,&p->IPV4_OPTS[p->IPV4_OPTS_CNT]) != 0) {
                        return -1;
                    }
                    p->ip4vars.o_lsrr = &p->IPV4_OPTS[p->IPV4_OPTS_CNT];
                    break;
                case IPV4_OPT_CIPSO:
                    if (p->ip4vars.o_cipso != NULL) {
                        DECODER_SET_EVENT(p,IPV4_OPT_DUPLICATE);
                        /* Warn - we can keep going */
                        break;
                    } else if (IPV4OptValidateCIPSO(p,&p->IPV4_OPTS[p->IPV4_OPTS_CNT]) != 0) {
                        return -1;
                    }
                    p->ip4vars.o_cipso = &p->IPV4_OPTS[p->IPV4_OPTS_CNT];
                    break;
                case IPV4_OPT_SID:
                    if (p->ip4vars.o_sid != NULL) {
                        DECODER_SET_EVENT(p,IPV4_OPT_DUPLICATE);
                        /* Warn - we can keep going */
                        break;
                    } else if (IPV4OptValidateGeneric(p, &p->IPV4_OPTS[p->IPV4_OPTS_CNT])) {
                        return -1;
                    }
                    p->ip4vars.o_sid = &p->IPV4_OPTS[p->IPV4_OPTS_CNT];
                    break;
                case IPV4_OPT_SSRR:
                    if (p->ip4vars.o_ssrr != NULL) {
                        DECODER_SET_EVENT(p,IPV4_OPT_DUPLICATE);
                        /* Warn - we can keep going */
                        break;
                    } else if (IPV4OptValidateRoute(p,&p->IPV4_OPTS[p->IPV4_OPTS_CNT]) != 0) {
                        return -1;
                    }
                    p->ip4vars.o_ssrr = &p->IPV4_OPTS[p->IPV4_OPTS_CNT];
                    break;
                case IPV4_OPT_RTRALT:
                    if (p->ip4vars.o_rtralt != NULL) {
                        DECODER_SET_EVENT(p,IPV4_OPT_DUPLICATE);
                        /* Warn - we can keep going */
                        break;
                    } else if (IPV4OptValidateGeneric(p, &p->IPV4_OPTS[p->IPV4_OPTS_CNT])) {
                        return -1;
                    }
                    p->ip4vars.o_rtralt = &p->IPV4_OPTS[p->IPV4_OPTS_CNT];
                    break;
                default:
                    SCLogDebug("IPV4OPT <unknown> (%" PRIu8 ") len %" PRIu8 "",
                           p->IPV4_OPTS[p->IPV4_OPTS_CNT].type,
                           p->IPV4_OPTS[p->IPV4_OPTS_CNT].len);
                    DECODER_SET_EVENT(p,IPV4_OPT_INVALID);
                    /* Warn - we can keep going */
                    break;
            }

            pkt += p->IPV4_OPTS[p->IPV4_OPTS_CNT].len;
            plen -= (p->IPV4_OPTS[p->IPV4_OPTS_CNT].len);
            p->IPV4_OPTS_CNT++;
        }
    }

    return 0;
}

static int DecodeIPV4Packet(ThreadVars *tv, Packet *p, uint8_t *pkt, uint16_t len)
{
    if (len < IPV4_HEADER_LEN) {
        DECODER_SET_EVENT(p,IPV4_PKT_TOO_SMALL);
        return -1;
    }

    if (IP_GET_RAW_VER(pkt) != 4) {
        SCLogDebug("wrong ip version %" PRIu8 "",IP_GET_RAW_VER(pkt));
        DECODER_SET_EVENT(p,IPV4_WRONG_IP_VER);
        return -1;
    }

    p->ip4h = (IPV4Hdr *)pkt;

    if (IPV4_GET_HLEN(p) < IPV4_HEADER_LEN) {
        DECODER_SET_EVENT(p,IPV4_HLEN_TOO_SMALL);
        return -1;
    }

    if (IPV4_GET_IPLEN(p) < IPV4_GET_HLEN(p)) {
        DECODER_SET_EVENT(p,IPV4_IPLEN_SMALLER_THAN_HLEN);
        return -1;
    }

    if (len < IPV4_GET_IPLEN(p)) {
        DECODER_SET_EVENT(p,IPV4_TRUNC_PKT);
        return -1;
    }

    /* save the options len */
    p->ip4vars.ip_opt_len = IPV4_GET_HLEN(p) - IPV4_HEADER_LEN;
    if (p->ip4vars.ip_opt_len > 0) {
        DecodeIPV4Options(tv, p, pkt + IPV4_HEADER_LEN, p->ip4vars.ip_opt_len);
    }

    /* set the address struct */
    SET_IPV4_SRC_ADDR(p,&p->src);
    SET_IPV4_DST_ADDR(p,&p->dst);

    return 0;
}

void DecodeIPV4(ThreadVars *tv, DecodeThreadVars *dtv, Packet *p, uint8_t *pkt, uint16_t len, PacketQueue *pq)
{
    int ret;

    SCPerfCounterIncr(dtv->counter_ipv4, tv->sc_perf_pca);

    /* reset the decoder cache flags */
    IPV4_CACHE_INIT(p);

    SCLogDebug("pkt %p len %"PRIu16"", pkt, len);

    /* do the actual decoding */
    ret = DecodeIPV4Packet (tv, p, pkt, len);
    if (ret < 0) {
        SCLogDebug("decoding IPv4 packet failed");
        p->ip4h = NULL;
        return;
    }

    /* do hdr test, process hdr rules */

#ifdef DEBUG
    if (SCLogDebugEnabled()) { /* only convert the addresses if debug is really enabled */
        /* debug print */
        char s[16], d[16];
        inet_ntop(AF_INET, (const void *)GET_IPV4_SRC_ADDR_PTR(p), s, sizeof(s));
        inet_ntop(AF_INET, (const void *)GET_IPV4_DST_ADDR_PTR(p), d, sizeof(d));
        SCLogDebug("IPV4 %s->%s PROTO: %" PRIu32 " OFFSET: %" PRIu32 " RF: %" PRIu32 " DF: %" PRIu32 " MF: %" PRIu32 " ID: %" PRIu32 "", s,d,
                IPV4_GET_IPPROTO(p), IPV4_GET_IPOFFSET(p), IPV4_GET_RF(p),
                IPV4_GET_DF(p), IPV4_GET_MF(p), IPV4_GET_IPID(p));
    }
#endif /* DEBUG */

    /* check what next decoder to invoke */
    switch (IPV4_GET_IPPROTO(p)) {
        case IPPROTO_IP:
            /* check PPP VJ uncompressed packets and decode tcp dummy */
            if(p->ppph != NULL && ntohs(p->ppph->protocol) == PPP_VJ_UCOMP)    {
                DecodeTCP(tv, dtv, p, pkt + IPV4_GET_HLEN(p),
                          IPV4_GET_IPLEN(p) -  IPV4_GET_HLEN(p), pq);
            }
            break;
        case IPPROTO_TCP:
            DecodeTCP(tv, dtv, p, pkt + IPV4_GET_HLEN(p),
                      IPV4_GET_IPLEN(p) - IPV4_GET_HLEN(p), pq);
            break;
        case IPPROTO_UDP:
            //printf("DecodeIPV4: next layer is UDP\n");
            DecodeUDP(tv, dtv, p, pkt + IPV4_GET_HLEN(p),
                      IPV4_GET_IPLEN(p) - IPV4_GET_HLEN(p), pq);
            break;
        case IPPROTO_ICMP:
            //printf("DecodeIPV4: next layer is ICMP\n");
            DecodeICMPV4(tv, dtv, p, pkt + IPV4_GET_HLEN(p),
                         IPV4_GET_IPLEN(p) - IPV4_GET_HLEN(p), pq);
            break;
        case IPPROTO_IPV6:
            {
                if (pq != NULL) {
                    /* spawn off tunnel packet */
                    Packet *tp = PacketPseudoPktSetup(p, pkt + IPV4_GET_HLEN(p),
                            IPV4_GET_IPLEN(p) - IPV4_GET_HLEN(p),
                            IPV4_GET_IPPROTO(p));

                    /* send that to the Tunnel decoder */
                    DecodeTunnel(tv, dtv, tp, tp->pkt, tp->pktlen, pq);

                    /* add the tp to the packet queue. */
                    PacketEnqueue(pq,tp);
                }
                break;
            }
        case IPPROTO_GRE:
            DecodeGRE(tv, dtv, p, pkt + IPV4_GET_HLEN(p),
                      IPV4_GET_IPLEN(p) - IPV4_GET_HLEN(p), pq);
            break;
    }

    /* If a fragment, pass off for re-assembly. */
    if (IPV4_GET_IPOFFSET(p) > 0 || IPV4_GET_MF(p) == 1) {
        Packet *rp = Defrag(tv, dtv, NULL, p);
        if (rp != NULL) {
            /* Got re-assembled packet, re-run through decoder. */
            DecodeIPV4(tv, dtv, rp, rp->pkt, rp->pktlen, pq);
            PacketEnqueue(pq, rp);
        }
    }

    return;
}

/* UNITTESTS */
#ifdef UNITTESTS

void DecodeIPV4OptionsPrint(Packet *p) {
    IPV4Vars *pv = &p->ip4vars;

    printf("DecodeIPV4Options: cnt=%" PRIu8
           ",rr={t=%" PRIu8 ",l=%" PRIu8 ",d=%p}"
           ",qs={t=%" PRIu8 ",l=%" PRIu8 ",d=%p}"
           ",ts={t=%" PRIu8 ",l=%" PRIu8 ",d=%p}"
           ",sec={t=%" PRIu8 ",l=%" PRIu8 ",d=%p}"
           ",lsrr={t=%" PRIu8 ",l=%" PRIu8 ",d=%p}"
           ",cipso={t=%" PRIu8 ",l=%" PRIu8 ",d=%p}"
           ",sid={t=%" PRIu8 ",l=%" PRIu8 ",d=%p}"
           ",ssrr={t=%" PRIu8 ",l=%" PRIu8 ",d=%p}"
           ",rtralt={t=%" PRIu8 ",l=%" PRIu8 ",d=%p}"
           "}\n",
           pv->ip_opt_cnt,
           (pv->o_rr ? pv->o_rr->type : 0), (pv->o_rr ? pv->o_rr->len : 0), (pv->o_rr ? pv->o_rr->data : 0),
           (pv->o_qs ? pv->o_qs->type : 0), (pv->o_qs ? pv->o_qs->len : 0), (pv->o_qs ? pv->o_qs->data : 0),
           (pv->o_ts ? pv->o_ts->type : 0), (pv->o_ts ? pv->o_ts->len : 0), (pv->o_ts ? pv->o_ts->data : 0),
           (pv->o_sec ? pv->o_sec->type : 0), (pv->o_sec ? pv->o_sec->len : 0), (pv->o_sec ? pv->o_sec->data : 0),
           (pv->o_lsrr ? pv->o_lsrr->type : 0), (pv->o_lsrr ? pv->o_lsrr->len : 0), (pv->o_lsrr ? pv->o_lsrr->data : 0),
           (pv->o_cipso ? pv->o_cipso->type : 0), (pv->o_cipso ? pv->o_cipso->len : 0), (pv->o_cipso ? pv->o_cipso->data : 0),
           (pv->o_sid ? pv->o_sid->type : 0), (pv->o_sid ? pv->o_sid->len : 0), (pv->o_sid ? pv->o_sid->data : 0),
           (pv->o_ssrr ? pv->o_ssrr->type : 0), (pv->o_ssrr ? pv->o_ssrr->len : 0), (pv->o_ssrr ? pv->o_ssrr->data : 0),
           (pv->o_rtralt ? pv->o_rtralt->type : 0), (pv->o_rtralt ? pv->o_rtralt->len : 0), (pv->o_rtralt ? pv->o_rtralt->data : 0));
}

/** \test IPV4 with no options. */
int DecodeIPV4OptionsNONETest01(void) {
    uint8_t raw_opts[] = { };
    Packet p;
    ThreadVars tv;
    uint8_t *data = (uint8_t *)&p;
    uint16_t i;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    if (rc != 0) {
        DecodeIPV4OptionsPrint(&p);
        return 0;
    }

    for (i = 0; i < (uint16_t)sizeof(Packet); i++) {
        if (*data) {
            /* Should not have modified packet data */
            //printf("Data modified at offset %" PRIu16 "\n", i);
            DecodeIPV4OptionsPrint(&p);
            return 0;
        }
    }

    return 1;
}

/** \test IPV4 with EOL option. */
int DecodeIPV4OptionsEOLTest01(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_EOL, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    uint8_t *data = (uint8_t *)&p;
    uint16_t i;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    if (rc != 0) {
        DecodeIPV4OptionsPrint(&p);
        return 0;
    }

    for (i = 0; i < (uint16_t)sizeof(Packet); i++) {
        if (*data) {
            /* Should not have modified packet data */
            //printf("Data modified at offset %" PRIu16 "\n", i);
            DecodeIPV4OptionsPrint(&p);
            return 0;
        }
    }

    return 1;
}

/** \test IPV4 with NOP option. */
int DecodeIPV4OptionsNOPTest01(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_NOP, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    uint8_t *data = (uint8_t *)&p;
    uint16_t i;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    if (rc != 0) {
        DecodeIPV4OptionsPrint(&p);
        return 0;
    }

    for (i = 0; i < (uint16_t)sizeof(Packet); i++) {
        if (*data) {
            /* Should not have modified packet data */
            //printf("Data modified at offset %" PRIu16 "\n", i);
            DecodeIPV4OptionsPrint(&p);
            return 0;
        }
    }

    return 1;
}

/** \test IPV4 with RR option. */
int DecodeIPV4OptionsRRTest01(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_RR, 0x27, 0x08, 0xc0, 0xa8, 0x2a, 0x64, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    //printf("rc=%d,cnt=%" PRIu16 ",type=%" PRIu8 ",len=%" PRIu8 ",rr=%" PRIuMAX "/%" PRIuMAX "\n", rc, p.IPV4_OPTS_CNT, p.IPV4_OPTS[0].type, p.IPV4_OPTS[0].len, (uintmax_t)p.ip4vars.o_rr, (uintmax_t)&p.IPV4_OPTS[0]);
    if (   (rc == 0)
        && (p.IPV4_OPTS_CNT == 1)
        && (p.IPV4_OPTS[0].type == IPV4_OPT_RR)
        && (p.IPV4_OPTS[0].len == 0x27)
        && (p.ip4vars.o_rr == &p.IPV4_OPTS[0]))
    {
        return 1;
    }

    DecodeIPV4OptionsPrint(&p);
    return 0;
}

/** \test IPV4 with RR option (len too large). */
int DecodeIPV4OptionsRRTest02(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_RR, 0xff, 0x08, 0xc0, 0xa8, 0x2a, 0x64, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    //printf("rc=%d,cnt=%" PRIu16 ",type=%" PRIu8 ",len=%" PRIu8 ",rr=%" PRIuMAX "/%" PRIuMAX "\n", rc, p.IPV4_OPTS_CNT, p.IPV4_OPTS[0].type, p.IPV4_OPTS[0].len, (uintmax_t)p.ip4vars.o_rr, (uintmax_t)&p.IPV4_OPTS[0]);
    if (rc != 0) {
        return 1;
    }

    DecodeIPV4OptionsPrint(&p);
    return 0;
}

/** \test IPV4 with RR option (ptr too large). */
int DecodeIPV4OptionsRRTest03(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_RR, 0x27, 0xff, 0xc0, 0xa8, 0x2a, 0x64, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    //printf("rc=%d,cnt=%" PRIu16 ",type=%" PRIu8 ",len=%" PRIu8 ",rr=%" PRIuMAX "/%" PRIuMAX "\n", rc, p.IPV4_OPTS_CNT, p.IPV4_OPTS[0].type, p.IPV4_OPTS[0].len, (uintmax_t)p.ip4vars.o_rr, (uintmax_t)&p.IPV4_OPTS[0]);
    if (rc != 0) {
        return 1;
    }

    DecodeIPV4OptionsPrint(&p);
    return 0;
}

/** \test IPV4 with RR option (ptr not in 4 byte increment). */
int DecodeIPV4OptionsRRTest04(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_RR, 0x27, 0x05, 0xc0, 0xa8, 0x2a, 0x64, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    //printf("rc=%d,cnt=%" PRIu16 ",type=%" PRIu8 ",len=%" PRIu8 ",rr=%" PRIuMAX "/%" PRIuMAX "\n", rc, p.IPV4_OPTS_CNT, p.IPV4_OPTS[0].type, p.IPV4_OPTS[0].len, (uintmax_t)p.ip4vars.o_rr, (uintmax_t)&p.IPV4_OPTS[0]);
    if (rc != 0) {
        return 1;
    }

    DecodeIPV4OptionsPrint(&p);
    return 0;
}

/** \test IPV4 with QS option. */
int DecodeIPV4OptionsQSTest01(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_QS, 0x08, 0x0d, 0x00, 0xbe, 0xef, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    //printf("rc=%d,cnt=%" PRIu16 ",type=%" PRIu8 ",len=%" PRIu8 ",qs=%" PRIuMAX "/%" PRIuMAX "\n", rc, p.IPV4_OPTS_CNT, p.IPV4_OPTS[0].type, p.IPV4_OPTS[0].len, (uintmax_t)p.ip4vars.o_qs, (uintmax_t)&p.IPV4_OPTS[0]);
    if (   (rc == 0)
        && (p.IPV4_OPTS_CNT == 1)
        && (p.IPV4_OPTS[0].type == IPV4_OPT_QS)
        && (p.IPV4_OPTS[0].len == 0x08)
        && (p.ip4vars.o_qs == &p.IPV4_OPTS[0]))
    {
        return 1;
    }

    DecodeIPV4OptionsPrint(&p);
    return 0;
}

/** \test IPV4 with QS option (len too small) */
int DecodeIPV4OptionsQSTest02(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_QS, 0x07, 0x0d, 0x00, 0xbe, 0xef, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    //printf("rc=%d,cnt=%" PRIu16 ",type=%" PRIu8 ",len=%" PRIu8 ",qs=%" PRIuMAX "/%" PRIuMAX "\n", rc, p.IPV4_OPTS_CNT, p.IPV4_OPTS[0].type, p.IPV4_OPTS[0].len, (uintmax_t)p.ip4vars.o_qs, (uintmax_t)&p.IPV4_OPTS[0]);
    if (rc != 0) {
        return 1;
    }

    DecodeIPV4OptionsPrint(&p);
    return 0;
}

/** \test IPV4 with TS option. */
int DecodeIPV4OptionsTSTest01(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_TS, 0x24, 0x0d, 0x01, 0x0a, 0x0a, 0x0a, 0x69,
        0x04, 0xce, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    //printf("rc=%d,cnt=%" PRIu16 ",type=%" PRIu8 ",len=%" PRIu8 ",ts=%" PRIuMAX "/%" PRIuMAX "\n", rc, p.IPV4_OPTS_CNT, p.IPV4_OPTS[0].type, p.IPV4_OPTS[0].len, (uintmax_t)p.ip4vars.o_ts, (uintmax_t)&p.IPV4_OPTS[0]);
    if (   (rc == 0)
        && (p.IPV4_OPTS_CNT == 1)
        && (p.IPV4_OPTS[0].type == IPV4_OPT_TS)
        && (p.IPV4_OPTS[0].len == 0x24)
        && (p.ip4vars.o_ts == &p.IPV4_OPTS[0]))
    {
        return 1;
    }

    DecodeIPV4OptionsPrint(&p);
    return 0;
}

/** \test IPV4 with TS option (ptr too small). */
int DecodeIPV4OptionsTSTest02(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_TS, 0x24, 0x04, 0x01, 0x0a, 0x0a, 0x0a, 0x69,
        0x04, 0xce, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    //printf("rc=%d,cnt=%" PRIu16 ",type=%" PRIu8 ",len=%" PRIu8 ",ts=%" PRIuMAX "/%" PRIuMAX "\n", rc, p.IPV4_OPTS_CNT, p.IPV4_OPTS[0].type, p.IPV4_OPTS[0].len, (uintmax_t)p.ip4vars.o_ts, (uintmax_t)&p.IPV4_OPTS[0]);
    if (rc != 0) {
        return 1;
    }

    DecodeIPV4OptionsPrint(&p);
    return 0;
}

/** \test IPV4 with TS option (ptr too large). */
int DecodeIPV4OptionsTSTest03(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_TS, 0x24, 0xff, 0x01, 0x0a, 0x0a, 0x0a, 0x69,
        0x04, 0xce, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    //printf("rc=%d,cnt=%" PRIu16 ",type=%" PRIu8 ",len=%" PRIu8 ",ts=%" PRIuMAX "/%" PRIuMAX "\n", rc, p.IPV4_OPTS_CNT, p.IPV4_OPTS[0].type, p.IPV4_OPTS[0].len, (uintmax_t)p.ip4vars.o_ts, (uintmax_t)&p.IPV4_OPTS[0]);
    if (rc != 0) {
        return 1;
    }

    DecodeIPV4OptionsPrint(&p);
    return 0;
}

/** \test IPV4 with TS option (ptr not valid). */
int DecodeIPV4OptionsTSTest04(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_TS, 0x24, 0x0a, 0x01, 0x0a, 0x0a, 0x0a, 0x69,
        0x04, 0xce, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    //printf("rc=%d,cnt=%" PRIu16 ",type=%" PRIu8 ",len=%" PRIu8 ",ts=%" PRIuMAX "/%" PRIuMAX "\n", rc, p.IPV4_OPTS_CNT, p.IPV4_OPTS[0].type, p.IPV4_OPTS[0].len, (uintmax_t)p.ip4vars.o_ts, (uintmax_t)&p.IPV4_OPTS[0]);
    if (rc != 0) {
        return 1;
    }

    DecodeIPV4OptionsPrint(&p);
    return 0;
}

/** \test IPV4 with SEC option. */
int DecodeIPV4OptionsSECTest01(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_SEC, 0x0b, 0xf1, 0x35, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    //printf("rc=%d,cnt=%" PRIu16 ",type=%" PRIu8 ",len=%" PRIu8 ",sec=%" PRIuMAX "/%" PRIuMAX "\n", rc, p.IPV4_OPTS_CNT, p.IPV4_OPTS[0].type, p.IPV4_OPTS[0].len, (uintmax_t)p.ip4vars.o_sec, (uintmax_t)&p.IPV4_OPTS[0]);
    if (   (rc == 0)
        && (p.IPV4_OPTS_CNT == 1)
        && (p.IPV4_OPTS[0].type == IPV4_OPT_SEC)
        && (p.IPV4_OPTS[0].len == 0x0b)
        && (p.ip4vars.o_sec == &p.IPV4_OPTS[0]))
    {
        return 1;
    }

    DecodeIPV4OptionsPrint(&p);
    return 0;
}

/** \test IPV4 with SEC option (invalid length). */
int DecodeIPV4OptionsSECTest02(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_SEC, 0x0a, 0xf1, 0x35, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    //printf("rc=%d,cnt=%" PRIu16 ",type=%" PRIu8 ",len=%" PRIu8 ",sec=%" PRIuMAX "/%" PRIuMAX "\n", rc, p.IPV4_OPTS_CNT, p.IPV4_OPTS[0].type, p.IPV4_OPTS[0].len, (uintmax_t)p.ip4vars.o_sec, (uintmax_t)&p.IPV4_OPTS[0]);
    if (rc != 0) {
        return 1;
    }

    DecodeIPV4OptionsPrint(&p);
    return 0;
}

/** \test IPV4 with LSRR option. */
int DecodeIPV4OptionsLSRRTest01(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_LSRR, 0x27, 0x08, 0xc0, 0xa8, 0x2a, 0x64, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    //printf("rc=%d,cnt=%" PRIu16 ",type=%" PRIu8 ",len=%" PRIu8 ",lsrr=%" PRIuMAX "/%" PRIuMAX "\n", rc, p.IPV4_OPTS_CNT, p.IPV4_OPTS[0].type, p.IPV4_OPTS[0].len, (uintmax_t)p.ip4vars.o_lsrr, (uintmax_t)&p.IPV4_OPTS[0]);
    if (   (rc == 0)
        && (p.IPV4_OPTS_CNT == 1)
        && (p.IPV4_OPTS[0].type == IPV4_OPT_LSRR)
        && (p.IPV4_OPTS[0].len == 0x27)
        && (p.ip4vars.o_lsrr == &p.IPV4_OPTS[0]))
    {
        return 1;
    }

    DecodeIPV4OptionsPrint(&p);
    return 0;
}

/** \test IPV4 with LSRR option (len too large). */
int DecodeIPV4OptionsLSRRTest02(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_LSRR, 0xff, 0x08, 0xc0, 0xa8, 0x2a, 0x64, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    //printf("rc=%d,cnt=%" PRIu16 ",type=%" PRIu8 ",len=%" PRIu8 ",lsrr=%" PRIuMAX "/%" PRIuMAX "\n", rc, p.IPV4_OPTS_CNT, p.IPV4_OPTS[0].type, p.IPV4_OPTS[0].len, (uintmax_t)p.ip4vars.o_lsrr, (uintmax_t)&p.IPV4_OPTS[0]);
    if (rc != 0) {
        return 1;
    }

    DecodeIPV4OptionsPrint(&p);
    return 0;
}

/** \test IPV4 with LSRR option (ptr too large). */
int DecodeIPV4OptionsLSRRTest03(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_LSRR, 0x27, 0xff, 0xc0, 0xa8, 0x2a, 0x64, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    //printf("rc=%d,cnt=%" PRIu16 ",type=%" PRIu8 ",len=%" PRIu8 ",lsrr=%" PRIuMAX "/%" PRIuMAX "\n", rc, p.IPV4_OPTS_CNT, p.IPV4_OPTS[0].type, p.IPV4_OPTS[0].len, (uintmax_t)p.ip4vars.o_lsrr, (uintmax_t)&p.IPV4_OPTS[0]);
    if (rc != 0) {
        return 1;
    }

    DecodeIPV4OptionsPrint(&p);
    return 0;
}

/** \test IPV4 with LSRR option (ptr not in 4 byte increment). */
int DecodeIPV4OptionsLSRRTest04(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_LSRR, 0x27, 0x05, 0xc0, 0xa8, 0x2a, 0x64, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    //printf("rc=%d,cnt=%" PRIu16 ",type=%" PRIu8 ",len=%" PRIu8 ",lsrr=%" PRIuMAX "/%" PRIuMAX "\n", rc, p.IPV4_OPTS_CNT, p.IPV4_OPTS[0].type, p.IPV4_OPTS[0].len, (uintmax_t)p.ip4vars.o_lsrr, (uintmax_t)&p.IPV4_OPTS[0]);
    if (rc != 0) {
        return 1;
    }

    DecodeIPV4OptionsPrint(&p);
    return 0;
}

/** \test IPV4 with CIPSO option. */
int DecodeIPV4OptionsCIPSOTest01(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_CIPSO, 0x18, 0x00, 0x00, 0x00, 0x05, 0x05, 0x12,
        0x00, 0x03, 0x00, 0xef, 0x00, 0xef, 0x00, 0x06,
        0x00, 0x04, 0x00, 0x02, 0x00, 0x02, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    //printf("rc=%d,cnt=%" PRIu16 ",type=%" PRIu8 ",len=%" PRIu8 ",rr=%" PRIuMAX "/%" PRIuMAX "\n", rc, p.IPV4_OPTS_CNT, p.IPV4_OPTS[0].type, p.IPV4_OPTS[0].len, (uintmax_t)p.ip4vars.o_cipso, (uintmax_t)&p.IPV4_OPTS[0]);
    if (   (rc == 0)
        && (p.IPV4_OPTS_CNT == 1)
        && (p.IPV4_OPTS[0].type == IPV4_OPT_CIPSO)
        && (p.IPV4_OPTS[0].len == 0x18)
        && (p.ip4vars.o_cipso == &p.IPV4_OPTS[0]))
    {
        return 1;
    }

    DecodeIPV4OptionsPrint(&p);
    return 0;
}

/** \test IPV4 with SID option. */
int DecodeIPV4OptionsSIDTest01(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_SID, 0x04, 0xbe, 0xef, 0x00, 0x00, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    //printf("rc=%d,cnt=%" PRIu16 ",type=%" PRIu8 ",len=%" PRIu8 ",sid=%" PRIuMAX "/%" PRIuMAX "\n", rc, p.IPV4_OPTS_CNT, p.IPV4_OPTS[0].type, p.IPV4_OPTS[0].len, (uintmax_t)p.ip4vars.o_sid, (uintmax_t)&p.IPV4_OPTS[0]);
    if (   (rc == 0)
        && (p.IPV4_OPTS_CNT == 1)
        && (p.IPV4_OPTS[0].type == IPV4_OPT_SID)
        && (p.IPV4_OPTS[0].len == 0x04)
        && (p.ip4vars.o_sid == &p.IPV4_OPTS[0]))
    {
        return 1;
    }

    DecodeIPV4OptionsPrint(&p);
    return 0;
}

/** \test IPV4 with SID option (len invalid. */
int DecodeIPV4OptionsSIDTest02(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_SID, 0x05, 0xbe, 0xef, 0x00, 0x00, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    //printf("rc=%d,cnt=%" PRIu16 ",type=%" PRIu8 ",len=%" PRIu8 ",sid=%" PRIuMAX "/%" PRIuMAX "\n", rc, p.IPV4_OPTS_CNT, p.IPV4_OPTS[0].type, p.IPV4_OPTS[0].len, (uintmax_t)p.ip4vars.o_sid, (uintmax_t)&p.IPV4_OPTS[0]);
    if (rc != 0) {
        return 1;
    }

    DecodeIPV4OptionsPrint(&p);
    return 0;
}

/** \test IPV4 with SSRR option. */
int DecodeIPV4OptionsSSRRTest01(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_SSRR, 0x27, 0x08, 0xc0, 0xa8, 0x2a, 0x64, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    //printf("rc=%d,cnt=%" PRIu16 ",type=%" PRIu8 ",len=%" PRIu8 ",ssrr=%" PRIuMAX "/%" PRIuMAX "\n", rc, p.IPV4_OPTS_CNT, p.IPV4_OPTS[0].type, p.IPV4_OPTS[0].len, (uintmax_t)p.ip4vars.o_ssrr, (uintmax_t)&p.IPV4_OPTS[0]);
    if (   (rc == 0)
        && (p.IPV4_OPTS_CNT == 1)
        && (p.IPV4_OPTS[0].type == IPV4_OPT_SSRR)
        && (p.IPV4_OPTS[0].len == 0x27)
        && (p.ip4vars.o_ssrr == &p.IPV4_OPTS[0]))
    {
        return 1;
    }

    DecodeIPV4OptionsPrint(&p);
    return 0;
}

/** \test IPV4 with SSRR option (len too large). */
int DecodeIPV4OptionsSSRRTest02(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_SSRR, 0xff, 0x08, 0xc0, 0xa8, 0x2a, 0x64, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    //printf("rc=%d,cnt=%" PRIu16 ",type=%" PRIu8 ",len=%" PRIu8 ",ssrr=%" PRIuMAX "/%" PRIuMAX "\n", rc, p.IPV4_OPTS_CNT, p.IPV4_OPTS[0].type, p.IPV4_OPTS[0].len, (uintmax_t)p.ip4vars.o_ssrr, (uintmax_t)&p.IPV4_OPTS[0]);
    if (rc != 0) {
        return 1;
    }

    DecodeIPV4OptionsPrint(&p);
    return 0;
}

/** \test IPV4 with SSRR option (ptr too large). */
int DecodeIPV4OptionsSSRRTest03(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_SSRR, 0x27, 0xff, 0xc0, 0xa8, 0x2a, 0x64, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    //printf("rc=%d,cnt=%" PRIu16 ",type=%" PRIu8 ",len=%" PRIu8 ",ssrr=%" PRIuMAX "/%" PRIuMAX "\n", rc, p.IPV4_OPTS_CNT, p.IPV4_OPTS[0].type, p.IPV4_OPTS[0].len, (uintmax_t)p.ip4vars.o_ssrr, (uintmax_t)&p.IPV4_OPTS[0]);
    if (rc != 0) {
        return 1;
    }

    DecodeIPV4OptionsPrint(&p);
    return 0;
}

/** \test IPV4 with SSRR option (ptr not in 4 byte increment). */
int DecodeIPV4OptionsSSRRTest04(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_SSRR, 0x27, 0x05, 0xc0, 0xa8, 0x2a, 0x64, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    //printf("rc=%d,cnt=%" PRIu16 ",type=%" PRIu8 ",len=%" PRIu8 ",ssrr=%" PRIuMAX "/%" PRIuMAX "\n", rc, p.IPV4_OPTS_CNT, p.IPV4_OPTS[0].type, p.IPV4_OPTS[0].len, (uintmax_t)p.ip4vars.o_ssrr, (uintmax_t)&p.IPV4_OPTS[0]);
    if (rc != 0) {
        return 1;
    }

    DecodeIPV4OptionsPrint(&p);
    return 0;
}

/** \test IPV4 with RTRALT option. */
int DecodeIPV4OptionsRTRALTTest01(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_RTRALT, 0x04, 0xbe, 0xef, 0x00, 0x00, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    //printf("rc=%d,cnt=%" PRIu16 ",type=%" PRIu8 ",len=%" PRIu8 ",rtralt=%" PRIuMAX "/%" PRIuMAX "\n", rc, p.IPV4_OPTS_CNT, p.IPV4_OPTS[0].type, p.IPV4_OPTS[0].len, (uintmax_t)p.ip4vars.o_rtralt, (uintmax_t)&p.IPV4_OPTS[0]);
    if (   (rc == 0)
        && (p.IPV4_OPTS_CNT == 1)
        && (p.IPV4_OPTS[0].type == IPV4_OPT_RTRALT)
        && (p.IPV4_OPTS[0].len == 0x04)
        && (p.ip4vars.o_rtralt == &p.IPV4_OPTS[0]))
    {
        return 1;
    }

    DecodeIPV4OptionsPrint(&p);
    return 0;
}

/** \test IPV4 with RTRALT option (len invalid. */
int DecodeIPV4OptionsRTRALTTest02(void) {
    uint8_t raw_opts[] = {
        IPV4_OPT_RTRALT, 0x05, 0xbe, 0xef, 0x00, 0x00, 0x00, 0x00
    };
    Packet p;
    ThreadVars tv;
    int rc;

    memset(&tv, 0, sizeof(ThreadVars));
    memset(&p, 0, sizeof(Packet));

    rc = DecodeIPV4Options(&tv, &p, raw_opts, sizeof(raw_opts));
    //printf("rc=%d,cnt=%" PRIu16 ",type=%" PRIu8 ",len=%" PRIu8 ",rtralt=%" PRIuMAX "/%" PRIuMAX "\n", rc, p.IPV4_OPTS_CNT, p.IPV4_OPTS[0].type, p.IPV4_OPTS[0].len, (uintmax_t)p.ip4vars.o_rtralt, (uintmax_t)&p.IPV4_OPTS[0]);
    if (rc != 0) {
        return 1;
    }

    DecodeIPV4OptionsPrint(&p);
    return 0;
}

static int IPV4CalculateValidChecksumtest01(void)
{
    uint16_t csum = 0;

    uint8_t raw_ipv4[] = {
        0x45, 0x00, 0x00, 0x54, 0x00, 0x00, 0x40, 0x00,
        0x40, 0x01, 0xb7, 0x52, 0xc0, 0xa8, 0x01, 0x03,
        0xc0, 0xa8, 0x01, 0x03};

    csum = *( ((uint16_t *)raw_ipv4) + 5);

    return (csum == IPV4CalculateChecksum((uint16_t *)raw_ipv4, sizeof(raw_ipv4)));
}

static int IPV4CalculateInvalidChecksumtest02(void)
{
    uint16_t csum = 0;

    uint8_t raw_ipv4[] = {
        0x45, 0x00, 0x00, 0x54, 0x00, 0x00, 0x40, 0x00,
        0x40, 0x01, 0xb7, 0x52, 0xc0, 0xa8, 0x01, 0x03,
        0xc0, 0xa8, 0x01, 0x07};

    csum = *( ((uint16_t *)raw_ipv4) + 5);

    return (csum == IPV4CalculateChecksum((uint16_t *)raw_ipv4, sizeof(raw_ipv4)));
}
#endif /* UNITTESTS */

void DecodeIPV4RegisterTests(void) {
#ifdef UNITTESTS
    UtRegisterTest("DecodeIPV4OptionsNONETest01", DecodeIPV4OptionsNONETest01, 1);
    UtRegisterTest("DecodeIPV4OptionsEOLTest01", DecodeIPV4OptionsEOLTest01, 1);
    UtRegisterTest("DecodeIPV4OptionsNOPTest01", DecodeIPV4OptionsNOPTest01, 1);
    UtRegisterTest("DecodeIPV4OptionsRRTest01", DecodeIPV4OptionsRRTest01, 1);
    UtRegisterTest("DecodeIPV4OptionsRRTest02", DecodeIPV4OptionsRRTest02, 1);
    UtRegisterTest("DecodeIPV4OptionsRRTest03", DecodeIPV4OptionsRRTest03, 1);
    UtRegisterTest("DecodeIPV4OptionsRRTest04", DecodeIPV4OptionsRRTest04, 1);
    UtRegisterTest("DecodeIPV4OptionsQSTest01", DecodeIPV4OptionsQSTest01, 1);
    UtRegisterTest("DecodeIPV4OptionsQSTest02", DecodeIPV4OptionsQSTest02, 1);
    UtRegisterTest("DecodeIPV4OptionsTSTest01", DecodeIPV4OptionsTSTest01, 1);
    UtRegisterTest("DecodeIPV4OptionsTSTest02", DecodeIPV4OptionsTSTest02, 1);
    UtRegisterTest("DecodeIPV4OptionsTSTest03", DecodeIPV4OptionsTSTest03, 1);
    UtRegisterTest("DecodeIPV4OptionsTSTest04", DecodeIPV4OptionsTSTest04, 1);
    UtRegisterTest("DecodeIPV4OptionsSECTest01", DecodeIPV4OptionsSECTest01, 1);
    UtRegisterTest("DecodeIPV4OptionsSECTest02", DecodeIPV4OptionsSECTest02, 1);
    UtRegisterTest("DecodeIPV4OptionsLSRRTest01", DecodeIPV4OptionsLSRRTest01, 1);
    UtRegisterTest("DecodeIPV4OptionsLSRRTest02", DecodeIPV4OptionsLSRRTest02, 1);
    UtRegisterTest("DecodeIPV4OptionsLSRRTest03", DecodeIPV4OptionsLSRRTest03, 1);
    UtRegisterTest("DecodeIPV4OptionsLSRRTest04", DecodeIPV4OptionsLSRRTest04, 1);
    UtRegisterTest("DecodeIPV4OptionsCIPSOTest01", DecodeIPV4OptionsCIPSOTest01, 1);
    UtRegisterTest("DecodeIPV4OptionsSIDTest01", DecodeIPV4OptionsSIDTest01, 1);
    UtRegisterTest("DecodeIPV4OptionsSIDTest02", DecodeIPV4OptionsSIDTest02, 1);
    UtRegisterTest("DecodeIPV4OptionsSSRRTest01", DecodeIPV4OptionsSSRRTest01, 1);
    UtRegisterTest("DecodeIPV4OptionsSSRRTest02", DecodeIPV4OptionsSSRRTest02, 1);
    UtRegisterTest("DecodeIPV4OptionsSSRRTest03", DecodeIPV4OptionsSSRRTest03, 1);
    UtRegisterTest("DecodeIPV4OptionsSSRRTest04", DecodeIPV4OptionsSSRRTest04, 1);
    UtRegisterTest("DecodeIPV4OptionsRTRALTTest01", DecodeIPV4OptionsRTRALTTest01, 1);
    UtRegisterTest("DecodeIPV4OptionsRTRALTTest02", DecodeIPV4OptionsRTRALTTest02, 1);
    UtRegisterTest("IPV4CalculateValidChecksumtest01",
                   IPV4CalculateValidChecksumtest01, 1);
    UtRegisterTest("IPV4CalculateInvalidChecksumtest02",
                   IPV4CalculateInvalidChecksumtest02, 0);
#endif /* UNITTESTS */
}
