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
 * \author Pablo Rincon <pablo.rincon.crespo@gmail.com>
 *
 * ftpbounce keyword, part of the detection engine.
 */

#include "suricata-common.h"
#include "debug.h"
#include "decode.h"
#include "detect.h"
#include "detect-parse.h"
#include "detect-engine.h"
#include "detect-engine-mpm.h"
#include "detect-engine-state.h"
#include "detect-content.h"

#include "app-layer.h"
#include "app-layer-ftp.h"
#include "util-unittest.h"
#include "util-debug.h"
#include "flow.h"
#include "flow-var.h"
#include "flow-util.h"
#include "threads.h"
#include "detect-ftpbounce.h"
#include "stream-tcp.h"

int DetectFtpbounceMatch(ThreadVars *, DetectEngineThreadCtx *, Packet *,
                          Signature *, SigMatch *);
int DetectFtpbounceALMatch(ThreadVars *, DetectEngineThreadCtx *, Flow *,
                           uint8_t, void *, Signature *, SigMatch *);
static int DetectFtpbounceSetup(DetectEngineCtx *, Signature *, char *);
int DetectFtpbounceMatchArgs(uint8_t *payload, uint16_t payload_len,
                             uint32_t ip_orig, uint16_t offset);
void DetectFtpbounceRegisterTests(void);
void DetectFtpbounceFree(void *);

/**
 * \brief Registration function for ftpbounce: keyword
 * \todo add support for no_stream and stream_only
 */
void DetectFtpbounceRegister(void)
{
    sigmatch_table[DETECT_FTPBOUNCE].name = "ftpbounce";
    sigmatch_table[DETECT_FTPBOUNCE].Setup = DetectFtpbounceSetup;
    sigmatch_table[DETECT_FTPBOUNCE].Match = NULL;
    sigmatch_table[DETECT_FTPBOUNCE].AppLayerMatch = DetectFtpbounceALMatch;
    sigmatch_table[DETECT_FTPBOUNCE].alproto = ALPROTO_FTP;
    sigmatch_table[DETECT_FTPBOUNCE].Free  = NULL;
    sigmatch_table[DETECT_FTPBOUNCE].RegisterTests = DetectFtpbounceRegisterTests;
    return;
}

/**
 * \brief This function is used to match ftpbounce attacks
 *
 * \param payload Payload of the PORT command
 * \param payload_len Length of the payload
 * \param ip_orig IP source to check the ftpbounce condition
 * \param offset offset to the arguments of the PORT command
 *
 * \retval 1 if ftpbounce detected, 0 if not
 */
int DetectFtpbounceMatchArgs(uint8_t *payload, uint16_t payload_len,
                             uint32_t ip_orig, uint16_t offset)
{
    SCEnter();
    SCLogDebug("Checking ftpbounce condition");
    char *c = NULL;
    uint16_t i = 0;
    int octet = 0;
    int octet_ascii_len = 0;
    int noctet = 0;
    uint32_t ip = 0;
    /* PrintRawDataFp(stdout, payload, payload_len); */

    if (payload_len < 7) {
        /* we need at least a differet ip address
         * in the format 1,2,3,4,x,y where x,y is the port
         * in two byte representation so let's look at
         * least for the IP octets in comma separated */
        return 0;
    }

    if (offset + 7 >= payload_len)
        return 0;

    c =(char*) payload;
    if (c == NULL) {
        SCLogDebug("No payload to check");
        return 0;
    }

    i = offset;
    /* Search for the first IP octect(Skips "PORT ") */
    while (i < payload_len && !isdigit(c[i])) i++;

    for (;i < payload_len && octet_ascii_len < 4 ;i++) {
        if (isdigit(c[i])) {
            octet =(c[i] - '0') + octet * 10;
            octet_ascii_len++;
        } else {
            if (octet > 256) {
                SCLogDebug("Octet not in ip format");
                return 0;
            }

            if (isspace(c[i]))
                while (i < payload_len && isspace(c[i]) ) i++;

            if (i < payload_len && c[i] == ',') { /* we have an octet */
                noctet++;
                octet_ascii_len = 0;
                ip =(ip << 8) + octet;
                octet = 0;
            } else {
                SCLogDebug("Unrecognized character '%c'", c[i]);
                return 0;
            }
            if (noctet == 4) {
                /* Different IP than src, ftp bounce scan */
                if (ip != ntohl(ip_orig)) {
                    SCLogDebug("Different ip, so Matched ip:%d <-> ip_orig:%d",
                               ip, ip_orig);
                    return 1;
                }
                SCLogDebug("Same ip, so no match here");
                return 0;
            }
        }
    }
    SCLogDebug("No match");
    return 0;
}

