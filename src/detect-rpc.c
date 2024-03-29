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
 * Implements RPC keyword
 */

#include "suricata-common.h"
#include "debug.h"
#include "decode.h"

#include "detect.h"
#include "detect-rpc.h"
#include "detect-parse.h"
#include "detect-engine.h"
#include "detect-engine-mpm.h"
#include "detect-engine-siggroup.h"
#include "detect-engine-address.h"

#include "util-unittest.h"
#include "util-debug.h"
#include "util-byte.h"

/**
 * \brief Regex for parsing our rpc options
 */
#define PARSE_REGEX  "^\\s*([0-9]{0,10})\\s*(?:,\\s*([0-9]{0,10}|[*])\\s*(?:,\\s*([0-9]{0,10}|[*]))?)?\\s*$"

static pcre *parse_regex;
static pcre_extra *parse_regex_study;

int DetectRpcMatch (ThreadVars *, DetectEngineThreadCtx *, Packet *, Signature *, SigMatch *);
int DetectRpcSetup (DetectEngineCtx *, Signature *, char *);
void DetectRpcRegisterTests(void);
void DetectRpcFree(void *);

/**
 * \brief Registration function for rpc keyword
 */
void DetectRpcRegister (void) {
    sigmatch_table[DETECT_RPC].name = "rpc";
    sigmatch_table[DETECT_RPC].Match = DetectRpcMatch;
    sigmatch_table[DETECT_RPC].Setup = DetectRpcSetup;
    sigmatch_table[DETECT_RPC].Free  = DetectRpcFree;
    sigmatch_table[DETECT_RPC].RegisterTests = DetectRpcRegisterTests;

    const char *eb;
    int eo;
    int opts = 0;

    parse_regex = pcre_compile(PARSE_REGEX, opts, &eb, &eo, NULL);
    if(parse_regex == NULL)
    {
        SCLogError(SC_ERR_PCRE_COMPILE, "pcre compile of \"%s\" failed at offset %" PRId32 ": %s", PARSE_REGEX, eo, eb);
        goto error;
    }

    parse_regex_study = pcre_study(parse_regex, 0, &eb);
    if(eb != NULL)
    {
        SCLogError(SC_ERR_PCRE_STUDY, "pcre study failed: %s", eb);
        goto error;
    }
    return;

error:
    /* XXX */
    return;
}

/*
 * returns 0: no match
 *         1: match
 *        -1: error
 */

/**
 * \brief This function is used to match rpc request set on a packet with those passed via rpc
 *
 * \param t pointer to thread vars
 * \param det_ctx pointer to the pattern matcher thread
 * \param p pointer to the current packet
 * \param m pointer to the sigmatch that we will cast into DetectRpcData
 *
 * \retval 0 no match
 * \retval 1 match
 */
int DetectRpcMatch (ThreadVars *t, DetectEngineThreadCtx *det_ctx, Packet *p, Signature *s, SigMatch *m)
{
    /* PrintRawDataFp(stdout, p->payload, p->payload_len); */
    DetectRpcData *rd = (DetectRpcData *)m->ctx;
    char *rpcmsg = (char *)p->payload;

    if (PKT_IS_TCP(p)) {
        /* if Rpc msg too small */
        if (p->payload_len < 28) {
            SCLogDebug("TCP packet to small for the rpc msg (%u)", p->payload_len);
            return 0;
        }
        rpcmsg += 4;
    } else if (PKT_IS_UDP(p)) {
        /* if Rpc msg too small */
        if (p->payload_len < 24) {
            SCLogDebug("UDP packet to small for the rpc msg (%u)", p->payload_len);
            return 0;
        }
    } else {
        SCLogDebug("No valid proto for the rpc message");
        return 0;
    }

    /* Point through the rpc msg structure. Use ntohl() to compare values */
    RpcMsg *msg = (RpcMsg *)rpcmsg;

    /* If its not a call, no match */
    if (ntohl(msg->type) != 0) {
        SCLogDebug("RPC message type is not a call");
        return 0;
    }

    if (ntohl(msg->prog) != rd->program)
        return 0;

    if (rd->flags & DETECT_RPC_CHECK_VERSION && ntohl(msg->vers) != rd->program_version)
        return 0;

    if (rd->flags & DETECT_RPC_CHECK_PROCEDURE && ntohl(msg->proc) != rd->procedure)
        return 0;

    SCLogDebug("prog:%u pver:%u proc:%u matched", ntohl(msg->prog), ntohl(msg->vers), ntohl(msg->proc));
    return 1;
}

