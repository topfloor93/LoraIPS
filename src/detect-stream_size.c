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
 * \author Gurvinder Singh <gurvindersinghdahiya@gmail.com>
 *
 * Stream size for the engine.
 */

#include "suricata-common.h"
#include "stream-tcp.h"
#include "util-unittest.h"

#include "detect.h"
#include "detect-parse.h"

#include "flow.h"
#include "detect-stream_size.h"
#include "stream-tcp-private.h"
#include "util-debug.h"

/**
 * \brief Regex for parsing our flow options
 */
#define PARSE_REGEX  "^\\s*([A-z_]+)\\s*,\\s*([<=>!]+)\\s*,\\s*([0-9]+)\\s*$"

static pcre *parse_regex;
static pcre_extra *parse_regex_study;

/*prototypes*/
int DetectStreamSizeMatch (ThreadVars *, DetectEngineThreadCtx *, Packet *, Signature *, SigMatch *);
static int DetectStreamSizeSetup (DetectEngineCtx *, Signature *, char *);
void DetectStreamSizeFree(void *);
void DetectStreamSizeRegisterTests(void);

/**
 * \brief Registration function for stream_size: keyword
 */

void DetectStreamSizeRegister(void) {
    sigmatch_table[DETECT_STREAM_SIZE].name = "stream_size";
    sigmatch_table[DETECT_STREAM_SIZE].Match = DetectStreamSizeMatch;
    sigmatch_table[DETECT_STREAM_SIZE].Setup = DetectStreamSizeSetup;
    sigmatch_table[DETECT_STREAM_SIZE].Free = DetectStreamSizeFree;
    sigmatch_table[DETECT_STREAM_SIZE].RegisterTests = DetectStreamSizeRegisterTests;

    const char *eb;
    int eo;
    int opts = 0;

    parse_regex = pcre_compile(PARSE_REGEX, opts, &eb, &eo, NULL);
    if (parse_regex == NULL) {
        SCLogError(SC_ERR_PCRE_COMPILE, "pcre compile of \"%s\" failed at offset %" PRId32 ": %s", PARSE_REGEX, eo, eb);
        goto error;
    }

    parse_regex_study = pcre_study(parse_regex, 0, &eb);
    if (eb != NULL) {
        SCLogError(SC_ERR_PCRE_STUDY, "pcre study failed: %s", eb);
        goto error;
    }
    return;

error:
    if (parse_regex != NULL) SCFree(parse_regex);
    if (parse_regex_study != NULL) SCFree(parse_regex_study);
    return;
}

/**
 * \brief Function to comapre the stream size against defined size in the user
 *  options.
 *
 *  \param  diff    The stream size of server or client stream.
 *  \param  stream_size User defined stream size
 *  \param  mode    The mode defined by user.
 *
 *  \retval 1 on success and 0 on failure.
 */

static int DetectStreamSizeCompare (uint32_t diff, uint32_t stream_size, uint8_t mode) {

    int ret = 0;
    switch (mode) {
        case DETECTSSIZE_LT:
            if (diff < stream_size)
                ret = 1;
            break;
        case DETECTSSIZE_LEQ:
            if (diff <= stream_size)
                ret = 1;
            break;
        case DETECTSSIZE_EQ:
            if (diff == stream_size)
                ret = 1;
            break;
        case DETECTSSIZE_NEQ:
            if (diff != stream_size)
                ret = 1;
            break;
        case DETECTSSIZE_GEQ:
            if (diff >= stream_size)
                ret = 1;
            break;
        case DETECTSSIZE_GT:
            if (diff > stream_size)
                ret = 1;
            break;
    }

    return ret;
}

/**
 * \brief This function is used to match Stream size rule option on a packet with those passed via stream_size:
 *
 * \param t pointer to thread vars
 * \param det_ctx pointer to the pattern matcher thread
 * \param p pointer to the current packet
 * \param m pointer to the sigmatch that we will cast into DetectStreamSizeData
 *
 * \retval 0 no match
 * \retval 1 match
 */
