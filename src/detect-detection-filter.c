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
 * \author Gerardo Iglesias <iglesiasg@gmail.com>
 *
 * Implements the detection_filter keyword
 */

#include "suricata-common.h"
#include "suricata.h"
#include "debug.h"
#include "decode.h"
#include "detect.h"

#include "detect-detection-filter.h"
#include "detect-threshold.h"
#include "detect-parse.h"

#include "util-byte.h"
#include "util-unittest.h"
#include "util-debug.h"

#define TRACK_DST      1
#define TRACK_SRC      2

/**
 *\brief Regex for parsing our detection_filter options
 */
#define PARSE_REGEX "^\\s*(track|count|seconds)\\s+(by_src|by_dst|\\d+)\\s*,\\s*(track|count|seconds)\\s+(by_src|by_dst|\\d+)\\s*,\\s*(track|count|seconds)\\s+(by_src|by_dst|\\d+)\\s*$"

static pcre *parse_regex;
static pcre_extra *parse_regex_study;

int DetectDetectionFilterMatch(ThreadVars *, DetectEngineThreadCtx *, Packet *, Signature *, SigMatch *);
static int DetectDetectionFilterSetup(DetectEngineCtx *, Signature *, char *);
void DetectDetectionFilterRegisterTests(void);
void DetectDetectionFilterFree(void *);

/**
 * \brief Registration function for detection_filter: keyword
 */
void DetectDetectionFilterRegister (void) {
    sigmatch_table[DETECT_DETECTION_FILTER].name = "detection_filter";
    sigmatch_table[DETECT_DETECTION_FILTER].Match = DetectDetectionFilterMatch;
    sigmatch_table[DETECT_DETECTION_FILTER].Setup = DetectDetectionFilterSetup;
    sigmatch_table[DETECT_DETECTION_FILTER].Free = DetectDetectionFilterFree;
    sigmatch_table[DETECT_DETECTION_FILTER].RegisterTests = DetectDetectionFilterRegisterTests;
    /* this is compatible to ip-only signatures */
    sigmatch_table[DETECT_DETECTION_FILTER].flags |= SIGMATCH_IPONLY_COMPAT;

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
    return;
}

int DetectDetectionFilterMatch (ThreadVars *thv, DetectEngineThreadCtx *det_ctx, Packet *p, Signature *s, SigMatch *sm) {
    return 1;
}

/**
 * \internal
 * \brief This function is used to parse detection_filter options passed via detection_filter: keyword
 *
 * \param rawstr Pointer to the user provided detection_filter options
 *
 * \retval df pointer to DetectThresholdData on success
 * \retval NULL on failure
 */
