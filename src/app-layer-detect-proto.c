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

/** \file
 *
 *  \author Victor Julien <victor@inliniac.net>
 *
 *  A simple application layer (L7) protocol detector. It works by allowing
 *  developers to set a series of patterns that if exactly matching indicate
 *  that the session is a certain protocol.
 *
 *  \todo More advanced detection methods, regex maybe.
 *  \todo Fall back to port based classification if other detection fails.
 */

#include "suricata-common.h"
#include "debug.h"
#include "decode.h"
#include "threads.h"
#include "tm-modules.h"
#include "threadvars.h"
#include "tm-threads.h"

#include "detect.h"
#include "detect-engine.h"
#include "detect-content.h"
#include "detect-engine-mpm.h"

#include "util-print.h"
#include "util-pool.h"
#include "util-unittest.h"

#include "stream-tcp-private.h"
#include "stream-tcp-reassemble.h"
#include "stream-tcp.h"
#include "stream.h"

#include "app-layer-protos.h"
#include "app-layer-parser.h"
#include "app-layer-detect-proto.h"

#include "util-spm.h"
#include "util-debug.h"

#define INSPECT_BYTES  32

/* undef __SC_CUDA_SUPPORT__.  We will get back to this later.  Need to
 * analyze the performance of cuda support for app layer */

/** global app layer detection context */
AlpProtoDetectCtx alp_proto_ctx;

/** \brief Initialize the app layer proto detection */
void AlpProtoInit(AlpProtoDetectCtx *ctx) {
    memset(ctx, 0x00, sizeof(AlpProtoDetectCtx));

    MpmInitCtx(&ctx->toserver.mpm_ctx, MPM_B2G, -1);
    MpmInitCtx(&ctx->toclient.mpm_ctx, MPM_B2G, -1);

    memset(&ctx->toserver.map, 0x00, sizeof(ctx->toserver.map));
    memset(&ctx->toclient.map, 0x00, sizeof(ctx->toclient.map));

    ctx->toserver.id = 0;
    ctx->toclient.id = 0;
    ctx->toclient.min_len = INSPECT_BYTES;
    ctx->toserver.min_len = INSPECT_BYTES;

    ctx->mpm_pattern_id_store = MpmPatternIdTableInitHash();
}

/**
 *  \brief Turn a proto detection into a AlpProtoSignature and store it
 *         in the ctx.
 *
 *  \param ctx the contex
 *  \param co the content match
 *  \param proto the proto id
 */
static void AlpProtoAddSignature(AlpProtoDetectCtx *ctx, DetectContentData *co, uint16_t ip_proto, uint16_t proto) {
    AlpProtoSignature *s = SCMalloc(sizeof(AlpProtoSignature));
    if (s == NULL) {
        return;
    }
    memset(s, 0x00, sizeof(AlpProtoSignature));

    s->ip_proto = ip_proto;
    s->proto = proto;
    s->co = co;

    if (ctx->head == NULL) {
        ctx->head = s;
    } else {
        s->next = ctx->head;
        ctx->head = s;
    }

    ctx->sigs++;
}

#ifdef UNITTESTS
/** \brief free a AlpProtoSignature, recursively free any next sig */
static void AlpProtoFreeSignature(AlpProtoSignature *s) {
    if (s == NULL)
        return;

    DetectContentFree(s->co);
    s->co = NULL;
    s->proto = 0;

    AlpProtoSignature *next_s = s->next;

    SCFree(s);

    AlpProtoFreeSignature(next_s);
}
#endif

/**
 *  \brief Match a AlpProtoSignature against a buffer
 *
 *  \param s signature
 *  \param buf pointer to buffer
 *  \param buflen length of the buffer
 *  \param ip_proto packet's ip_proto
 *
 *  \retval proto the detected proto or ALPROTO_UNKNOWN if no match
 */
static uint16_t AlpProtoMatchSignature(AlpProtoSignature *s, uint8_t *buf,
        uint16_t buflen, uint16_t ip_proto)
{
    SCEnter();
    uint16_t proto = ALPROTO_UNKNOWN;

    if (s->ip_proto != ip_proto) {
        goto end;
    }

    if (s->co->offset > buflen) {
        SCLogDebug("s->co->offset (%"PRIu16") > buflen (%"PRIu16")",
                s->co->offset, buflen);
        goto end;
    }

    if (s->co->depth > buflen) {
        SCLogDebug("s->co->depth (%"PRIu16") > buflen (%"PRIu16")",
                s->co->depth, buflen);
        goto end;
    }

    uint8_t *sbuf = buf + s->co->offset;
    uint16_t sbuflen = s->co->depth - s->co->offset;
    SCLogDebug("s->co->offset (%"PRIu16") s->co->depth (%"PRIu16")",
                s->co->offset, s->co->depth);

    uint8_t *found = SpmSearch(sbuf, sbuflen, s->co->content, s->co->content_len);
    if (found != NULL) {
        proto = s->proto;
    }

end:
    SCReturnInt(proto);
}

/**
 *  \brief Add a proto detection string to the detection ctx.
 *
 *  \param ctx The detection ctx
 *  \param ip_proto The IP proto (TCP, UDP, etc)
 *  \param al_proto Application layer proto
 *  \param content A content string in the 'content:"some|20|string"' format.
 *  \param depth Depth setting for the content. E.g. 4 means that the content has to match in the first 4 bytes of the stream.
 *  \param offset Offset setting for the content. E.g. 4 mean that the content has to match after the first 4 bytes of the stream.
 *  \param flags Set STREAM_TOCLIENT or STREAM_TOSERVER for the direction in which to try to match the content.
 */