/**
 * \brief This function is used to parse rpc options passed via rpc keyword
 *
 * \param rpcstr Pointer to the user provided rpc options
 *
 * \retval rd pointer to DetectRpcData on success
 * \retval NULL on failure
 */
DetectRpcData *DetectRpcParse (char *rpcstr)
{
    DetectRpcData *rd = NULL;
    char *args[3] = {NULL,NULL,NULL};
#define MAX_SUBSTRINGS 30
    int ret = 0, res = 0;
    int ov[MAX_SUBSTRINGS];

    ret = pcre_exec(parse_regex, parse_regex_study, rpcstr, strlen(rpcstr), 0, 0, ov, MAX_SUBSTRINGS);
    if (ret < 1 || ret > 4) {
        SCLogError(SC_ERR_PCRE_MATCH, "parse error, ret %" PRId32 ", string %s", ret, rpcstr);
        goto error;
    }
    if (ret > 1) {
        const char *str_ptr;
        res = pcre_get_substring((char *)rpcstr, ov, MAX_SUBSTRINGS, 1, &str_ptr);
        if (res < 0) {
            SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed");
            goto error;
        }
        args[0] = (char *)str_ptr;

        if (ret > 2) {
            res = pcre_get_substring((char *)rpcstr, ov, MAX_SUBSTRINGS, 2, &str_ptr);
            if (res < 0) {
                SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed");
                goto error;
            }
            args[1] = (char *)str_ptr;
        }
        if (ret > 3) {
            res = pcre_get_substring((char *)rpcstr, ov, MAX_SUBSTRINGS, 3, &str_ptr);
            if (res < 0) {
                SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed");
                goto error;
            }
            args[2] = (char *)str_ptr;
        }
    }

    rd = SCMalloc(sizeof(DetectRpcData));
    if (rd == NULL)
        goto error;
    rd->flags = 0;
    rd->program = 0;
    rd->program_version = 0;
    rd->procedure = 0;

    int i;
    for (i = 0; i < (ret -1); i++) {
        if (args[i]) {
            switch (i) {
                case 0:
                    if (ByteExtractStringUint32(&rd->program, 10, strlen(args[i]), args[i]) <= 0) {
                        SCLogError(SC_ERR_INVALID_ARGUMENT, "Invalid size specified for the rpc program:\"%s\"", args[i]);
                        goto error;
                    }
                    rd->flags |= DETECT_RPC_CHECK_PROGRAM;
                break;
                case 1:
                    if (args[i][0] != '*') {
                        if (ByteExtractStringUint32(&rd->program_version, 10, strlen(args[i]), args[i]) <= 0) {
                            SCLogError(SC_ERR_INVALID_ARGUMENT, "Invalid size specified for the rpc version:\"%s\"", args[i]);
                            goto error;
                        }
                        rd->flags |= DETECT_RPC_CHECK_VERSION;
                    }
                break;
                case 2:
                    if (args[i][0] != '*') {
                        if (ByteExtractStringUint32(&rd->procedure, 10, strlen(args[i]), args[i]) <= 0) {
                            SCLogError(SC_ERR_INVALID_ARGUMENT, "Invalid size specified for the rpc procedure:\"%s\"", args[i]);
                            goto error;
                        }
                        rd->flags |= DETECT_RPC_CHECK_PROCEDURE;
                    }
                break;
                }
            } else {
                SCLogError(SC_ERR_INVALID_VALUE, "invalid rpc option %s",args[i]);
                goto error;
            }
    }
    for (i = 0; i < (ret -1); i++){
        if (args[i] != NULL) SCFree(args[i]);
    }
    return rd;

error:
    for (i = 0; i < (ret -1); i++){
        if (args[i] != NULL) SCFree(args[i]);
    }
    if (rd != NULL) DetectRpcFree(rd);
    return NULL;

}