DetectThresholdData *DetectDetectionFilterParse (char *rawstr) {
    DetectThresholdData *df = NULL;
#define MAX_SUBSTRINGS 30
    int ret = 0, res = 0;
    int ov[MAX_SUBSTRINGS];
    const char *str_ptr = NULL;
    char *args[6] = { NULL, NULL, NULL, NULL, NULL, NULL};
    char *copy_str = NULL, *df_opt = NULL;
    int seconds_found = 0, count_found = 0, track_found = 0;
    int seconds_pos = 0, count_pos = 0;
    uint16_t pos = 0;
    int i = 0;

    copy_str = SCStrdup(rawstr);

    for(pos = 0, df_opt = strtok(copy_str,",");  pos < strlen(copy_str) &&  df_opt != NULL;  pos++, df_opt = strtok(NULL,",")) {

        if(strstr(df_opt,"count"))
            count_found++;
        if(strstr(df_opt,"second"))
            seconds_found++;
        if(strstr(df_opt,"track"))
            track_found++;
    }

    if(copy_str)
        SCFree(copy_str);

    if(count_found != 1 || seconds_found != 1 || track_found != 1)
        goto error;

    ret = pcre_exec(parse_regex, parse_regex_study, rawstr, strlen(rawstr), 0, 0, ov, MAX_SUBSTRINGS);
    if (ret < 5) {
        SCLogError(SC_ERR_PCRE_MATCH, "pcre_exec parse error, ret %" PRId32 ", string %s", ret, rawstr);
        goto error;
    }

    df = SCMalloc(sizeof(DetectThresholdData));
    if (df == NULL)
        goto error;

    memset(df,0,sizeof(DetectThresholdData));

    df->type = TYPE_DETECTION;

    for (i = 0; i < (ret - 1); i++) {
        res = pcre_get_substring((char *)rawstr, ov, MAX_SUBSTRINGS, i + 1, &str_ptr);
        if (res < 0) {
            SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed");
            goto error;
        }

        args[i] = (char *)str_ptr;

        if (strncasecmp(args[i],"by_dst",strlen("by_dst")) == 0)
            df->track = TRACK_DST;
        if (strncasecmp(args[i],"by_src",strlen("by_src")) == 0)
            df->track = TRACK_SRC;
        if (strncasecmp(args[i],"count",strlen("count")) == 0)
            count_pos = i+1;
        if (strncasecmp(args[i],"seconds",strlen("seconds")) == 0)
            seconds_pos = i+1;
    }

    if (args[count_pos] == NULL || args[seconds_pos] == NULL) {
        goto error;
    }

    if (ByteExtractStringUint32(&df->count, 10, strlen(args[count_pos]),
                args[count_pos]) <= 0) {
        goto error;
    }

    if (ByteExtractStringUint32(&df->seconds, 10, strlen(args[seconds_pos]),
                args[seconds_pos]) <= 0) {
        goto error;
    }

    if (df->count == 0 || df->seconds == 0) {
        SCLogError(SC_ERR_INVALID_VALUE, "found an invalid value");
        goto error;
    }

    for (i = 0; i < (ret - 1); i++){
        if (args[i] != NULL) SCFree(args[i]);
    }
    return df;

error:
    for (i = 0; i < (ret - 1); i++){
        if (args[i] != NULL) SCFree(args[i]);
    }
    if (df) SCFree(df);
    return NULL;
}

/**
 * \internal
 * \brief this function is used to add the parsed detection_filter into the current signature
 *
 * \param de_ctx pointer to the Detection Engine Context
 * \param s pointer to the Current Signature
 * \param m pointer to the Current SigMatch
 * \param rawstr pointer to the user provided detection_filter options
 *
 * \retval 0 on Success
 * \retval -1 on Failure
 */
int DetectDetectionFilterSetup (DetectEngineCtx *de_ctx, Signature *s, char *rawstr) {
    SCEnter();
    DetectThresholdData *df = NULL;
    SigMatch *sm = NULL;
    SigMatch *tmpm = NULL;

    /* checks if there's a previous instance of threshold */
    tmpm = SigMatchGetLastSM(s->match_tail, DETECT_THRESHOLD);
    if (tmpm != NULL) {
        SCLogError(SC_ERR_INVALID_SIGNATURE, "\"detection_filter\" and \"threshold\" are not allowed in the same rule");
        SCReturnInt(-1);
    }
    /* checks there's no previous instance of detection_filter */
    tmpm = SigMatchGetLastSM(s->match_tail, DETECT_DETECTION_FILTER);
    if (tmpm != NULL) {
        SCLogError(SC_ERR_INVALID_SIGNATURE, "At most one \"detection_filter\" is allowed per rule");
        SCReturnInt(-1);
    }

    df = DetectDetectionFilterParse(rawstr);
    if (df == NULL)
        goto error;

    sm = SigMatchAlloc();
    if (sm == NULL)
        goto error;

    sm->type = DETECT_DETECTION_FILTER;
    sm->ctx = (void *)df;

    SigMatchAppendPacket(s, sm);

    return 0;

error:
    if (df) SCFree(df);
    if (sm) SCFree(sm);
    return -1;
}

/**
 * \internal
 * \brief this function will free memory associated with DetectThresholdData
 *
 * \param df_ptr pointer to DetectDetectionFilterData
 */
void DetectDetectionFilterFree(void *df_ptr) {
    DetectThresholdData *df = (DetectThresholdData *)df_ptr;
    if (df) SCFree(df);
}