void AlpProtoAdd(AlpProtoDetectCtx *ctx, uint16_t ip_proto, uint16_t al_proto, char *content, uint16_t depth, uint16_t offset, uint8_t flags) {
    DetectContentData *cd = DetectContentParse(content);
    if (cd == NULL) {
        return;
    }
    cd->depth = depth;
    cd->offset = offset;

    cd->id = DetectContentGetId(ctx->mpm_pattern_id_store, cd);

    //PrintRawDataFp(stdout,cd->content,cd->content_len);
    SCLogDebug("cd->depth %"PRIu16" and cd->offset %"PRIu16" cd->id  %"PRIu32"",
            cd->depth, cd->offset, cd->id);

    AlpProtoDetectDirection *dir;
    if (flags & STREAM_TOCLIENT) {
        dir = &ctx->toclient;
    } else {
        dir = &ctx->toserver;
    }

    mpm_table[dir->mpm_ctx.mpm_type].AddPattern(&dir->mpm_ctx, cd->content, cd->content_len,
                                cd->offset, cd->depth, cd->id, cd->id, 0);
    dir->map[dir->id] = al_proto;
    dir->id++;

    if (depth > dir->max_len)
        dir->max_len = depth;

    /* set the min_len for the stream engine to set the min smsg size for app
       layer*/
    if (depth < dir->min_len)
        dir->min_len = depth;

    /* finally turn into a signature and add to the ctx */
    AlpProtoAddSignature(ctx, cd, ip_proto, al_proto);
}

#ifdef UNITTESTS
static void AlpProtoTestDestroy(AlpProtoDetectCtx *ctx) {
    mpm_table[ctx->toserver.mpm_ctx.mpm_type].DestroyCtx(&ctx->toserver.mpm_ctx);
    mpm_table[ctx->toclient.mpm_ctx.mpm_type].DestroyCtx(&ctx->toclient.mpm_ctx);
    AlpProtoFreeSignature(ctx->head);
}
#endif

void AlpProtoDestroy() {
    SCEnter();
    mpm_table[alp_proto_ctx.toserver.mpm_ctx.mpm_type].DestroyCtx(&alp_proto_ctx.toserver.mpm_ctx);
    mpm_table[alp_proto_ctx.toclient.mpm_ctx.mpm_type].DestroyCtx(&alp_proto_ctx.toclient.mpm_ctx);
    MpmPatternIdTableFreeHash(alp_proto_ctx.mpm_pattern_id_store);
    SCReturn;
}

void AlpProtoFinalizeThread(AlpProtoDetectCtx *ctx, AlpProtoDetectThreadCtx *tctx) {
    uint32_t sig_maxid = 0;
    uint32_t pat_maxid = ctx->mpm_pattern_id_store ? ctx->mpm_pattern_id_store->max_id : 0;

    memset(tctx, 0x00, sizeof(AlpProtoDetectThreadCtx));

    if (ctx->toclient.id > 0) {
        //sig_maxid = ctx->toclient.id;
        mpm_table[ctx->toclient.mpm_ctx.mpm_type].InitThreadCtx(&ctx->toclient.mpm_ctx, &tctx->toclient.mpm_ctx, sig_maxid);
        PmqSetup(&tctx->toclient.pmq, sig_maxid, pat_maxid);
    }
    if (ctx->toserver.id > 0) {
        //sig_maxid = ctx->toserver.id;
        mpm_table[ctx->toserver.mpm_ctx.mpm_type].InitThreadCtx(&ctx->toserver.mpm_ctx, &tctx->toserver.mpm_ctx, sig_maxid);
        PmqSetup(&tctx->toserver.pmq, sig_maxid, pat_maxid);
    }

}

void AlpProtoDeFinalize2Thread(AlpProtoDetectThreadCtx *tctx) {
    if (alp_proto_ctx.toclient.id > 0) {
        mpm_table[alp_proto_ctx.toclient.mpm_ctx.mpm_type].DestroyThreadCtx
                    (&alp_proto_ctx.toclient.mpm_ctx, &tctx->toclient.mpm_ctx);
        PmqFree(&tctx->toclient.pmq);
    }
    if (alp_proto_ctx.toserver.id > 0) {
        mpm_table[alp_proto_ctx.toserver.mpm_ctx.mpm_type].DestroyThreadCtx
                    (&alp_proto_ctx.toserver.mpm_ctx, &tctx->toserver.mpm_ctx);
        PmqFree(&tctx->toserver.pmq);
    }

}
/** \brief to be called by ReassemblyThreadInit
 *  \todo this is a hack, we need a proper place to store the global ctx */
void AlpProtoFinalize2Thread(AlpProtoDetectThreadCtx *tctx) {
    return AlpProtoFinalizeThread(&alp_proto_ctx, tctx);
}

void AlpProtoFinalizeGlobal(AlpProtoDetectCtx *ctx) {
    if (ctx == NULL)
        return;

    mpm_table[ctx->toclient.mpm_ctx.mpm_type].Prepare(&ctx->toclient.mpm_ctx);
    mpm_table[ctx->toserver.mpm_ctx.mpm_type].Prepare(&ctx->toserver.mpm_ctx);

    /* tell the stream reassembler, that initially we only want chunks of size
       min_len */
    StreamMsgQueueSetMinInitChunkLen(FLOW_PKT_TOCLIENT, ctx->toclient.min_len);
    StreamMsgQueueSetMinInitChunkLen(FLOW_PKT_TOSERVER, ctx->toserver.min_len);

    /* allocate and initialize the mapping between pattern id and signature */
    ctx->map = (AlpProtoSignature **)SCMalloc(ctx->sigs * sizeof(AlpProtoSignature *));
    if (ctx->map == NULL) {
        SCLogError(SC_ERR_MEM_ALLOC, "%s", strerror(errno));
        return;
    }
    memset(ctx->map, 0x00, ctx->sigs * sizeof(AlpProtoSignature *));

    AlpProtoSignature *s = ctx->head;
    AlpProtoSignature *temp = NULL;
    for ( ; s != NULL; s = s->next) {
        BUG_ON(s->co == NULL);

        if (ctx->map[s->co->id] == NULL) {
            ctx->map[s->co->id] = s;
        } else {
            temp = ctx->map[s->co->id];
            while (temp->map_next != NULL)
                temp = temp->map_next;
            temp->map_next = s;
        }
    }
}

