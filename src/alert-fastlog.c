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
 *
 * Logs alerts in a line based text format compatible to Snort's
 * alert_fast format.
 *
 * \todo Print the protocol as a string
 * \todo Support classifications
 * \todo Support more than just IPv4/IPv4 TCP/UDP.
 * \todo Print [drop] as well if appropriate
 */

#include "suricata-common.h"
#include "debug.h"
#include "detect.h"
#include "flow.h"
#include "conf.h"

#include "threads.h"
#include "tm-threads.h"
#include "threadvars.h"
#include "tm-modules.h"
#include "util-debug.h"
#include "util-unittest.h"

#include "detect.h"
#include "detect-parse.h"
#include "detect-engine.h"
#include "detect-engine-mpm.h"
#include "detect-reference.h"
#include "util-classification-config.h"

#include "output.h"
#include "alert-fastlog.h"

#include "util-privs.h"
#include "util-print.h"

#define DEFAULT_LOG_FILENAME "fast.log"

#define MODULE_NAME "AlertFastLog"

TmEcode AlertFastLog (ThreadVars *, Packet *, void *, PacketQueue *, PacketQueue *);
TmEcode AlertFastLogIPv4(ThreadVars *, Packet *, void *, PacketQueue *, PacketQueue *);
TmEcode AlertFastLogIPv6(ThreadVars *, Packet *, void *, PacketQueue *, PacketQueue *);
TmEcode AlertFastLogThreadInit(ThreadVars *, void *, void **);
TmEcode AlertFastLogThreadDeinit(ThreadVars *, void *);
void AlertFastLogExitPrintStats(ThreadVars *, void *);
static int AlertFastLogOpenFileCtx(LogFileCtx *, const char *);
void AlertFastLogRegisterTests(void);
static void AlertFastLogDeInitCtx(OutputCtx *);

void TmModuleAlertFastLogRegister (void) {
    tmm_modules[TMM_ALERTFASTLOG].name = MODULE_NAME;
    tmm_modules[TMM_ALERTFASTLOG].ThreadInit = AlertFastLogThreadInit;
    tmm_modules[TMM_ALERTFASTLOG].Func = AlertFastLog;
    tmm_modules[TMM_ALERTFASTLOG].ThreadExitPrintStats = AlertFastLogExitPrintStats;
    tmm_modules[TMM_ALERTFASTLOG].ThreadDeinit = AlertFastLogThreadDeinit;
    tmm_modules[TMM_ALERTFASTLOG].RegisterTests = AlertFastLogRegisterTests;
    tmm_modules[TMM_ALERTFASTLOG].cap_flags = 0;

    OutputRegisterModule(MODULE_NAME, "fast", AlertFastLogInitCtx);
}

void TmModuleAlertFastLogIPv4Register (void) {
    tmm_modules[TMM_ALERTFASTLOG4].name = "AlertFastLogIPv4";
    tmm_modules[TMM_ALERTFASTLOG4].ThreadInit = AlertFastLogThreadInit;
    tmm_modules[TMM_ALERTFASTLOG4].Func = AlertFastLogIPv4;
    tmm_modules[TMM_ALERTFASTLOG4].ThreadExitPrintStats = AlertFastLogExitPrintStats;
    tmm_modules[TMM_ALERTFASTLOG4].ThreadDeinit = AlertFastLogThreadDeinit;
    tmm_modules[TMM_ALERTFASTLOG4].RegisterTests = NULL;
}

void TmModuleAlertFastLogIPv6Register (void) {
    tmm_modules[TMM_ALERTFASTLOG6].name = "AlertFastLogIPv6";
    tmm_modules[TMM_ALERTFASTLOG6].ThreadInit = AlertFastLogThreadInit;
    tmm_modules[TMM_ALERTFASTLOG6].Func = AlertFastLogIPv6;
    tmm_modules[TMM_ALERTFASTLOG6].ThreadExitPrintStats = AlertFastLogExitPrintStats;
    tmm_modules[TMM_ALERTFASTLOG6].ThreadDeinit = AlertFastLogThreadDeinit;
    tmm_modules[TMM_ALERTFASTLOG6].RegisterTests = NULL;
}

typedef struct AlertFastLogThread_ {
    /** LogFileCtx has the pointer to the file and a mutex to allow multithreading */
    LogFileCtx* file_ctx;
} AlertFastLogThread;

static void CreateTimeString (const struct timeval *ts, char *str, size_t size) {
    time_t time = ts->tv_sec;
    struct tm local_tm;
    struct tm *t = gmtime_r(&time, &local_tm);
    uint32_t sec = ts->tv_sec % 86400;

    snprintf(str, size, "%02d/%02d/%02d-%02d:%02d:%02d.%06u",
            t->tm_mon + 1, t->tm_mday, t->tm_year - 100,
            sec / 3600, (sec % 3600) / 60, sec % 60,
            (uint32_t) ts->tv_usec);
}