/**
 * \brief This function is used to check matches from the FTP App Layer Parser
 *
 * \param t pointer to thread vars
 * \param det_ctx pointer to the pattern matcher thread
 * \param p pointer to the current packet
 * \param m pointer to the sigmatch but we don't use it since ftpbounce
 *          has no options
 * \retval 0 no match
 * \retval 1 match
 */
int DetectFtpbounceALMatch(ThreadVars *t, DetectEngineThreadCtx *det_ctx,
                           Flow *f, uint8_t flags, void *state, Signature *s,
                           SigMatch *m)
{
    SCEnter();
    FtpState *ftp_state =(FtpState *)state;
    if (ftp_state == NULL) {
        SCLogDebug("no ftp state, no match");
        SCReturnInt(0);
    }

    int ret = 0;
    SCMutexLock(&f->m);

    if (ftp_state->command == FTP_COMMAND_PORT) {
        ret = DetectFtpbounceMatchArgs(ftp_state->port_line,
                  ftp_state->port_line_len, f->src.address.address_un_data32[0],
                  ftp_state->arg_offset);
    }
    SCMutexUnlock(&f->m);

    SCReturnInt(ret);
}

/**
 * \brief This function is used to match ftpbounce attacks
 *
 * \param t pointer to thread vars
 * \param det_ctx pointer to the pattern matcher thread
 * \param p pointer to the current packet
 * \param m pointer to the sigmatch but we don't use it since ftpbounce
 *          has no options
 * \retval 0 no match, 1 if match
 */
int DetectFtpbounceMatch(ThreadVars *t, DetectEngineThreadCtx *det_ctx,
                          Packet *p, Signature *s, SigMatch *m)
{
/** \todo VJ broken and no longer used */
#if 0
    SCEnter();
    uint16_t offset = 0;
    if (!(PKT_IS_TCP(p)))
        return 0;

    SigMatch *sm = SigMatchGetLastSM(s->pmatch_tail, DETECT_CONTENT);
    if (sm == NULL)
        return 0;

    DetectContentData *co = sm->ctx;
    if (co == NULL)
        return 0;

    MpmMatch *mm = det_ctx->mtc.match[co->id].top;
    SCLogDebug("Starting Offset: %u",mm->offset + co->content_len);

    offset = mm->offset + co->content_len;
    SCLogDebug("Payload: \"%s\"\nLen: %u Offset: %u\n", p->payload,
               p->payload_len, offset);

    return DetectFtpbounceMatchArgs(p->payload, p->payload_len,
                                    p->src.addr_data32[0], offset);
#endif
    return 0;
}

/**
 * \brief this function is used to add the parsed ftpbounce
 *
 * \param de_ctx pointer to the Detection Engine Context
 * \param s pointer to the Current Signature
 * \param m pointer to the Current SigMatch
 * \param ftpbouncestr pointer to the user provided ftpbounce options
 *                     currently there are no options.
 *
 * \retval 0 on Success
 * \retval -1 on Failure
 */
int DetectFtpbounceSetup(DetectEngineCtx *de_ctx, Signature *s, char *ftpbouncestr)
{
    SCEnter();

    SigMatch *sm = NULL;

    sm = SigMatchAlloc();
    if (sm == NULL) {
        goto error;;
    }

    sm->type = DETECT_FTPBOUNCE;

    /* We don't need to allocate any data for ftpbounce here.
    *
    * TODO: As a suggestion, maybe we can add a flag in the flow
    * to set the stream as "bounce detected" for fast Match.
    * When you do a ftp bounce attack you usually use the same
    * communication control stream to "setup" various destinations
    * whithout breaking the connection, so I guess we can make it a bit faster
    * with a flow flag set lookup in the Match function.
    */
    sm->ctx = NULL;

    SigMatchAppendAppLayer(s, sm);

    if (s->alproto != ALPROTO_UNKNOWN && s->alproto != ALPROTO_FTP) {
        SCLogError(SC_ERR_CONFLICTING_RULE_KEYWORDS, "rule contains conflicting keywords.");
        goto error;
    }

    s->alproto = ALPROTO_FTP;
    SCReturnInt(0);

error:
    if (sm != NULL) {
        SigMatchFree(sm);
    }
    SCReturnInt(-1);
}