void AppLayerDetectProtoThreadInit(void) {
    AlpProtoInit(&alp_proto_ctx);

    /** \todo register these in the protocol parser api */

    /** HTTP */
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_HTTP, "GET|20|", 4, 0, STREAM_TOSERVER);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_HTTP, "GET|09|", 4, 0, STREAM_TOSERVER);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_HTTP, "PUT|20|", 4, 0, STREAM_TOSERVER);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_HTTP, "PUT|09|", 4, 0, STREAM_TOSERVER);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_HTTP, "POST|20|", 5, 0, STREAM_TOSERVER);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_HTTP, "POST|09|", 5, 0, STREAM_TOSERVER);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_HTTP, "HEAD|20|", 5, 0, STREAM_TOSERVER);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_HTTP, "HEAD|09|", 5, 0, STREAM_TOSERVER);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_HTTP, "TRACE|20|", 6, 0, STREAM_TOSERVER);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_HTTP, "TRACE|09|", 6, 0, STREAM_TOSERVER);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_HTTP, "OPTIONS|20|", 8, 0, STREAM_TOSERVER);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_HTTP, "OPTIONS|09|", 8, 0, STREAM_TOSERVER);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_HTTP, "CONNECT|20|", 8, 0, STREAM_TOSERVER);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_HTTP, "CONNECT|09|", 8, 0, STREAM_TOSERVER);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_HTTP, "HTTP/", 5, 0, STREAM_TOCLIENT);

    /** SSH */
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_SSH, "SSH-", 4, 0, STREAM_TOCLIENT);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_SSH, "SSH-", 4, 0, STREAM_TOSERVER);

    /** SSLv2 */
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_SSL, "|01 00 02|", 5, 2, STREAM_TOSERVER);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_SSL, "|00 02|", 7, 5, STREAM_TOCLIENT);

    /** SSLv3 */
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_TLS, "|01 03 00|", 3, 0, STREAM_TOSERVER);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_TLS, "|16 03 00|", 3, 0, STREAM_TOSERVER); /* client hello */
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_TLS, "|16 03 00|", 3, 0, STREAM_TOCLIENT); /* server hello */
    /** TLSv1 */
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_TLS, "|01 03 01|", 3, 0, STREAM_TOSERVER);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_TLS, "|16 03 01|", 3, 0, STREAM_TOSERVER); /* client hello */
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_TLS, "|16 03 01|", 3, 0, STREAM_TOCLIENT); /* server hello */
    /** TLSv1.1 */
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_TLS, "|01 03 02|", 3, 0, STREAM_TOSERVER);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_TLS, "|16 03 02|", 3, 0, STREAM_TOSERVER); /* client hello */
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_TLS, "|16 03 02|", 3, 0, STREAM_TOCLIENT); /* server hello */
    /** TLSv1.2 */
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_TLS, "|01 03 03|", 3, 0, STREAM_TOSERVER);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_TLS, "|16 03 03|", 3, 0, STREAM_TOSERVER); /* client hello */
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_TLS, "|16 03 03|", 3, 0, STREAM_TOCLIENT); /* server hello */

    /** IMAP */
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_IMAP, "|2A 20|OK|20|", 5, 0, STREAM_TOCLIENT);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_IMAP, "1|20|capability", 12, 0, STREAM_TOSERVER);

    /** SMTP */
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_SMTP, "EHLO ", 5, 0, STREAM_TOCLIENT);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_SMTP, "HELO ", 5, 0, STREAM_TOCLIENT);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_SMTP, "ESMTP ", 64, 4, STREAM_TOSERVER);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_SMTP, "SMTP ", 64, 4, STREAM_TOSERVER);

    /** FTP */
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_FTP, "USER ", 5, 0, STREAM_TOSERVER);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_FTP, "PASS ", 5, 0, STREAM_TOSERVER);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_FTP, "PORT ", 5, 0, STREAM_TOSERVER);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_FTP, "AUTH SSL", 8, 0, STREAM_TOCLIENT);

    /** MSN Messenger */
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_MSN, "MSNP", 10, 6, STREAM_TOCLIENT);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_MSN, "MSNP", 10, 6, STREAM_TOSERVER);

    /** Jabber */
    //AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_JABBER, "xmlns='jabber|3A|client'", 74, 53, STREAM_TOCLIENT);
    //AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_JABBER, "xmlns='jabber|3A|client'", 74, 53, STREAM_TOSERVER);

    /** SMB */
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_SMB, "|ff|SMB", 8, 4, STREAM_TOCLIENT);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_SMB, "|ff|SMB", 8, 4, STREAM_TOSERVER);

    /** SMB2 */
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_SMB2, "|fe|SMB", 8, 4, STREAM_TOCLIENT);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_SMB2, "|fe|SMB", 8, 4, STREAM_TOSERVER);

    /** DCERPC */
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_UDP, ALPROTO_DCERPC_UDP, "|04 00|", 2, 0, STREAM_TOCLIENT);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_UDP, ALPROTO_DCERPC_UDP, "|04 00|", 2, 0, STREAM_TOSERVER);

    /** DCERPC */
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_DCERPC, "|05 00|", 2, 0, STREAM_TOCLIENT);
    AlpProtoAdd(&alp_proto_ctx, IPPROTO_TCP, ALPROTO_DCERPC, "|05 00|", 2, 0, STREAM_TOSERVER);

    AlpProtoFinalizeGlobal(&alp_proto_ctx);
}

/**
 *  \brief Get the app layer proto based on a buffer
 *
 *  \param ctx Global app layer detection context
 *  \param tctx Thread app layer detection context
 *  \param buf Pointer to the buffer to inspect
 *  \param buflen Lenght of the buffer
 *  \param flags Flags.
 *
 *  \retval proto App Layer proto, or ALPROTO_UNKNOWN if unknown
 */