TmEcode AlertFastLogIPv4(ThreadVars *tv, Packet *p, void *data, PacketQueue *pq, PacketQueue *postpq)
{
    AlertFastLogThread *aft = (AlertFastLogThread *)data;
    int i;
    Reference *ref = NULL;
    char timebuf[64];

    if (p->alerts.cnt == 0)
        return TM_ECODE_OK;

    CreateTimeString(&p->ts, timebuf, sizeof(timebuf));

    SCMutexLock(&aft->file_ctx->fp_mutex);

    aft->file_ctx->alerts += p->alerts.cnt;

    for (i = 0; i < p->alerts.cnt; i++) {
        PacketAlert *pa = &p->alerts.alerts[i];

        char srcip[16], dstip[16];

        inet_ntop(AF_INET, (const void *)GET_IPV4_SRC_ADDR_PTR(p), srcip, sizeof(srcip));
        inet_ntop(AF_INET, (const void *)GET_IPV4_DST_ADDR_PTR(p), dstip, sizeof(dstip));

        fprintf(aft->file_ctx->fp, "%s  [**] [%" PRIu32 ":%" PRIu32 ":%" PRIu32 "] %s [**] [Classification: %s] [Priority: %" PRIu32 "] {%" PRIu32 "} %s:%" PRIu32 " -> %s:%" PRIu32 "",
                timebuf, pa->gid, pa->sid, pa->rev, pa->msg, pa->class_msg, pa->prio, IPV4_GET_IPPROTO(p), srcip, p->sp, dstip, p->dp);

        if(pa->references != NULL)  {
            fprintf(aft->file_ctx->fp," ");
            for (ref = pa->references; ref != NULL; ref = ref->next)   {
                fprintf(aft->file_ctx->fp,"[Xref => %s%s]", ref->key, ref->reference);
            }
        }

        fprintf(aft->file_ctx->fp,"\n");

        fflush(aft->file_ctx->fp);
    }
    SCMutexUnlock(&aft->file_ctx->fp_mutex);

    return TM_ECODE_OK;
}

TmEcode AlertFastLogIPv6(ThreadVars *tv, Packet *p, void *data, PacketQueue *pq, PacketQueue *postpq)
{
    AlertFastLogThread *aft = (AlertFastLogThread *)data;
    int i;
    Reference *ref = NULL;
    char timebuf[64];

    if (p->alerts.cnt == 0)
        return TM_ECODE_OK;

    CreateTimeString(&p->ts, timebuf, sizeof(timebuf));

    SCMutexLock(&aft->file_ctx->fp_mutex);

    aft->file_ctx->alerts += p->alerts.cnt;

    for (i = 0; i < p->alerts.cnt; i++) {
        PacketAlert *pa = &p->alerts.alerts[i];
        char srcip[46], dstip[46];

        inet_ntop(AF_INET6, (const void *)GET_IPV6_SRC_ADDR(p), srcip, sizeof(srcip));
        inet_ntop(AF_INET6, (const void *)GET_IPV6_DST_ADDR(p), dstip, sizeof(dstip));

        fprintf(aft->file_ctx->fp, "%s  [**] [%" PRIu32 ":%" PRIu32 ":%" PRIu32 "] %s [**] [Classification: %s] [Priority: %" PRIu32 "] {%" PRIu32 "} %s:%" PRIu32 " -> %s:%" PRIu32 "",
                timebuf, pa->gid, pa->sid, pa->rev, pa->msg, pa->class_msg, pa->prio, IPV6_GET_L4PROTO(p), srcip, p->sp, dstip, p->dp);

        if(pa->references != NULL)  {
            fprintf(aft->file_ctx->fp," ");
            for (ref = pa->references; ref != NULL; ref = ref->next)   {
                fprintf(aft->file_ctx->fp,"[Xref => %s%s]", ref->key, ref->reference);
            }
        }

        fprintf(aft->file_ctx->fp,"\n");

        fflush(aft->file_ctx->fp);
    }
    SCMutexUnlock(&aft->file_ctx->fp_mutex);

    return TM_ECODE_OK;
}