int DetectStreamSizeMatch (ThreadVars *t, DetectEngineThreadCtx *det_ctx, Packet *p, Signature *s, SigMatch *m) {

    int ret = 0;
    DetectStreamSizeData *sd = (DetectStreamSizeData *) m->ctx;

    if (!(PKT_IS_TCP(p)))
        return ret;

    uint32_t csdiff = 0;
    uint32_t ssdiff = 0;

    if (p->flow == NULL)
        return ret;

    TcpSession *ssn = (TcpSession *)p->flow->protoctx;
    if (ssn == NULL)
        return ret;

    if (sd->flags & STREAM_SIZE_SERVER) {
        /* get the server stream size */
        ssdiff = ssn->server.next_seq - ssn->server.isn;
        ret = DetectStreamSizeCompare(ssdiff, sd->ssize, sd->mode);

    } else if (sd->flags & STREAM_SIZE_CLIENT) {
        /* get the client stream size */
        csdiff = ssn->client.next_seq - ssn->client.isn;
        ret = DetectStreamSizeCompare(csdiff, sd->ssize, sd->mode);

    } else if (sd->flags & STREAM_SIZE_BOTH) {
        ssdiff = ssn->server.next_seq - ssn->server.isn;
        csdiff = ssn->client.next_seq - ssn->client.isn;
        if (DetectStreamSizeCompare(ssdiff, sd->ssize, sd->mode) && DetectStreamSizeCompare(csdiff, sd->ssize, sd->mode))
            ret = 1;

    } else if (sd->flags & STREAM_SIZE_EITHER) {
        ssdiff = ssn->server.next_seq - ssn->server.isn;
        csdiff = ssn->client.next_seq - ssn->client.isn;
        if (DetectStreamSizeCompare(ssdiff, sd->ssize, sd->mode) || DetectStreamSizeCompare(csdiff, sd->ssize, sd->mode))
            ret = 1;
    }

    return ret;
}

/**
 * \brief This function is used to parse stream options passed via stream_size: keyword
 *
 * \param streamstr Pointer to the user provided stream_size options
 *
 * \retval sd pointer to DetectStreamSizeData on success
 * \retval NULL on failure
 */

DetectStreamSizeData *DetectStreamSizeParse (char *streamstr) {

    DetectStreamSizeData *sd = NULL;
    char *arg = NULL;
    char *value = NULL;
    char *mode = NULL;
#define MAX_SUBSTRINGS 30
    int ret = 0, res = 0;
    int ov[MAX_SUBSTRINGS];

    ret = pcre_exec(parse_regex, parse_regex_study, streamstr, strlen(streamstr), 0, 0, ov, MAX_SUBSTRINGS);
    if (ret != 4) {
        SCLogError(SC_ERR_PCRE_MATCH, "pcre_exec parse error, ret %" PRId32 ", string %s", ret, streamstr);
        goto error;
    }

    const char *str_ptr;

    res = pcre_get_substring((char *)streamstr, ov, MAX_SUBSTRINGS, 1, &str_ptr);
    if (res < 0) {
        SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed");
        goto error;
    }
    arg = (char *)str_ptr;

    res = pcre_get_substring((char *)streamstr, ov, MAX_SUBSTRINGS, 2, &str_ptr);
    if (res < 0) {
        SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed");
        goto error;
    }
    mode = (char *)str_ptr;

    res = pcre_get_substring((char *)streamstr, ov, MAX_SUBSTRINGS, 3, &str_ptr);
    if (res < 0) {
        SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed");
        goto error;
    }
    value = (char *)str_ptr;

    sd = SCMalloc(sizeof(DetectStreamSizeData));
    if (sd == NULL)
        goto error;
    sd->ssize = 0;
    sd->flags = 0;

    if (strlen(mode) == 0)
        goto error;

    if(mode[0] == '<') sd->mode = DETECTSSIZE_LT;
    else if (strcmp("<=", mode) == 0) sd->mode = DETECTSSIZE_LEQ;
    else if (mode[0] == '>') sd->mode = DETECTSSIZE_GT;
    else if (strcmp(">=", mode)) sd->mode = DETECTSSIZE_GEQ;
    else if (strcmp("!=", mode)) sd->mode = DETECTSSIZE_NEQ;
    else if (mode[0] == '=') sd->mode = DETECTSSIZE_EQ;
    else {
        SCLogError(SC_ERR_INVALID_OPERATOR, "Invalid operator");
        goto error;
    }

    /* set the value */
    sd->ssize = (uint32_t)atoi(value);

    /* inspect our options and set the flags */
    if (strcmp(arg, "server") == 0) {
        if (sd->flags & STREAM_SIZE_SERVER) {
            SCLogDebug("DetectStreamSizeParse error STREAM_SIZE_SERVER flag is already set ");
            goto error;
        }
        sd->flags |= STREAM_SIZE_SERVER;

    } else if (strcmp(arg, "client") == 0) {

        if (sd->flags & STREAM_SIZE_CLIENT) {
            SCLogDebug("DetectStreamSizeParse error STREAM_SIZE_CLIENT flag is already set ");
            goto error;
        }
        sd->flags |= STREAM_SIZE_CLIENT;

    } else if ((strcmp(arg, "both") == 0)) {

        if (sd->flags & STREAM_SIZE_SERVER || sd->flags & STREAM_SIZE_CLIENT) {
            SCLogDebug("DetectStreamSizeParse error STREAM_SIZE_SERVER or STREAM_SIZE_CLIENT flag is already set ");
            goto error;
        }
        sd->flags |= STREAM_SIZE_BOTH;
    } else if (strcmp(arg, "either") == 0) {

        if (sd->flags & STREAM_SIZE_SERVER || sd->flags & STREAM_SIZE_CLIENT) {
            SCLogDebug("DetectStreamSizeParse error STREAM_SIZE_SERVER or STREAM_SIZE_CLIENT flag is already set ");
            goto error;
        }
        sd->flags |= STREAM_SIZE_EITHER;

    } else {
        goto error;
    }

    if (mode != NULL) SCFree(mode);
    if (arg != NULL) SCFree(arg);
    if (value != NULL) SCFree(value);
    return sd;

error:
    if (mode != NULL) SCFree(mode);
    if (arg != NULL) SCFree(arg);
    if (value != NULL) SCFree(value);
    if (sd != NULL) DetectStreamSizeFree(sd);

    return NULL;
}