uint16_t AppLayerDetectGetProto(AlpProtoDetectCtx *ctx, AlpProtoDetectThreadCtx *tctx, uint8_t *buf, uint16_t buflen, uint8_t flags, uint8_t ipproto) {
    SCEnter();

    AlpProtoDetectDirection *dir;
    AlpProtoDetectDirectionThread *tdir;

    if (flags & FLOW_AL_STREAM_TOSERVER) {
        dir = &ctx->toserver;
        tdir = &tctx->toserver;
    } else {
        dir = &ctx->toclient;
        tdir = &tctx->toclient;
    }

    if (dir->id == 0) {
        SCReturnUInt(ALPROTO_UNKNOWN);
    }

    /* see if we can limit the data we inspect */
    uint16_t searchlen = buflen;
    if (searchlen > dir->max_len)
        searchlen = dir->max_len;

    uint16_t proto = ALPROTO_UNKNOWN;
    uint32_t cnt = 0;

    /* do the mpm search */
    cnt = mpm_table[dir->mpm_ctx.mpm_type].Search(&dir->mpm_ctx,
                                                &tdir->mpm_ctx,
                                                &tdir->pmq, buf,
                                                searchlen);
    SCLogDebug("search cnt %" PRIu32 "", cnt);
    if (cnt == 0) {
        proto = ALPROTO_UNKNOWN;
        goto end;
    }

    /* We just work with the first match */
    uint16_t patid = tdir->pmq.pattern_id_array[0];
    SCLogDebug("array count is %"PRIu32" patid %"PRIu16"",
            tdir->pmq.pattern_id_array_cnt, patid);

    AlpProtoSignature *s = ctx->map[patid];
    if (s == NULL) {
        goto end;
    }
    uint8_t s_cnt = 1;

    while (proto == ALPROTO_UNKNOWN && s != NULL) {
        proto = AlpProtoMatchSignature(s, buf, buflen, ipproto);

        s = s->map_next;
        if (s == NULL && s_cnt < tdir->pmq.pattern_id_array_cnt) {
            patid = tdir->pmq.pattern_id_array[s_cnt];
            s = ctx->map[patid];
            s_cnt++;
        }
    }

end:
    PmqReset(&tdir->pmq);

    if (mpm_table[dir->mpm_ctx.mpm_type].Cleanup != NULL) {
        mpm_table[dir->mpm_ctx.mpm_type].Cleanup(&tdir->mpm_ctx);
    }
#if 0
    printf("AppLayerDetectGetProto: returning %" PRIu16 " (%s): ", proto, flags & STREAM_TOCLIENT ? "TOCLIENT" : "TOSERVER");
    switch (proto) {
        case ALPROTO_HTTP:
            printf("HTTP: ");
            /* print the first 32 bytes */
            if (buflen > 0) {
                PrintRawUriFp(stdout,buf,(buflen>32)?32:buflen);
            }
            printf("\n");
            break;
        case ALPROTO_FTP:
            printf("FTP\n");
            break;
        case ALPROTO_SSL:
            printf("SSL\n");
            break;
        case ALPROTO_SSH:
            printf("SSH\n");
            break;
        case ALPROTO_TLS:
            printf("TLS\n");
            break;
        case ALPROTO_IMAP:
            printf("IMAP\n");
            break;
        case ALPROTO_SMTP:
            printf("SMTP\n");
            break;
        case ALPROTO_JABBER:
            printf("JABBER\n");
            break;
        case ALPROTO_MSN:
            printf("MSN\n");
            break;
        case ALPROTO_SMB:
            printf("SMB\n");
            break;
        case ALPROTO_SMB2:
            printf("SMB2\n");
            break;
        case ALPROTO_DCERPC:
            printf("DCERPC\n");
            break;
        case ALPROTO_UNKNOWN:
        default:
            printf("UNKNOWN (%u): cnt was %u (", proto, cnt);
            /* print the first 32 bytes */
            if (buflen > 0) {
                PrintRawUriFp(stdout,buf,(buflen>32)?32:buflen);
            }
            printf(")\n");
            break;
    }
#endif
    SCReturnUInt(proto);
}

/* VJ Originally I thought of having separate app layer
 * handling threads, leaving this here in case we'll revisit that */
#if 0
void *AppLayerDetectProtoThread(void *td)
{
    ThreadVars *tv = (ThreadVars *)td;
    char run = TRUE;

    /* get the stream msg queue for this thread */
    StreamMsgQueue *stream_q = StreamMsgQueueGetByPort(0);
    /* set the minimum size we expect */
    StreamMsgQueueSetMinInitChunkLen(FLOW_PKT_TOSERVER, INSPECT_BYTES);
    StreamMsgQueueSetMinInitChunkLen(FLOW_PKT_TOCLIENT, INSPECT_BYTES);

    TmThreadsSetFlag(tv, THV_INIT_DONE);

    /* main loop */
    while(run) {
        TmThreadTestThreadUnPaused(tv);

        /* grab a msg, can return NULL on signals */
        StreamMsg *smsg = StreamMsgGetFromQueue(stream_q);
        if (smsg != NULL) {
            AppLayerHandleMsg(smsg, TRUE);
        }

        if (TmThreadsCheckFlag(tv, THV_KILL)) {
            SCPerfUpdateCounterArray(tv->sc_perf_pca, &tv->sc_perf_pctx, 0);
            run = 0;
        }
    }

    pthread_exit((void *) 0);
}

void AppLayerDetectProtoThreadSpawn()
{
    ThreadVars *tv_applayerdetect = NULL;

    tv_applayerdetect = TmThreadCreateMgmtThread("AppLayerDetectProtoThread",
                                                 AppLayerDetectProtoThread, 0);
    if (tv_applayerdetect == NULL) {
        printf("ERROR: TmThreadsCreate failed\n");
        exit(1);
    }
    if (TmThreadSpawn(tv_applayerdetect) != TM_ECODE_OK) {
        printf("ERROR: TmThreadSpawn failed\n");
        exit(1);
    }

#ifdef DEBUG
    printf("AppLayerDetectProtoThread thread created\n");
#endif
    return;
}
#endif
#ifdef UNITTESTS

int AlpDetectTest01(void) {
    char *buf = SCStrdup("HTTP");
    int r = 1;
    AlpProtoDetectCtx ctx;

    AlpProtoInit(&ctx);

    AlpProtoAdd(&ctx, IPPROTO_TCP, ALPROTO_HTTP, buf, 4, 0, STREAM_TOCLIENT);
    SCFree(buf);

    if (ctx.toclient.id != 1) {
        r = 0;
    }

    buf = SCStrdup("GET");
    AlpProtoAdd(&ctx, IPPROTO_TCP, ALPROTO_HTTP, buf, 4, 0, STREAM_TOSERVER);
    if (ctx.toserver.id != 1) {
        r = 0;
    }
    SCFree(buf);

    AlpProtoTestDestroy(&ctx);

    return r;
}

int AlpDetectTest02(void) {
    char *buf = SCStrdup("HTTP");
    int r = 1;
    AlpProtoDetectCtx ctx;

    AlpProtoInit(&ctx);

    AlpProtoAdd(&ctx, IPPROTO_TCP, ALPROTO_HTTP, buf, 4, 0, STREAM_TOCLIENT);
    SCFree(buf);

    if (ctx.toclient.id != 1) {
        r = 0;
    }

    if (ctx.toclient.map[ctx.toclient.id - 1] != ALPROTO_HTTP) {
        r = 0;
    }

    buf = SCStrdup("220 ");
    AlpProtoAdd(&ctx, IPPROTO_TCP, ALPROTO_FTP, buf, 4, 0, STREAM_TOCLIENT);
    SCFree(buf);

    if (ctx.toclient.id != 2) {
        r = 0;
    }

    if (ctx.toclient.map[ctx.toclient.id - 1] != ALPROTO_FTP) {
        r = 0;
    }

    AlpProtoTestDestroy(&ctx);

    return r;
}