/**
 * \brief this function is used to add the parsed rpcdata into the current signature
 *
 * \param de_ctx pointer to the Detection Engine Context
 * \param s pointer to the Current Signature
 * \param m pointer to the Current SigMatch
 * \param rpcstr pointer to the user provided rpc options
 *
 * \retval 0 on Success
 * \retval -1 on Failure
 */
int DetectRpcSetup (DetectEngineCtx *de_ctx, Signature *s, char *rpcstr)
{
    DetectRpcData *rd = NULL;
    SigMatch *sm = NULL;

    rd = DetectRpcParse(rpcstr);
    if (rd == NULL) goto error;

    sm = SigMatchAlloc();
    if (sm == NULL)
        goto error;

    sm->type = DETECT_RPC;
    sm->ctx = (void *)rd;

    SigMatchAppendPacket(s, sm);

    return 0;

error:
    if (rd != NULL) DetectRpcFree(rd);
    if (sm != NULL) SCFree(sm);
    return -1;

}

/**
 * \brief this function will free memory associated with DetectRpcData
 *
 * \param rd pointer to DetectRpcData
 */
void DetectRpcFree(void *ptr) {
    SCEnter();

    if (ptr == NULL) {
        SCReturn;
    }

    DetectRpcData *rd = (DetectRpcData *)ptr;
    SCFree(rd);

    SCReturn;
}

#ifdef UNITTESTS
/**
 * \test DetectRpcTestParse01 is a test to make sure that we return "something"
 *  when given valid rpc opt
 */
int DetectRpcTestParse01 (void) {
    int result = 0;
    DetectRpcData *rd = NULL;
    rd = DetectRpcParse("123,444,555");
    if (rd != NULL) {
        DetectRpcFree(rd);
        result = 1;
    }

    return result;
}

/**
 * \test DetectRpcTestParse02 is a test for setting the established rpc opt
 */
int DetectRpcTestParse02 (void) {
    int result = 0;
    DetectRpcData *rd = NULL;
    rd = DetectRpcParse("111,222,333");
    if (rd != NULL) {
        if (rd->flags & DETECT_RPC_CHECK_PROGRAM &&
            rd->flags & DETECT_RPC_CHECK_VERSION &&
            rd->flags & DETECT_RPC_CHECK_PROCEDURE &&
            rd->program == 111 && rd->program_version == 222 &&
            rd->procedure == 333) {
            result = 1;
        } else {
            SCLogDebug("Error: Flags: %d; program: %u, version: %u, procedure: %u", rd->flags, rd->program, rd->program_version, rd->procedure);
        }
        DetectRpcFree(rd);
    }

    return result;
}

/**
 * \test DetectRpcTestParse03 is a test for checking the wildcards
 * and not specified fields
 */