TmEcode AlertFastLogDecoderEvent(ThreadVars *tv, Packet *p, void *data, PacketQueue *pq, PacketQueue *postpq)
{
    AlertFastLogThread *aft = (AlertFastLogThread *)data;
    int i;
    Reference *ref = NULL;
    char timebuf[64];

    if (p->alerts.cnt == 0)
        return TM_ECODE_OK;

    CreateTimeString(&p->ts, timebuf, sizeof(timebuf));

    SCMutexLock(&aft->file_ctx->fp_mutex);

    aft->file_ctx->alerts += p->alerts.cnt;

    for (i = 0; i < p->alerts.cnt; i++) {
        PacketAlert *pa = &p->alerts.alerts[i];

        fprintf(aft->file_ctx->fp, "%s  [**] [%" PRIu32 ":%" PRIu32 ":%" PRIu32 "] %s [**] [Classification: %s] [Priority: %" PRIu32 "] [**] [Raw pkt: ",
                timebuf, pa->gid, pa->sid, pa->rev, pa->msg, pa->class_msg, pa->prio);

        PrintRawLineHexFp(aft->file_ctx->fp, p->pkt, p->pktlen < 32 ? p->pktlen : 32);
        if (p->pcap_cnt != 0) {
            fprintf(aft->file_ctx->fp, "] [pcap file packet: %"PRIu64"]", p->pcap_cnt);
        }

        if(pa->references != NULL)  {
            fprintf(aft->file_ctx->fp," ");
            for (ref = pa->references; ref != NULL; ref = ref->next)   {
                fprintf(aft->file_ctx->fp,"[Xref => %s%s]", ref->key, ref->reference);
            }
        }

        fprintf(aft->file_ctx->fp,"\n");

        fflush(aft->file_ctx->fp);
    }
    SCMutexUnlock(&aft->file_ctx->fp_mutex);

    return TM_ECODE_OK;
}

TmEcode AlertFastLog (ThreadVars *tv, Packet *p, void *data, PacketQueue *pq, PacketQueue *postpq)
{
    if (PKT_IS_IPV4(p)) {
        return AlertFastLogIPv4(tv, p, data, pq, postpq);
    } else if (PKT_IS_IPV6(p)) {
        return AlertFastLogIPv6(tv, p, data, pq, postpq);
    } else if (p->events.cnt > 0) {
        return AlertFastLogDecoderEvent(tv, p, data, pq, postpq);
    }

    return TM_ECODE_OK;
}

TmEcode AlertFastLogThreadInit(ThreadVars *t, void *initdata, void **data)
{
    AlertFastLogThread *aft = SCMalloc(sizeof(AlertFastLogThread));
    if (aft == NULL)
        return TM_ECODE_FAILED;
    memset(aft, 0, sizeof(AlertFastLogThread));
    if(initdata == NULL)
    {
        SCLogDebug("Error getting context for AlertFastLog.  \"initdata\" argument NULL");
        SCFree(aft);
        return TM_ECODE_FAILED;
    }
    /** Use the Ouptut Context (file pointer and mutex) */
    aft->file_ctx = ((OutputCtx *)initdata)->data;

    *data = (void *)aft;
    return TM_ECODE_OK;
}

TmEcode AlertFastLogThreadDeinit(ThreadVars *t, void *data)
{
    AlertFastLogThread *aft = (AlertFastLogThread *)data;
    if (aft == NULL) {
        return TM_ECODE_OK;
    }

    /* clear memory */
    memset(aft, 0, sizeof(AlertFastLogThread));

    SCFree(aft);
    return TM_ECODE_OK;
}

void AlertFastLogExitPrintStats(ThreadVars *tv, void *data) {
    AlertFastLogThread *aft = (AlertFastLogThread *)data;
    if (aft == NULL) {
        return;
    }

    SCLogInfo("(%s) Alerts %" PRIu64 "", tv->name, aft->file_ctx->alerts);
}

/**
 * \brief Create a new LogFileCtx for "fast" output style.
 * \param conf The configuration node for this output.
 * \return A LogFileCtx pointer on success, NULL on failure.
 */
OutputCtx *AlertFastLogInitCtx(ConfNode *conf)
{
    LogFileCtx *logfile_ctx = LogFileNewCtx();
    if (logfile_ctx == NULL) {
        SCLogDebug("AlertFastLogInitCtx2: Could not create new LogFileCtx");
        return NULL;
    }

    const char *filename = ConfNodeLookupChildValue(conf, "filename");
    if (filename == NULL)
        filename = DEFAULT_LOG_FILENAME;
    if (AlertFastLogOpenFileCtx(logfile_ctx, filename) < 0) {
        LogFileFreeCtx(logfile_ctx);
        return NULL;
    }

    OutputCtx *output_ctx = SCCalloc(1, sizeof(OutputCtx));
    if (output_ctx == NULL)
        return NULL;
    output_ctx->data = logfile_ctx;
    output_ctx->DeInit = AlertFastLogDeInitCtx;

    SCLogInfo("Fast log output initialized, filename: %s", filename);

    return output_ctx;
}