int AlpDetectTest03(void) {
    uint8_t l7data[] = "HTTP/1.1 200 OK\r\nServer: Apache/1.0\r\n\r\n";
    char *buf = SCStrdup("HTTP");
    int r = 1;
    AlpProtoDetectCtx ctx;
    AlpProtoDetectThreadCtx tctx;


    AlpProtoInit(&ctx);

    AlpProtoAdd(&ctx, IPPROTO_TCP, ALPROTO_HTTP, buf, 4, 0, STREAM_TOCLIENT);
    SCFree(buf);

    if (ctx.toclient.id != 1) {
        r = 0;
    }

    if (ctx.toclient.map[ctx.toclient.id - 1] != ALPROTO_HTTP) {
        r = 0;
    }

    buf = SCStrdup("220 ");
    AlpProtoAdd(&ctx, IPPROTO_TCP, ALPROTO_FTP, buf, 4, 0, STREAM_TOCLIENT);
    SCFree(buf);

    if (ctx.toclient.id != 2) {
        r = 0;
    }

    if (ctx.toclient.map[ctx.toclient.id - 1] != ALPROTO_FTP) {
        r = 0;
    }

    AlpProtoFinalizeGlobal(&ctx);
    AlpProtoFinalizeThread(&ctx, &tctx);

    uint32_t cnt = mpm_table[ctx.toclient.mpm_ctx.mpm_type].Search(&ctx.toclient.mpm_ctx, &tctx.toclient.mpm_ctx, NULL, l7data, sizeof(l7data));
    if (cnt != 1) {
        printf("cnt %u != 1: ", cnt);
        r = 0;
    }

    AlpProtoTestDestroy(&ctx);

    return r;
}

int AlpDetectTest04(void) {
    uint8_t l7data[] = "HTTP/1.1 200 OK\r\nServer: Apache/1.0\r\n\r\n";
    char *buf = SCStrdup("200 ");
    int r = 1;
    AlpProtoDetectCtx ctx;
    AlpProtoDetectThreadCtx tctx;

    AlpProtoInit(&ctx);

    AlpProtoAdd(&ctx, IPPROTO_TCP, ALPROTO_HTTP, buf, 4, 0, STREAM_TOCLIENT);
    SCFree(buf);

    if (ctx.toclient.id != 1) {
        r = 0;
    }

    if (ctx.toclient.map[ctx.toclient.id - 1] != ALPROTO_HTTP) {
        r = 0;
    }

    AlpProtoFinalizeGlobal(&ctx);
    AlpProtoFinalizeThread(&ctx, &tctx);

    uint32_t cnt = mpm_table[ctx.toclient.mpm_ctx.mpm_type].Search(&ctx.toclient.mpm_ctx, &tctx.toclient.mpm_ctx, &tctx.toclient.pmq, l7data, sizeof(l7data));
    if (cnt != 1) {
        printf("cnt %u != 1: ", cnt);
        r = 0;
    }

    AlpProtoTestDestroy(&ctx);

    return r;
}

int AlpDetectTest05(void) {
    uint8_t l7data[] = "HTTP/1.1 200 OK\r\nServer: Apache/1.0\r\n\r\n<HTML><BODY>Blahblah</BODY></HTML>";
    char *buf = SCStrdup("HTTP");
    int r = 1;

    AlpProtoDetectCtx ctx;
    AlpProtoDetectThreadCtx tctx;

    AlpProtoInit(&ctx);

    AlpProtoAdd(&ctx, IPPROTO_TCP, ALPROTO_HTTP, buf, 4, 0, STREAM_TOCLIENT);
    SCFree(buf);

    if (ctx.toclient.id != 1) {
        r = 0;
    }

    if (ctx.toclient.map[ctx.toclient.id - 1] != ALPROTO_HTTP) {
        r = 0;
    }

    buf = SCStrdup("220 ");
    AlpProtoAdd(&ctx, IPPROTO_TCP, ALPROTO_FTP, buf, 4, 0, STREAM_TOCLIENT);
    SCFree(buf);

    if (ctx.toclient.id != 2) {
        r = 0;
    }

    if (ctx.toclient.map[ctx.toclient.id - 1] != ALPROTO_FTP) {
        r = 0;
    }

    AlpProtoFinalizeGlobal(&ctx);
    AlpProtoFinalizeThread(&ctx, &tctx);

    uint8_t proto = AppLayerDetectGetProto(&ctx, &tctx, l7data,sizeof(l7data), STREAM_TOCLIENT, IPPROTO_TCP);
    if (proto != ALPROTO_HTTP) {
        printf("proto %" PRIu8 " != %" PRIu8 ": ", proto, ALPROTO_HTTP);
        r = 0;
    }

    AlpProtoTestDestroy(&ctx);

    return r;
}

int AlpDetectTest06(void) {
    uint8_t l7data[] = "220 Welcome to the OISF FTP server\r\n";
    char *buf = SCStrdup("HTTP");
    int r = 1;
    AlpProtoDetectCtx ctx;
    AlpProtoDetectThreadCtx tctx;

    AlpProtoInit(&ctx);

    AlpProtoAdd(&ctx, IPPROTO_TCP, ALPROTO_HTTP, buf, 4, 0, STREAM_TOCLIENT);
    SCFree(buf);

    if (ctx.toclient.id != 1) {
        r = 0;
    }

    if (ctx.toclient.map[ctx.toclient.id - 1] != ALPROTO_HTTP) {
        r = 0;
    }

    buf = SCStrdup("220 ");
    AlpProtoAdd(&ctx, IPPROTO_TCP, ALPROTO_FTP, buf, 4, 0, STREAM_TOCLIENT);
    SCFree(buf);

    if (ctx.toclient.id != 2) {
        r = 0;
    }

    if (ctx.toclient.map[ctx.toclient.id - 1] != ALPROTO_FTP) {
        r = 0;
    }

    AlpProtoFinalizeGlobal(&ctx);
    AlpProtoFinalizeThread(&ctx, &tctx);

    uint8_t proto = AppLayerDetectGetProto(&ctx, &tctx, l7data,sizeof(l7data), STREAM_TOCLIENT, IPPROTO_TCP);
    if (proto != ALPROTO_FTP) {
        printf("proto %" PRIu8 " != %" PRIu8 ": ", proto, ALPROTO_FTP);
        r = 0;
    }

    AlpProtoTestDestroy(&ctx);

    return r;
}