int DetectRpcTestParse03 (void) {
    int result = 1;
    DetectRpcData *rd = NULL;
    rd = DetectRpcParse("111,*,333");
    if (rd == NULL)
        return 0;

    if ( !(rd->flags & DETECT_RPC_CHECK_PROGRAM &&
        !(rd->flags & DETECT_RPC_CHECK_VERSION) &&
        rd->flags & DETECT_RPC_CHECK_PROCEDURE &&
        rd->program == 111 && rd->program_version == 0 &&
        rd->procedure == 333))
            result = 0;
    SCLogDebug("rd1 Flags: %d; program: %u, version: %u, procedure: %u", rd->flags, rd->program, rd->program_version, rd->procedure);

    DetectRpcFree(rd);

    rd = DetectRpcParse("111,222,*");
    if (rd == NULL)
        return 0;

    if ( !(rd->flags & DETECT_RPC_CHECK_PROGRAM &&
        rd->flags & DETECT_RPC_CHECK_VERSION &&
        !(rd->flags & DETECT_RPC_CHECK_PROCEDURE) &&
        rd->program == 111 && rd->program_version == 222 &&
        rd->procedure == 0))
            result = 0;
    SCLogDebug("rd2 Flags: %d; program: %u, version: %u, procedure: %u", rd->flags, rd->program, rd->program_version, rd->procedure);

    DetectRpcFree(rd);

    rd = DetectRpcParse("111,*,*");
    if (rd == NULL)
        return 0;

    if ( !(rd->flags & DETECT_RPC_CHECK_PROGRAM &&
        !(rd->flags & DETECT_RPC_CHECK_VERSION) &&
        !(rd->flags & DETECT_RPC_CHECK_PROCEDURE) &&
        rd->program == 111 && rd->program_version == 0 &&
        rd->procedure == 0))
            result = 0;
    SCLogDebug("rd2 Flags: %d; program: %u, version: %u, procedure: %u", rd->flags, rd->program, rd->program_version, rd->procedure);

    DetectRpcFree(rd);

    rd = DetectRpcParse("111,222");
    if (rd == NULL)
        return 0;

    if ( !(rd->flags & DETECT_RPC_CHECK_PROGRAM &&
        rd->flags & DETECT_RPC_CHECK_VERSION &&
        !(rd->flags & DETECT_RPC_CHECK_PROCEDURE) &&
        rd->program == 111 && rd->program_version == 222 &&
        rd->procedure == 0))
            result = 0;
    SCLogDebug("rd2 Flags: %d; program: %u, version: %u, procedure: %u", rd->flags, rd->program, rd->program_version, rd->procedure);

    DetectRpcFree(rd);

    rd = DetectRpcParse("111");
    if (rd == NULL)
        return 0;

    if ( !(rd->flags & DETECT_RPC_CHECK_PROGRAM &&
        !(rd->flags & DETECT_RPC_CHECK_VERSION) &&
        !(rd->flags & DETECT_RPC_CHECK_PROCEDURE) &&
        rd->program == 111 && rd->program_version == 0 &&
        rd->procedure == 0))
            result = 0;
    SCLogDebug("rd2 Flags: %d; program: %u, version: %u, procedure: %u", rd->flags, rd->program, rd->program_version, rd->procedure);

    DetectRpcFree(rd);
    return result;
}

/**
 * \test DetectRpcTestParse04 is a test for check the discarding of empty options
 */
int DetectRpcTestParse04 (void) {
    int result = 0;
    DetectRpcData *rd = NULL;
    rd = DetectRpcParse("");
    if (rd == NULL) {
        result = 1;
    } else {
        SCLogDebug("Error: Flags: %d; program: %u, version: %u, procedure: %u", rd->flags, rd->program, rd->program_version, rd->procedure);
        DetectRpcFree(rd);
    }

    return result;
}

/**
 * \test DetectRpcTestParse05 is a test for check invalid values
 */
int DetectRpcTestParse05 (void) {
    int result = 0;
    DetectRpcData *rd = NULL;
    rd = DetectRpcParse("111,aaa,*");
    if (rd == NULL) {
        result = 1;
    } else {
        SCLogDebug("Error: Flags: %d; program: %u, version: %u, procedure: %u", rd->flags, rd->program, rd->program_version, rd->procedure);
        DetectRpcFree(rd);
    }

    return result;
}

/**
 * \test DetectRpcTestParse05 is a test to check the match function
 */