/*
 * ONLY TESTS BELOW THIS COMMENT
 */
#ifdef UNITTESTS

#include "detect-parse.h"
#include "detect-engine.h"
#include "detect-engine-mpm.h"
#include "detect-engine-threshold.h"
#include "util-time.h"
#include "util-hashlist.h"

/**
 * \test DetectDetectionFilterTestParse01 is a test for a valid detection_filter options
 *
 *  \retval 1 on succces
 *  \retval 0 on failure
 */
int DetectDetectionFilterTestParse01 (void) {
    DetectThresholdData *df = NULL;
    df = DetectDetectionFilterParse("track by_dst,count 10,seconds 60");
    if (df && (df->track == TRACK_DST) && (df->count == 10) && (df->seconds == 60)) {
        DetectDetectionFilterFree(df);
        return 1;
    }

    return 0;
}

/**
 * \test DetectDetectionFilterTestParse02 is a test for a invalid detection_filter options
 *
 *  \retval 1 on succces
 *  \retval 0 on failure
 */
int DetectDetectionFilterTestParse02 (void) {
    DetectThresholdData *df = NULL;
    df = DetectDetectionFilterParse("track both,count 10,seconds 60");
    if (df && (df->track == TRACK_DST || df->track == TRACK_SRC) && (df->count == 10) && (df->seconds == 60)) {
        DetectDetectionFilterFree(df);
        return 1;
    }

    return 0;
}

/**
 * \test DetectDetectionfilterTestParse03 is a test for a valid detection_filter options in any order
 *
 *  \retval 1 on succces
 *  \retval 0 on failure
 */
int DetectDetectionFilterTestParse03 (void) {
    DetectThresholdData *df = NULL;
    df = DetectDetectionFilterParse("track by_dst, seconds 60, count 10");
    if (df && (df->track == TRACK_DST) && (df->count == 10) && (df->seconds == 60)) {
        DetectDetectionFilterFree(df);
        return 1;
    }

    return 0;
}


/**
 * \test DetectDetectionFilterTestParse04 is a test for an invalid detection_filter options in any order
 *
 *  \retval 1 on succces
 *  \retval 0 on failure
 */
int DetectDetectionFilterTestParse04 (void) {
    DetectThresholdData *df = NULL;
    df = DetectDetectionFilterParse("count 10, track by_dst, seconds 60, count 10");
    if (df && (df->track == TRACK_DST) && (df->count == 10) && (df->seconds == 60)) {
        DetectDetectionFilterFree(df);
        return 1;
    }

    return 0;
}

/**
 * \test DetectDetectionFilterTestParse05 is a test for a valid detection_filter options in any order
 *
 *  \retval 1 on succces
 *  \retval 0 on failure
 */
int DetectDetectionFilterTestParse05 (void) {
    DetectThresholdData *df = NULL;
    df = DetectDetectionFilterParse("count 10, track by_dst, seconds 60");
    if (df && (df->track == TRACK_DST) && (df->count == 10) && (df->seconds == 60)) {
        DetectDetectionFilterFree(df);
        return 1;
    }

    return 0;
}

/**
 * \test DetectDetectionFilterTestParse06 is a test for an invalid value in detection_filter
 *
 *  \retval 1 on succces
 *  \retval 0 on failure
 */
int DetectDetectionFilterTestParse06 (void) {
    DetectThresholdData *df = NULL;
    df = DetectDetectionFilterParse("count 10, track by_dst, seconds 0");
    if (df && (df->track == TRACK_DST) && (df->count == 10) && (df->seconds == 0)) {
        DetectDetectionFilterFree(df);
        return 1;
    }

    return 0;
}

/**
 * \test DetectDetectionFilterTestSig1 is a test for checking the working of detection_filter keyword
 *       by setting up the signature and later testing its working by matching
 *       the received packet against the sig.
 *
 *  \retval 1 on succces
 *  \retval 0 on failure
 */
