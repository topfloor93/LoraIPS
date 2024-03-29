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

#ifndef __STREAM_H__
#define __STREAM_H__

#include "flow.h"

#define STREAM_START        FLOW_AL_STREAM_START
#define STREAM_EOF          FLOW_AL_STREAM_EOF
#define STREAM_TOSERVER     FLOW_AL_STREAM_TOSERVER
#define STREAM_TOCLIENT     FLOW_AL_STREAM_TOCLIENT
#define STREAM_GAP          FLOW_AL_STREAM_GAP

/** size of the data chunks sent to the app layer parser. */
#define MSG_DATA_SIZE       2048

#define STREAMQUEUE_FLAG_INIT    0x01

typedef struct StreamMsg_ {
    uint32_t id;    /**< unique stream id */
    uint8_t flags;  /**< msg flags */
    Flow *flow;     /**< parent flow */

    union {
        /* case !STREAM_EOF && !STREAM_GAP */
        struct {
            Address src_ip, dst_ip;     /**< ipaddresses */
            Port src_port, dst_port;    /**< ports */
            uint8_t data[MSG_DATA_SIZE];/**< reassembled data */
            uint16_t data_len;          /**< length of the data */
            uint32_t seq;               /**< sequence number */
        } data;
        /* case STREAM_GAP */
        struct {
            uint32_t gap_size;
        } gap;
    };

    struct StreamMsg_ *next;
    struct StreamMsg_ *prev;
} StreamMsg;

typedef struct StreamMsgQueue_ {
    StreamMsg *top;
    StreamMsg *bot;
    uint16_t len;
//    SCMutex mutex_q;
//    SCCondT cond_q;
    uint8_t flags;
#ifdef DBG_PERF
    uint16_t dbg_maxlen;
#endif /* DBG_PERF */
} StreamMsgQueue;

/* prototypes */
void StreamMsgQueuesInit(void);
void StreamMsgQueuesDeinit(char);

StreamMsg *StreamMsgGetFromPool(void);
void StreamMsgReturnToPool(StreamMsg *);
StreamMsg *StreamMsgGetFromQueue(StreamMsgQueue *);
void StreamMsgPutInQueue(StreamMsgQueue *, StreamMsg *);

StreamMsgQueue *StreamMsgQueueGetNew(void);
void StreamMsgQueueFree(StreamMsgQueue *);
StreamMsgQueue *StreamMsgQueueGetByPort(uint16_t);

void StreamMsgQueueSetMinInitChunkLen(uint8_t, uint16_t);
void StreamMsgQueueSetMinChunkLen(uint8_t dir, uint16_t len);
uint16_t StreamMsgQueueGetMinInitChunkLen(uint8_t);
uint16_t StreamMsgQueueGetMinChunkLen(uint8_t);

#endif /* __STREAM_H__ */