int AlpDetectTest07(void) {
    uint8_t l7data[] = "220 Welcome to the OISF HTTP/FTP server\r\n";
    char *buf = SCStrdup("HTTP");
    int r = 1;
    AlpProtoDetectCtx ctx;
    AlpProtoDetectThreadCtx tctx;

    AlpProtoInit(&ctx);

    AlpProtoAdd(&ctx, IPPROTO_TCP, ALPROTO_HTTP, buf, 4, 0, STREAM_TOCLIENT);
    SCFree(buf);

    if (ctx.toclient.id != 1) {
        r = 0;
    }

    if (ctx.toclient.map[ctx.toclient.id - 1] != ALPROTO_HTTP) {
        r = 0;
    }

    AlpProtoFinalizeGlobal(&ctx);
    AlpProtoFinalizeThread(&ctx, &tctx);

    uint8_t proto = AppLayerDetectGetProto(&ctx, &tctx, l7data,sizeof(l7data), STREAM_TOCLIENT, IPPROTO_TCP);
    if (proto != ALPROTO_UNKNOWN) {
        printf("proto %" PRIu8 " != %" PRIu8 ": ", proto, ALPROTO_UNKNOWN);
        r = 0;
    }

    AlpProtoTestDestroy(&ctx);

    return r;
}

int AlpDetectTest08(void) {
    uint8_t l7data[] = "\x00\x00\x00\x85"  // NBSS
        "\xff\x53\x4d\x42\x72\x00\x00\x00" // SMB
        "\x00\x18\x53\xc8\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\xff\xfe\x00\x00\x00\x00"
        "\x00" // WordCount
        "\x62\x00" // ByteCount
        "\x02\x50\x43\x20\x4e\x45\x54\x57\x4f\x52\x4b\x20\x50\x52\x4f\x47\x52\x41\x4d\x20"
        "\x31\x2e\x30\x00\x02\x4c\x41\x4e\x4d\x41\x4e\x31\x2e\x30\x00\x02\x57\x69\x6e\x64\x6f\x77\x73"
        "\x20\x66\x6f\x72\x20\x57\x6f\x72\x6b\x67\x72\x6f\x75\x70\x73\x20\x33\x2e\x31\x61\x00\x02\x4c"
        "\x4d\x31\x2e\x32\x58\x30\x30\x32\x00\x02\x4c\x41\x4e\x4d\x41\x4e\x32\x2e\x31\x00\x02\x4e\x54"
        "\x20\x4c\x4d\x20\x30\x2e\x31\x32\x00";
    char *buf = SCStrdup("|ff|SMB");
    int r = 1;
    AlpProtoDetectCtx ctx;
    AlpProtoDetectThreadCtx tctx;

    AlpProtoInit(&ctx);

    AlpProtoAdd(&ctx, IPPROTO_TCP, ALPROTO_SMB, buf, 8, 4, STREAM_TOCLIENT);
    SCFree(buf);

    if (ctx.toclient.id != 1) {
        r = 0;
    }

    if (ctx.toclient.map[ctx.toclient.id - 1] != ALPROTO_SMB) {
        r = 0;
    }

    AlpProtoFinalizeGlobal(&ctx);
    AlpProtoFinalizeThread(&ctx, &tctx);

    uint8_t proto = AppLayerDetectGetProto(&ctx, &tctx, l7data,sizeof(l7data), STREAM_TOCLIENT, IPPROTO_TCP);
    if (proto != ALPROTO_SMB) {
        printf("proto %" PRIu8 " != %" PRIu8 ": ", proto, ALPROTO_SMB);
        r = 0;
    }

    AlpProtoTestDestroy(&ctx);

    return r;
}

int AlpDetectTest09(void) {
    uint8_t l7data[] =
        "\x00\x00\x00\x66" // NBSS
        "\xfe\x53\x4d\x42\x40\x00\x00\x00\x00\x00\x00\x00\x00\x00" // SMB2
        "\x3f\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00"
        "\x24\x00\x01\x00x00\x00\x00\x00\x00\x00\x0\x00\x00\x00\x00\x00\x00\x00\x00"
        "\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x02";

    char *buf = SCStrdup("|fe|SMB");
    int r = 1;
    AlpProtoDetectCtx ctx;
    AlpProtoDetectThreadCtx tctx;

    AlpProtoInit(&ctx);

    AlpProtoAdd(&ctx, IPPROTO_TCP, ALPROTO_SMB2, buf, 8, 4, STREAM_TOCLIENT);
    SCFree(buf);

    if (ctx.toclient.id != 1) {
        r = 0;
    }

    if (ctx.toclient.map[ctx.toclient.id - 1] != ALPROTO_SMB2) {
        r = 0;
    }

    AlpProtoFinalizeGlobal(&ctx);
    AlpProtoFinalizeThread(&ctx, &tctx);

    uint8_t proto = AppLayerDetectGetProto(&ctx, &tctx, l7data,sizeof(l7data), STREAM_TOCLIENT, IPPROTO_TCP);
    if (proto != ALPROTO_SMB2) {
        printf("proto %" PRIu8 " != %" PRIu8 ": ", proto, ALPROTO_SMB2);
        r = 0;
    }

    AlpProtoTestDestroy(&ctx);

    return r;
}