static int DetectDetectionFilterTestSig1(void) {

    Packet p;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;
    int result = 0;
    int alerts = 0;
    IPV4Hdr ip4h;

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    memset(&ip4h, 0, sizeof(ip4h));

    p.src.family = AF_INET;
    p.dst.family = AF_INET;
    p.proto = IPPROTO_TCP;
    p.ip4h = &ip4h;
    p.ip4h->ip_src.s_addr = 0x01010101;
    p.ip4h->ip_dst.s_addr = 0x02020202;
    p.sp = 1024;
    p.dp = 80;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert tcp any any -> any 80 (msg:\"detection_filter Test\"; detection_filter: track by_dst, count 4, seconds 60; sid:1;)");
    if (s == NULL) {
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);
    alerts = PacketAlertCheck(&p, 1);
    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);
    alerts += PacketAlertCheck(&p, 1);
    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);
    alerts += PacketAlertCheck(&p, 1);
    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);
    alerts += PacketAlertCheck(&p, 1);
    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);
    alerts += PacketAlertCheck(&p, 1);
    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);
    alerts += PacketAlertCheck(&p, 1);
    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);
    alerts += PacketAlertCheck(&p, 1);
    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);
    alerts += PacketAlertCheck(&p, 1);

    if(alerts == 5)
        result = 1;

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    DetectEngineCtxFree(de_ctx);

end:
    return result;
}

/**
 * \test DetectDetectionFilterTestSig2 is a test for checking the working of detection_filter keyword
 *       by setting up the signature and later testing its working by matching
 *       the received packet against the sig.
 *
 *  \retval 1 on succces
 *  \retval 0 on failure
 */

static int DetectDetectionFilterTestSig2(void) {

    Packet p;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;
    int result = 0;
    int alerts = 0;
    IPV4Hdr ip4h;
    struct timeval ts;

    memset (&ts, 0, sizeof(struct timeval));
    TimeGet(&ts);

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    memset(&ip4h, 0, sizeof(ip4h));

    p.src.family = AF_INET;
    p.dst.family = AF_INET;
    p.proto = IPPROTO_TCP;
    p.ip4h = &ip4h;
    p.ip4h->ip_src.s_addr = 0x01010101;
    p.ip4h->ip_dst.s_addr = 0x02020202;
    p.sp = 1024;
    p.dp = 80;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert tcp any any -> any 80 (msg:\"detection_filter Test 2\"; detection_filter: track by_dst, count 4, seconds 60; sid:10;)");
    if (s == NULL) {
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    TimeGet(&p.ts);

    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);
    alerts = PacketAlertCheck(&p, 10);
    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);
    alerts += PacketAlertCheck(&p, 10);
    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);
    alerts += PacketAlertCheck(&p, 10);

    TimeSetIncrementTime(200);
    TimeGet(&p.ts);

    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);
    alerts += PacketAlertCheck(&p, 10);
    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);
    alerts += PacketAlertCheck(&p, 10);
    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);
    alerts += PacketAlertCheck(&p, 10);
    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);
    alerts += PacketAlertCheck(&p, 10);

    if (alerts == 1)
        result = 1;

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    DetectEngineCtxFree(de_ctx);
end:
    return result;
}


#endif /* UNITTESTS */

void DetectDetectionFilterRegisterTests(void) {
#ifdef UNITTESTS
    UtRegisterTest("DetectDetectionFilterTestParse01", DetectDetectionFilterTestParse01, 1);
    UtRegisterTest("DetectDetectionFilterTestParse02", DetectDetectionFilterTestParse02, 0);
    UtRegisterTest("DetectDetectionFilterTestParse03", DetectDetectionFilterTestParse03, 1);
    UtRegisterTest("DetectDetectionFilterTestParse04", DetectDetectionFilterTestParse04, 0);
    UtRegisterTest("DetectDetectionFilterTestParse05", DetectDetectionFilterTestParse05, 1);
    UtRegisterTest("DetectDetectionFilterTestParse06", DetectDetectionFilterTestParse06, 0);
    UtRegisterTest("DetectDetectionFilterTestSig1", DetectDetectionFilterTestSig1, 1);
    UtRegisterTest("DetectDetectionFilterTestSig2", DetectDetectionFilterTestSig2, 1);
#endif /* UNITTESTS */
}