/**
 * \brief this function is used to add the parsed stream size data into the current signature
 *
 * \param de_ctx pointer to the Detection Engine Context
 * \param s pointer to the Current Signature
 * \param streamstr pointer to the user provided stream size options
 *
 * \retval 0 on Success
 * \retval -1 on Failure
 */
static int DetectStreamSizeSetup (DetectEngineCtx *de_ctx, Signature *s, char *streamstr) {

    DetectStreamSizeData *sd = NULL;
    SigMatch *sm = NULL;

    sd = DetectStreamSizeParse(streamstr);
    if (sd == NULL)
        goto error;

    sm = SigMatchAlloc();
    if (sm == NULL)
        goto error;

    sm->type = DETECT_STREAM_SIZE;
    sm->ctx = (void *)sd;

    SigMatchAppendPacket(s, sm);

    return 0;

error:
    if (sd != NULL) DetectStreamSizeFree(sd);
    if (sm != NULL) SCFree(sm);
    return -1;
}

/**
 * \brief this function will free memory associated with DetectStreamSizeData
 *
 * \param ptr pointer to DetectStreamSizeData
 */
void DetectStreamSizeFree(void *ptr) {
    DetectStreamSizeData *sd = (DetectStreamSizeData *)ptr;
    SCFree(sd);
}

#ifdef UNITTESTS
/**
 * \test DetectStreamSizeParseTest01 is a test to make sure that we parse the
 *  user options correctly, when given valid stream_size options.
 */

static int DetectStreamSizeParseTest01 (void) {
    int result = 0;
    DetectStreamSizeData *sd = NULL;
    sd = DetectStreamSizeParse("server,<,6");
    if (sd != NULL) {
        if (sd->flags & STREAM_SIZE_SERVER && sd->mode == DETECTSSIZE_LT && sd->ssize == 6)
            result = 1;
        DetectStreamSizeFree(sd);
    }

    return result;
}

/**
 * \test DetectStreamSizeParseTest02 is a test to make sure that we detect the
 *  invalid stream_size options.
 */

static int DetectStreamSizeParseTest02 (void) {
    int result = 1;
    DetectStreamSizeData *sd = NULL;
    sd = DetectStreamSizeParse("invalidoption,<,6");
    if (sd != NULL) {
        printf("expected: NULL got 0x%02X %" PRId16 ": ",sd->flags, sd->ssize);
        result = 0;
        DetectStreamSizeFree(sd);
    }

    return result;
}

/**
 * \test DetectStreamSizeParseTest03 is a test to make sure that we match the
 *  packet correctly provided valid stream size.
 */