int AlpDetectTest10(void) {
    uint8_t l7data[] = "\x05\x00\x0b\x03\x10\x00\x00\x00\x48\x00\x00\x00"
        "\x00\x00\x00\x00\xd0\x16\xd0\x16\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00"
        "\x01\x00\xb8\x4a\x9f\x4d\x1c\x7d\xcf\x11\x86\x1e\x00\x20\xaf\x6e\x7c\x57"
        "\x00\x00\x00\x00\x04\x5d\x88\x8a\xeb\x1c\xc9\x11\x9f\xe8\x08\x00\x2b\x10"
        "\x48\x60\x02\x00\x00\x00";
    char *buf = SCStrdup("|05 00|");
    int r = 1;
    AlpProtoDetectCtx ctx;
    AlpProtoDetectThreadCtx tctx;

    AlpProtoInit(&ctx);

    AlpProtoAdd(&ctx, IPPROTO_TCP, ALPROTO_DCERPC, buf, 4, 0, STREAM_TOCLIENT);
    SCFree(buf);

    if (ctx.toclient.id != 1) {
        r = 0;
    }

    if (ctx.toclient.map[ctx.toclient.id - 1] != ALPROTO_DCERPC) {
        r = 0;
    }

    AlpProtoFinalizeGlobal(&ctx);
    AlpProtoFinalizeThread(&ctx, &tctx);

    uint8_t proto = AppLayerDetectGetProto(&ctx, &tctx, l7data,sizeof(l7data), STREAM_TOCLIENT, IPPROTO_TCP);
    if (proto != ALPROTO_DCERPC) {
        printf("proto %" PRIu8 " != %" PRIu8 ": ", proto, ALPROTO_DCERPC);
        r = 0;
    }

    AlpProtoTestDestroy(&ctx);

    return r;
}

/** \test why we still get http for connect... obviously because we also match on the reply, duh */
int AlpDetectTest11(void) {
    uint8_t l7data[] = "CONNECT www.ssllabs.com:443 HTTP/1.0\r\n";
    uint8_t l7data_resp[] = "HTTP/1.1 405 Method Not Allowed\r\n";
    int r = 1;
    AlpProtoDetectCtx ctx;
    AlpProtoDetectThreadCtx tctx;

    AlpProtoInit(&ctx);

    AlpProtoAdd(&ctx, IPPROTO_TCP, ALPROTO_HTTP, "HTTP", 4, 0, STREAM_TOSERVER);
    AlpProtoAdd(&ctx, IPPROTO_TCP, ALPROTO_HTTP, "GET", 3, 0, STREAM_TOSERVER);
    AlpProtoAdd(&ctx, IPPROTO_TCP, ALPROTO_HTTP, "PUT", 3, 0, STREAM_TOSERVER);
    AlpProtoAdd(&ctx, IPPROTO_TCP, ALPROTO_HTTP, "POST", 4, 0, STREAM_TOSERVER);
    AlpProtoAdd(&ctx, IPPROTO_TCP, ALPROTO_HTTP, "TRACE", 5, 0, STREAM_TOSERVER);
    AlpProtoAdd(&ctx, IPPROTO_TCP, ALPROTO_HTTP, "OPTIONS", 7, 0, STREAM_TOSERVER);
    AlpProtoAdd(&ctx, IPPROTO_TCP, ALPROTO_HTTP, "HTTP", 4, 0, STREAM_TOCLIENT);

    if (ctx.toserver.id != 6) {
        printf("ctx.toserver.id %u != 6: ", ctx.toserver.id);
        r = 0;
    }

    if (ctx.toserver.map[ctx.toserver.id - 1] != ALPROTO_HTTP) {
        printf("ctx.toserver.id %u != %u: ", ctx.toserver.map[ctx.toserver.id - 1],ALPROTO_HTTP);
        r = 0;
    }

    AlpProtoFinalizeGlobal(&ctx);
    AlpProtoFinalizeThread(&ctx, &tctx);

    uint8_t proto = AppLayerDetectGetProto(&ctx, &tctx, l7data, sizeof(l7data), STREAM_TOCLIENT, IPPROTO_TCP);
    if (proto == ALPROTO_HTTP) {
        printf("proto %" PRIu8 " == %" PRIu8 ": ", proto, ALPROTO_HTTP);
        r = 0;
    }

    proto = AppLayerDetectGetProto(&ctx, &tctx, l7data_resp, sizeof(l7data_resp), STREAM_TOSERVER, IPPROTO_TCP);
    if (proto != ALPROTO_HTTP) {
        printf("proto %" PRIu8 " != %" PRIu8 ": ", proto, ALPROTO_HTTP);
        r = 0;
    }

    AlpProtoTestDestroy(&ctx);
    return r;
}

/** \test AlpProtoSignature test */
int AlpDetectTest12(void) {
    AlpProtoDetectCtx ctx;
    int r = 0;

    AlpProtoInit(&ctx);
    AlpProtoAdd(&ctx, IPPROTO_TCP, ALPROTO_HTTP, "HTTP", 4, 0, STREAM_TOSERVER);
    AlpProtoFinalizeGlobal(&ctx);

    if (ctx.head == NULL) {
        printf("ctx.head == NULL: ");
        goto end;
    }

    if (ctx.head->proto != ALPROTO_HTTP) {
        printf("ctx.head->proto != ALPROTO_HTTP: ");
        goto end;
    }

    if (ctx.sigs != 1) {
        printf("ctx.sigs %"PRIu16", expected 1: ", ctx.sigs);
        goto end;
    }

    if (ctx.map == NULL) {
        printf("no mapping: ");
        goto end;
    }

    if (ctx.map[ctx.head->co->id] != ctx.head) {
        printf("wrong sig: ");
        goto end;
    }

    r = 1;
end:
    return r;
}

/**
 * \test What about if we add some sigs only for udp but call for tcp?
 *       It should not detect any proto
 */