static int DetectRpcTestSig01(void) {
    /* RPC Call */
    uint8_t buf[] = {
        /* XID */
        0x64,0xb2,0xb3,0x75,
        /* Message type: Call (0) */
        0x00,0x00,0x00,0x00,
        /* RPC Version (2) */
        0x00,0x00,0x00,0x02,
        /* Program portmap (100000) */
        0x00,0x01,0x86,0xa0,
        /* Program version (2) */
        0x00,0x00,0x00,0x02,
        /* Program procedure (3) = GETPORT */
        0x00,0x00,0x00,0x03,
        /* AUTH_NULL */
        0x00,0x00,0x00,0x00,
        /* Length 0 */
        0x00,0x00,0x00,0x00,
        /* VERIFIER NULL */
        0x00,0x00,0x00,0x00,
        /* Length 0 */
        0x00,0x00,0x00,0x00,
        /* Program portmap */
        0x00,0x01,0x86,0xa2,
        /* Version 2 */
        0x00,0x00,0x00,0x02,
        /* Proto UDP */
        0x00,0x00,0x00,0x11,
        /* Port 0 */
        0x00,0x00,0x00,0x00 };
    uint16_t buflen = sizeof(buf);
    Packet p;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;
    int result = 0;

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    p.src.family = AF_INET;
    p.dst.family = AF_INET;
    p.payload = buf;
    p.payload_len = buflen;
    p.proto = IPPROTO_UDP;
    /** Be careful, this is just to match the macro PKT_IS_UDP! */
    p.udph = (void *)1;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert udp any any -> any any (msg:\"RPC Get Port Call\"; rpc:100000, 2, 3; sid:1;)");
    if (s == NULL) {
        goto end;
    }

    s = s->next = SigInit(de_ctx,"alert udp any any -> any any (msg:\"RPC Get Port Call\"; rpc:100000, 2, *; sid:2;)");
    if (s == NULL) {
        goto end;
    }

    s = s->next = SigInit(de_ctx,"alert udp any any -> any any (msg:\"RPC Get Port Call\"; rpc:100000, *, 3; sid:3;)");
    if (s == NULL) {
        goto end;
    }

    s = s->next = SigInit(de_ctx,"alert udp any any -> any any (msg:\"RPC Get Port Call\"; rpc:100000, *, *; sid:4;)");
    if (s == NULL) {
        goto end;
    }

    s = s->next = SigInit(de_ctx,"alert udp any any -> any any (msg:\"RPC Get XXX Call.. no match\"; rpc:123456, *, 3; sid:5;)");
    if (s == NULL) {
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);
    if (PacketAlertCheck(&p, 1) == 0) {
        printf("sid 1 didnt alert, but it should have: ");
        goto cleanup;
    } else if (PacketAlertCheck(&p, 2) == 0) {
        printf("sid 2 didnt alert, but it should have: ");
        goto cleanup;
    } else if (PacketAlertCheck(&p, 3) == 0) {
        printf("sid 3 didnt alert, but it should have: ");
        goto cleanup;
    } else if (PacketAlertCheck(&p, 4) == 0) {
        printf("sid 4 didnt alert, but it should have: ");
        goto cleanup;
    } else if (PacketAlertCheck(&p, 5) > 0) {
        printf("sid 5 did alert, but should not: ");
        goto cleanup;
    }

    result = 1;

cleanup:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    DetectEngineCtxFree(de_ctx);

    DetectSigGroupPrintMemory();
    DetectAddressPrintMemory();

end:
    return result;
}
#endif /* UNITTESTS */

/**
 * \brief this function registers unit tests for DetectRpc
 */
void DetectRpcRegisterTests(void) {
#ifdef UNITTESTS
    UtRegisterTest("DetectRpcTestParse01", DetectRpcTestParse01, 1);
    UtRegisterTest("DetectRpcTestParse02", DetectRpcTestParse02, 1);
    UtRegisterTest("DetectRpcTestParse03", DetectRpcTestParse03, 1);
    UtRegisterTest("DetectRpcTestParse04", DetectRpcTestParse04, 1);
    UtRegisterTest("DetectRpcTestParse05", DetectRpcTestParse05, 1);
    UtRegisterTest("DetectRpcTestSig01", DetectRpcTestSig01, 1);
#endif /* UNITTESTS */
}