#ifdef UNITTESTS

/**
 * \test DetectFtpbounceTestSetup01 is a test for the Setup ftpbounce
 */
int DetectFtpbounceTestSetup01(void)
{
    int res = 0;
    DetectEngineCtx *de_ctx = NULL;
    Signature *s = SigAlloc();
    if (s == NULL)
        return 0;

    /* ftpbounce doesn't accept options so the str is NULL */
    res = !DetectFtpbounceSetup(de_ctx, s, NULL);
    res &= s->amatch != NULL && s->amatch->type & DETECT_FTPBOUNCE;

    SigFree(s);
    return res;
}

#include "stream-tcp-reassemble.h"

/**
 * \test Check the ftpbounce match, send a get request in three chunks
 * + more data.
 * \brief This test tests the ftpbounce condition match, based on the
 *   ftp layer parser
 */
static int DetectFtpbounceTestALMatch02(void) {
    int result = 0;

    uint8_t ftpbuf1[] = { 'P','O' };
    uint32_t ftplen1 = sizeof(ftpbuf1);
    uint8_t ftpbuf2[] = { 'R', 'T' };
    uint32_t ftplen2 = sizeof(ftpbuf2);
    uint8_t ftpbuf3[] = { ' ', '8','0',',','5' };
    uint32_t ftplen3 = sizeof(ftpbuf3);
    uint8_t ftpbuf4[] = "8,0,33,10,20\r\n";
    uint32_t ftplen4 = sizeof(ftpbuf4);

    TcpSession ssn;
    Flow f;
    Packet p;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p.src.family = AF_INET;
    p.dst.family = AF_INET;
    p.src.addr_data32[0] = 0x01020304;
    p.payload = NULL;
    p.payload_len = 0;
    p.proto = IPPROTO_TCP;

    FLOW_INITIALIZE(&f);
    f.src.address.address_un_data32[0]=0x01020304;
    f.protoctx =(void *)&ssn;

    p.flow = &f;
    p.flowflags |= FLOW_PKT_TOSERVER;
    p.flowflags |= FLOW_PKT_ESTABLISHED;
    f.alproto = ALPROTO_FTP;

    StreamTcpInitConfig(TRUE);
    FlowL7DataPtrInit(&f);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert tcp any any -> any any "
                                   "(msg:\"Ftp Bounce\"; ftpbounce; sid:1;)");
    if (s == NULL) {
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v,(void *)de_ctx,(void *)&det_ctx);

    FlowL7DataPtrInit(&f);
    int r = AppLayerParse(&f, ALPROTO_FTP, STREAM_TOSERVER, ftpbuf1, ftplen1);
    if (r != 0) {
        SCLogDebug("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    r = AppLayerParse(&f,ALPROTO_FTP, STREAM_TOSERVER, ftpbuf2, ftplen2);
    if (r != 0) {
        SCLogDebug("toserver chunk 2 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    r = AppLayerParse(&f,ALPROTO_FTP, STREAM_TOSERVER, ftpbuf3, ftplen3);
    if (r != 0) {
        SCLogDebug("toserver chunk 3 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    r = AppLayerParse(&f,ALPROTO_FTP, STREAM_TOSERVER, ftpbuf4, ftplen4);
    if (r != 0) {
        SCLogDebug("toserver chunk 4 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    FtpState *ftp_state = f.aldata[AlpGetStateIdx(ALPROTO_FTP)];
    if (ftp_state == NULL) {
        SCLogDebug("no ftp state: ");
        result = 0;
        goto end;
    }

    if (ftp_state->command != FTP_COMMAND_PORT) {
        SCLogDebug("expected command port not detected");
        result = 0;
        goto end;
    }

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);

    if (!(PacketAlertCheck(&p, 1))) {
        goto end;
    }

    result = 1;
end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v,(void *)det_ctx);
    DetectEngineCtxFree(de_ctx);

    FlowL7DataPtrFree(&f);
    StreamTcpFreeConfig(TRUE);
    FLOW_DESTROY(&f);
    return result;
}

/**
 * \test Check the ftpbounce match
 * \brief This test tests the ftpbounce condition match, based on
 *  the ftp layer parser
 */
static int DetectFtpbounceTestALMatch03(void) {
    int result = 0;

    uint8_t ftpbuf1[] = { 'P','O' };
    uint32_t ftplen1 = sizeof(ftpbuf1);
    uint8_t ftpbuf2[] = { 'R', 'T' };
    uint32_t ftplen2 = sizeof(ftpbuf2);
    uint8_t ftpbuf3[] = { ' ', '1',',','2',',' };
    uint32_t ftplen3 = sizeof(ftpbuf3);
    uint8_t ftpbuf4[] = "3,4,10,20\r\n";
    uint32_t ftplen4 = sizeof(ftpbuf4);

    TcpSession ssn;
    Flow f;
    Packet p;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p.src.family = AF_INET;
    p.dst.family = AF_INET;
    p.src.addr_data32[0] = 0x04030201;
    p.payload = NULL;
    p.payload_len = 0;
    p.proto = IPPROTO_TCP;

    FLOW_INITIALIZE(&f);
    f.src.address.address_un_data32[0]=0x04030201;
    f.protoctx =(void *)&ssn;

    p.flow = &f;
    p.flowflags |= FLOW_PKT_TOSERVER;
    p.flowflags |= FLOW_PKT_ESTABLISHED;
    f.alproto = ALPROTO_FTP;

    StreamTcpInitConfig(TRUE);
    FlowL7DataPtrInit(&f);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert tcp any any -> any any "
                                   "(msg:\"Ftp Bounce\"; ftpbounce; sid:1;)");
    if (s == NULL) {
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v,(void *)de_ctx,(void *)&det_ctx);

    int r = AppLayerParse(&f, ALPROTO_FTP, STREAM_TOSERVER, ftpbuf1, ftplen1);
    if (r != 0) {
        SCLogDebug("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    r = AppLayerParse(&f,ALPROTO_FTP, STREAM_TOSERVER, ftpbuf2, ftplen2);
    if (r != 0) {
        SCLogDebug("toserver chunk 2 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    r = AppLayerParse(&f,ALPROTO_FTP, STREAM_TOSERVER, ftpbuf3, ftplen3);
    if (r != 0) {
        SCLogDebug("toserver chunk 3 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    r = AppLayerParse(&f,ALPROTO_FTP, STREAM_TOSERVER, ftpbuf4, ftplen4);
    if (r != 0) {
        SCLogDebug("toserver chunk 4 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        goto end;
    }

    FtpState *ftp_state = f.aldata[AlpGetStateIdx(ALPROTO_FTP)];
    if (ftp_state == NULL) {
        SCLogDebug("no ftp state: ");
        result = 0;
        goto end;
    }

    if (ftp_state->command != FTP_COMMAND_PORT) {
        SCLogDebug("expected command port not detected");
        result = 0;
        goto end;
    }

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);

    /* It should not match */
    if (!(PacketAlertCheck(&p, 1))) {
        result = 1;
    } else {
        SCLogDebug("It should not match here!");
    }

end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v,(void *)det_ctx);
    DetectEngineCtxFree(de_ctx);

    FlowL7DataPtrFree(&f);
    StreamTcpFreeConfig(TRUE);
    FLOW_DESTROY(&f);
    return result;
}

#endif /* UNITTESTS */

/**
 * \brief this function registers unit tests for DetectFtpbounce
 */
void DetectFtpbounceRegisterTests(void)
{
#ifdef UNITTESTS
    UtRegisterTest("DetectFtpbounceTestSetup01", DetectFtpbounceTestSetup01, 1);
    UtRegisterTest("DetectFtpbounceTestALMatch02",
                   DetectFtpbounceTestALMatch02, 1);
    UtRegisterTest("DetectFtpbounceTestALMatch03",
                   DetectFtpbounceTestALMatch03, 1);
#endif /* UNITTESTS */
}