int AlpDetectTest13(void) {
    uint8_t l7data[] = "CONNECT www.ssllabs.com:443 HTTP/1.0\r\n";
    uint8_t l7data_resp[] = "HTTP/1.1 405 Method Not Allowed\r\n";
    int r = 1;
    AlpProtoDetectCtx ctx;
    AlpProtoDetectThreadCtx tctx;

    AlpProtoInit(&ctx);

    AlpProtoAdd(&ctx, IPPROTO_UDP, ALPROTO_HTTP, "HTTP", 4, 0, STREAM_TOSERVER);
    AlpProtoAdd(&ctx, IPPROTO_UDP, ALPROTO_HTTP, "GET", 3, 0, STREAM_TOSERVER);
    AlpProtoAdd(&ctx, IPPROTO_UDP, ALPROTO_HTTP, "PUT", 3, 0, STREAM_TOSERVER);
    AlpProtoAdd(&ctx, IPPROTO_UDP, ALPROTO_HTTP, "POST", 4, 0, STREAM_TOSERVER);
    AlpProtoAdd(&ctx, IPPROTO_UDP, ALPROTO_HTTP, "TRACE", 5, 0, STREAM_TOSERVER);
    AlpProtoAdd(&ctx, IPPROTO_UDP, ALPROTO_HTTP, "OPTIONS", 7, 0, STREAM_TOSERVER);
    AlpProtoAdd(&ctx, IPPROTO_UDP, ALPROTO_HTTP, "HTTP", 4, 0, STREAM_TOCLIENT);

    if (ctx.toserver.id != 6) {
        printf("ctx.toserver.id %u != 6: ", ctx.toserver.id);
        r = 0;
    }

    if (ctx.toserver.map[ctx.toserver.id - 1] != ALPROTO_HTTP) {
        printf("ctx.toserver.id %u != %u: ", ctx.toserver.map[ctx.toserver.id - 1],ALPROTO_HTTP);
        r = 0;
    }

    AlpProtoFinalizeGlobal(&ctx);
    AlpProtoFinalizeThread(&ctx, &tctx);

    uint8_t proto = AppLayerDetectGetProto(&ctx, &tctx, l7data, sizeof(l7data), STREAM_TOCLIENT, IPPROTO_TCP);
    if (proto == ALPROTO_HTTP) {
        printf("proto %" PRIu8 " == %" PRIu8 ": ", proto, ALPROTO_HTTP);
        r = 0;
    }

    proto = AppLayerDetectGetProto(&ctx, &tctx, l7data_resp, sizeof(l7data_resp), STREAM_TOSERVER, IPPROTO_TCP);
    if (proto == ALPROTO_HTTP) {
        printf("proto %" PRIu8 " != %" PRIu8 ": ", proto, ALPROTO_HTTP);
        r = 0;
    }

    AlpProtoTestDestroy(&ctx);
    return r;
}

/**
 * \test What about if we add some sigs only for udp calling it for UDP?
 *       It should detect ALPROTO_HTTP (over udp). This is just a check
 *       to ensure that TCP/UDP differences work correctly.
 */
int AlpDetectTest14(void) {
    uint8_t l7data[] = "CONNECT www.ssllabs.com:443 HTTP/1.0\r\n";
    uint8_t l7data_resp[] = "HTTP/1.1 405 Method Not Allowed\r\n";
    int r = 1;
    AlpProtoDetectCtx ctx;
    AlpProtoDetectThreadCtx tctx;

    AlpProtoInit(&ctx);

    AlpProtoAdd(&ctx, IPPROTO_UDP, ALPROTO_HTTP, "HTTP", 4, 0, STREAM_TOSERVER);
    AlpProtoAdd(&ctx, IPPROTO_UDP, ALPROTO_HTTP, "GET", 3, 0, STREAM_TOSERVER);
    AlpProtoAdd(&ctx, IPPROTO_UDP, ALPROTO_HTTP, "PUT", 3, 0, STREAM_TOSERVER);
    AlpProtoAdd(&ctx, IPPROTO_UDP, ALPROTO_HTTP, "POST", 4, 0, STREAM_TOSERVER);
    AlpProtoAdd(&ctx, IPPROTO_UDP, ALPROTO_HTTP, "TRACE", 5, 0, STREAM_TOSERVER);
    AlpProtoAdd(&ctx, IPPROTO_UDP, ALPROTO_HTTP, "OPTIONS", 7, 0, STREAM_TOSERVER);
    AlpProtoAdd(&ctx, IPPROTO_UDP, ALPROTO_HTTP, "HTTP", 4, 0, STREAM_TOCLIENT);

    if (ctx.toserver.id != 6) {
        printf("ctx.toserver.id %u != 6: ", ctx.toserver.id);
        r = 0;
    }

    if (ctx.toserver.map[ctx.toserver.id - 1] != ALPROTO_HTTP) {
        printf("ctx.toserver.id %u != %u: ", ctx.toserver.map[ctx.toserver.id - 1],ALPROTO_HTTP);
        r = 0;
    }

    AlpProtoFinalizeGlobal(&ctx);
    AlpProtoFinalizeThread(&ctx, &tctx);

    uint8_t proto = AppLayerDetectGetProto(&ctx, &tctx, l7data, sizeof(l7data), STREAM_TOCLIENT, IPPROTO_UDP);
    if (proto == ALPROTO_HTTP) {
        printf("proto %" PRIu8 " == %" PRIu8 ": ", proto, ALPROTO_HTTP);
        r = 0;
    }

    proto = AppLayerDetectGetProto(&ctx, &tctx, l7data_resp, sizeof(l7data_resp), STREAM_TOSERVER, IPPROTO_UDP);
    if (proto != ALPROTO_HTTP) {
        printf("proto %" PRIu8 " != %" PRIu8 ": ", proto, ALPROTO_HTTP);
        r = 0;
    }

    AlpProtoTestDestroy(&ctx);
    return r;
}

#endif /* UNITTESTS */

void AlpDetectRegisterTests(void) {
#ifdef UNITTESTS
    UtRegisterTest("AlpDetectTest01", AlpDetectTest01, 1);
    UtRegisterTest("AlpDetectTest02", AlpDetectTest02, 1);
    UtRegisterTest("AlpDetectTest03", AlpDetectTest03, 1);
    UtRegisterTest("AlpDetectTest04", AlpDetectTest04, 1);
    UtRegisterTest("AlpDetectTest05", AlpDetectTest05, 1);
    UtRegisterTest("AlpDetectTest06", AlpDetectTest06, 1);
    UtRegisterTest("AlpDetectTest07", AlpDetectTest07, 1);
    UtRegisterTest("AlpDetectTest08", AlpDetectTest08, 1);
    UtRegisterTest("AlpDetectTest09", AlpDetectTest09, 1);
    UtRegisterTest("AlpDetectTest10", AlpDetectTest10, 1);
    UtRegisterTest("AlpDetectTest11", AlpDetectTest11, 1);
    UtRegisterTest("AlpDetectTest12", AlpDetectTest12, 1);
    UtRegisterTest("AlpDetectTest13", AlpDetectTest13, 1);
    UtRegisterTest("AlpDetectTest14", AlpDetectTest14, 1);
#endif /* UNITTESTS */
}