static int DetectStreamSizeParseTest03 (void) {

    int result = 0;
    DetectStreamSizeData *sd = NULL;
    TcpSession ssn;
    ThreadVars tv;
    DetectEngineThreadCtx dtx;
    Packet p;
    Signature s;
    SigMatch sm;
    TcpStream client;
    Flow f;
    TCPHdr tcph;

    memset(&ssn, 0, sizeof(TcpSession));
    memset(&tv, 0, sizeof(ThreadVars));
    memset(&dtx, 0, sizeof(DetectEngineThreadCtx));
    memset(&p, 0, sizeof(Packet));
    memset(&s, 0, sizeof(Signature));
    memset(&sm, 0, sizeof(SigMatch));
    memset(&client, 0, sizeof(TcpStream));
    memset(&f, 0, sizeof(Flow));
    memset(&tcph, 0, sizeof(TCPHdr));

    sd = DetectStreamSizeParse("client,>,8");
    if (sd != NULL) {
        if (!(sd->flags & STREAM_SIZE_CLIENT)) {
            printf("sd->flags not STREAM_SIZE_CLIENT: ");
            DetectStreamSizeFree(sd);
            return 0;
        }

        if (sd->mode != DETECTSSIZE_GT) {
            printf("sd->mode not DETECTSSIZE_GT: ");
            DetectStreamSizeFree(sd);
            return 0;
        }

        if (sd->ssize != 8) {
            printf("sd->ssize is %"PRIu32", not 8: ", sd->ssize);
            DetectStreamSizeFree(sd);
            return 0;
        }
    } else {
        printf("sd == NULL: ");
        return 0;
    }

    client.next_seq = 20;
    client.isn = 10;
    ssn.client = client;
    f.protoctx = &ssn;
    p.flow = &f;
    p.tcph = &tcph;
    sm.ctx = sd;

    result = DetectStreamSizeMatch(&tv, &dtx, &p, &s, &sm);
    if (result == 0) {
        printf("result 0 != 1: ");
    }
    DetectStreamSizeFree(sd);
    return result;
}

/**
 * \test DetectStreamSizeParseTest04 is a test to make sure that we match the
 *  stream_size against invalid packet parameters.
 */

static int DetectStreamSizeParseTest04 (void) {

    int result = 0;
    DetectStreamSizeData *sd = NULL;
    TcpSession ssn;
    ThreadVars tv;
    DetectEngineThreadCtx dtx;
    Packet p;
    Signature s;
    SigMatch sm;
    TcpStream client;
    Flow f;
    IPV4Hdr ip4h;

    memset(&ssn, 0, sizeof(TcpSession));
    memset(&tv, 0, sizeof(ThreadVars));
    memset(&dtx, 0, sizeof(DetectEngineThreadCtx));
    memset(&p, 0, sizeof(Packet));
    memset(&s, 0, sizeof(Signature));
    memset(&sm, 0, sizeof(SigMatch));
    memset(&client, 0, sizeof(TcpStream));
    memset(&f, 0, sizeof(Flow));
    memset(&ip4h, 0, sizeof(IPV4Hdr));

    sd = DetectStreamSizeParse(" client , > , 8 ");
    if (sd != NULL) {
        if (!(sd->flags & STREAM_SIZE_CLIENT) && sd->mode != DETECTSSIZE_GT && sd->ssize != 8)
            return 0;
    } else
        return 0;

    client.next_seq = 20;
    client.isn = 12;
    ssn.client = client;
    f.protoctx = &ssn;
    p.flow = &f;
    p.ip4h = &ip4h;
    sm.ctx = sd;

    if (!DetectStreamSizeMatch(&tv, &dtx, &p, &s, &sm))
        result = 1;

    return result;
}
#endif /* UNITTESTS */

/**
 * \brief this function registers unit tests for DetectStreamSize
 */
void DetectStreamSizeRegisterTests(void) {
#ifdef UNITTESTS
    UtRegisterTest("DetectStreamSizeParseTest01", DetectStreamSizeParseTest01, 1);
    UtRegisterTest("DetectStreamSizeParseTest02", DetectStreamSizeParseTest02, 1);
    UtRegisterTest("DetectStreamSizeParseTest03", DetectStreamSizeParseTest03, 1);
    UtRegisterTest("DetectStreamSizeParseTest04", DetectStreamSizeParseTest04, 1);
#endif /* UNITTESTS */
}