static void AlertFastLogDeInitCtx(OutputCtx *output_ctx)
{
    LogFileCtx *logfile_ctx = (LogFileCtx *)output_ctx->data;
    LogFileFreeCtx(logfile_ctx);
    free(output_ctx);
}

/** \brief Read the config set the file pointer, open the file
 *  \param file_ctx pointer to a created LogFileCtx using LogFileNewCtx()
 *  \param filename name of log file
 *  \return -1 if failure, 0 if succesful
 * */
static int AlertFastLogOpenFileCtx(LogFileCtx *file_ctx, const char *filename)
{
    char log_path[PATH_MAX], *log_dir;
    if (ConfGet("default-log-dir", &log_dir) != 1)
        log_dir = DEFAULT_LOG_DIR;
    snprintf(log_path, PATH_MAX, "%s/%s", log_dir, filename);

    file_ctx->fp = fopen(log_path, "w");

    if (file_ctx->fp == NULL) {
        SCLogError(SC_ERR_FOPEN, "ERROR: failed to open %s: %s", log_path,
                strerror(errno));
        return -1;
    }

    return 0;
}


/*------------------------------Unittests-------------------------------------*/

#ifdef UNITTESTS

int AlertFastLogTest01()
{
    int result = 0;
    uint8_t *buf = (uint8_t *) "GET /one/ HTTP/1.1\r\n"
        "Host: one.example.org\r\n";

    uint16_t buflen = strlen((char *)buf);
    Packet p;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    p.src.family = AF_INET;
    p.dst.family = AF_INET;
    p.payload = buf;
    p.payload_len = buflen;
    p.proto = IPPROTO_TCP;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        return result;
    }

    de_ctx->flags |= DE_QUIET;

    SCClassConfGenerateValidDummyClassConfigFD01();
    SCClassConfLoadClassficationConfigFile(de_ctx);
    SCClassConfDeleteDummyClassificationConfigFD();

    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
            "(msg:\"FastLog test\"; content:GET; "
            "Classtype:unknown; sid:1;)");
    result = (de_ctx->sig_list != NULL);

    SigGroupBuild(de_ctx);
    //PatternMatchPrepare(mpm_ctx, MPM_B2G);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);
    if (p.alerts.cnt == 1)
        result = (strcmp(p.alerts.alerts[0].class_msg, "Unknown are we") == 0);
    else
        result = 0;

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    //PatternMatchDestroy(mpm_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int AlertFastLogTest02()
{
    int result = 0;
    uint8_t *buf = (uint8_t *) "GET /one/ HTTP/1.1\r\n"
        "Host: one.example.org\r\n";
    uint16_t buflen = strlen((char *)buf);
    Packet p;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    p.src.family = AF_INET;
    p.dst.family = AF_INET;
    p.payload = buf;
    p.payload_len = buflen;
    p.proto = IPPROTO_TCP;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        return result;
    }

    de_ctx->flags |= DE_QUIET;

    SCClassConfGenerateValidDummyClassConfigFD01();
    SCClassConfLoadClassficationConfigFile(de_ctx);
    SCClassConfDeleteDummyClassificationConfigFD();

    de_ctx->sig_list = SigInit(de_ctx, "alert tcp any any -> any any "
            "(msg:\"FastLog test\"; content:GET; "
            "Classtype:unknown; sid:1;)");
    result = (de_ctx->sig_list != NULL);
    if (result == 0) printf("sig parse failed: ");

    SigGroupBuild(de_ctx);
    //PatternMatchPrepare(mpm_ctx, MPM_B2G);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);
    if (p.alerts.cnt == 1) {
        result = (strcmp(p.alerts.alerts[0].class_msg, "Unknown Traffic") != 0);
        if (result == 0) printf("p.alerts.alerts[0].class_msg %s: ", p.alerts.alerts[0].class_msg);
        result = (strcmp(p.alerts.alerts[0].class_msg,
                    "Unknown are we") == 0);
        if (result == 0) printf("p.alerts.alerts[0].class_msg %s: ", p.alerts.alerts[0].class_msg);
    } else {
        result = 0;
    }

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    //PatternMatchDestroy(mpm_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

#endif /* UNITTESTS */

/**
 * \brief This function registers unit tests for AlertFastLog API.
 */
void AlertFastLogRegisterTests(void)
{

#ifdef UNITTESTS

    UtRegisterTest("AlertFastLogTest01", AlertFastLogTest01, 1);
    UtRegisterTest("AlertFastLogTest02", AlertFastLogTest02, 1);

#endif /* UNITTESTS */

}
