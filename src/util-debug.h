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
 * \author Anoop Saldanha <poonaatsoc@gmail.com>
 */

#include <stdio.h>
#include "threads.h"
#include <stdint.h>

#include "util-enum.h"
#include "util-error.h"
#include "pcre.h"
#include "util-debug-filters.h"

#ifndef __UTIL_DEBUG_H__
#define __UTIL_DEBUG_H__

/**
 * \brief ENV vars that can be used to set the properties for the logging module
 */
#define SC_LOG_ENV_LOG_LEVEL        "SC_LOG_LEVEL"
#define SC_LOG_ENV_LOG_OP_IFACE     "SC_LOG_OP_IFACE"
#define SC_LOG_ENV_LOG_FILE         "SC_LOG_FILE"
#define SC_LOG_ENV_LOG_FACILITY     "SC_LOG_FACILITY"
#define SC_LOG_ENV_LOG_FORMAT       "SC_LOG_FORMAT"
#define SC_LOG_ENV_LOG_OP_FILTER    "SC_LOG_OP_FILTER"

/**
 * \brief The various log levels
 */
typedef enum {
    SC_LOG_NOTSET = -1,
    SC_LOG_NONE = 0,
    SC_LOG_EMERGENCY,
    SC_LOG_ALERT,
    SC_LOG_CRITICAL,
    SC_LOG_ERROR,
    SC_LOG_WARNING,
    SC_LOG_NOTICE,
    SC_LOG_INFO,
    SC_LOG_DEBUG,
    SC_LOG_LEVEL_MAX,
} SCLogLevel;

/**
 * \brief The various output interfaces supported
 */
typedef enum {
    SC_LOG_OP_IFACE_CONSOLE,
    SC_LOG_OP_IFACE_FILE,
    SC_LOG_OP_IFACE_SYSLOG,
    SC_LOG_OP_IFACE_MAX,
} SCLogOPIface;

/* The default log_format, if it is not supplied by the user */
#define SC_LOG_DEF_LOG_FORMAT "[%i] %t - (%f:%l) <%d> (%n) -- "

/* The maximum length of the log message */
#define SC_LOG_MAX_LOG_MSG_LEN 1024

/* The maximum length of the log format */
#define SC_LOG_MAX_LOG_FORMAT_LEN 128

/* The default log level, if it is not supplied by the user */
#define SC_LOG_DEF_LOG_LEVEL SC_LOG_INFO

/* The default output interface to be used */
#define SC_LOG_DEF_LOG_OP_IFACE SC_LOG_OP_IFACE_CONSOLE

/* The default log file to be used */
#define SC_LOG_DEF_LOG_FILE "sc_ids_log.log"

/* The default syslog facility to be used */
#define SC_LOG_DEF_SYSLOG_FACILITY_STR "local0"
#define SC_LOG_DEF_SYSLOG_FACILITY LOG_LOCAL0

/**
 * \brief Structure to be used when log_level override support would be provided
 *        by the logging module
 */
typedef struct SCLogOPBuffer_ {
    char msg[SC_LOG_MAX_LOG_MSG_LEN];
    char *temp;
    const char *log_format;
} SCLogOPBuffer;

/**
 * \brief The output interface context for the logging module
 */
typedef struct SCLogOPIfaceCtx_ {
    SCLogOPIface iface;

    /* the output file to be used if the interface is SC_LOG_IFACE_FILE */
    const char *file;
    /* the output file descriptor for the above file */
    FILE * file_d;

    /* the facility code if the interface is SC_LOG_IFACE_SYSLOG */
    int facility;

    /* override for the global_log_format(currently not used) */
    const char *log_format;

    /* override for the global_log_level */
    SCLogLevel log_level;

    struct SCLogOPIfaceCtx_ *next;
} SCLogOPIfaceCtx;

/**
 * \brief Structure containing init data, that would be passed to
 *        SCInitDebugModule()
 */
typedef struct SCLogInitData_ {
    /* startup message */
    char *startup_message;

    /* the log level */
    SCLogLevel global_log_level;

    /* the log format */
    char *global_log_format;

    /* output filter */
    char *op_filter;

    /* list of output interfaces to be used */
    SCLogOPIfaceCtx *op_ifaces;
    /* no of op ifaces */
    uint8_t op_ifaces_cnt;
} SCLogInitData;

/**
 * \brief Holds the config state used by the logging api
 */
typedef struct SCLogConfig_ {
    char *startup_message;
    SCLogLevel log_level;
    char *log_format;

    /* compiled pcre filter expression */
    pcre *op_filter_regex;
    pcre_extra *op_filter_regex_study;

    /* op ifaces used */
    SCLogOPIfaceCtx *op_ifaces;
    /* no of op ifaces */
    uint8_t op_ifaces_cnt;
} SCLogConfig;

/* The different log format specifiers supported by the API */
#define SC_LOG_FMT_TIME             't' /* Timestamp in standard format */
#define SC_LOG_FMT_PID              'p' /* PID */
#define SC_LOG_FMT_TID              'i' /* Thread ID */
#define SC_LOG_FMT_TM               'm' /* Thread module name */
#define SC_LOG_FMT_LOG_LEVEL        'd' /* Log level */
#define SC_LOG_FMT_FILE_NAME        'f' /* File name */
#define SC_LOG_FMT_LINE             'l' /* Line number */
#define SC_LOG_FMT_FUNCTION         'n' /* Function */

/* The log format prefix for the format specifiers */
#define SC_LOG_FMT_PREFIX           '%'

extern SCLogLevel sc_log_global_log_level;

extern int sc_log_module_initialized;

extern int sc_log_module_cleaned;


#define SCLog(x, ...)         do {                                       \
                                  char _sc_log_msg[SC_LOG_MAX_LOG_MSG_LEN]; \
                                  char *_sc_log_temp = _sc_log_msg;      \
                                  if ( !(                                \
                                      (sc_log_global_log_level >= x) &&  \
                                       SCLogMessage(x, &_sc_log_temp,    \
                                                    __FILE__,            \
                                                    __LINE__,            \
                                                    __FUNCTION__)        \
                                       == SC_OK) )                       \
                                  { } else {                             \
                                      snprintf(_sc_log_temp,             \
                                               (SC_LOG_MAX_LOG_MSG_LEN - \
                                                (_sc_log_temp - _sc_log_msg)), \
                                               __VA_ARGS__);             \
                                      SCLogOutputBuffer(x, _sc_log_msg); \
                                  }                                      \
                              } while(0)

#define SCLogErr(x, err, ...) do {                                       \
                                  char _sc_log_err_msg[SC_LOG_MAX_LOG_MSG_LEN]; \
                                  char *_sc_log_err_temp = _sc_log_err_msg; \
                                  if ( !(                                \
                                      (sc_log_global_log_level >= x) &&  \
                                       SCLogMessage(x, &_sc_log_err_temp,\
                                                    __FILE__,            \
                                                    __LINE__,            \
                                                    __FUNCTION__)        \
                                       == SC_OK) )                       \
                                  { } else {                             \
                                      _sc_log_err_temp =                 \
                                                _sc_log_err_temp +       \
                                                snprintf(_sc_log_err_temp, \
                                               (SC_LOG_MAX_LOG_MSG_LEN - \
                                                (_sc_log_err_temp - _sc_log_err_msg)), \
                                               "[ERRCODE: %s(%d)] - ",   \
                                               SCErrorToString(err),     \
                                               err);                     \
                                      if ((_sc_log_err_temp - _sc_log_err_msg) > \
                                          SC_LOG_MAX_LOG_MSG_LEN) {      \
                                          printf("Warning: Log message exceeded message length limit of %d\n",\
                                                 SC_LOG_MAX_LOG_MSG_LEN); \
                                          _sc_log_err_temp = _sc_log_err_msg + \
                                              SC_LOG_MAX_LOG_MSG_LEN;    \
                                      }                                  \
                                      snprintf(_sc_log_err_temp,         \
                                               (SC_LOG_MAX_LOG_MSG_LEN - \
                                                (_sc_log_err_temp - _sc_log_err_msg)), \
                                               __VA_ARGS__);             \
                                      SCLogOutputBuffer(x, _sc_log_err_msg); \
                                  }                                      \
                              } while(0)

/**
 * \brief Macro used to log INFORMATIONAL messages.
 *
 * \retval ... Takes as argument(s), a printf style format message
 */
#define SCLogInfo(...) SCLog(SC_LOG_INFO, __VA_ARGS__)

/**
 * \brief Macro used to log NOTICE messages.
 *
 * \retval ... Takes as argument(s), a printf style format message
 */
#define SCLogNotice(...) SCLog(SC_LOG_NOTICE, __VA_ARGS__)

/**
 * \brief Macro used to log WARNING messages.
 *
 * \retval err_code Error code that has to be logged along with the
 *                  warning message
 * \retval ...      Takes as argument(s), a printf style format message
 */
#define SCLogWarning(err_code, ...) SCLogErr(SC_LOG_WARNING, err_code, \
                                          __VA_ARGS__)
/**
 * \brief Macro used to log ERROR messages.
 *
 * \retval err_code Error code that has to be logged along with the
 *                  error message
 * \retval ...      Takes as argument(s), a printf style format message
 */
#define SCLogError(err_code, ...) SCLogErr(SC_LOG_ERROR, err_code, \
                                        __VA_ARGS__)
/**
 * \brief Macro used to log CRITICAL messages.
 *
 * \retval err_code Error code that has to be logged along with the
 *                  critical message
 * \retval ...      Takes as argument(s), a printf style format message
 */
#define SCLogCritical(err_code, ...) SCLogErr(SC_LOG_CRITICAL, err_code, \
                                           __VA_ARGS__)
/**
 * \brief Macro used to log ALERT messages.
 *
 * \retval err_code Error code that has to be logged along with the
 *                  alert message
 * \retval ...      Takes as argument(s), a printf style format message
 */
#define SCLogAlert(err_code, ...) SCLogErr(SC_LOG_ALERT, err_code, \
                                        __VA_ARGS__)
/**
 * \brief Macro used to log EMERGENCY messages.
 *
 * \retval err_code Error code that has to be logged along with the
 *                  emergency message
 * \retval ...      Takes as argument(s), a printf style format message
 */
#define SCLogEmerg(err_code, ...) SCLogErr(SC_LOG_EMERGENCY, err_code, \
                                          __VA_ARGS__)


/* Avoid the overhead of using the debugging subsystem, in production mode */
#ifndef DEBUG

#define SCLogDebug(...)                 do { } while (0)

#define SCEnter(...)

#define SCReturn                        return

#define SCReturnInt(x)                  return x

#define SCReturnUInt(x)                 return x

#define SCReturnDbl(x)                  return x

#define SCReturnChar(x)                 return x

#define SCReturnCharPtr(x)              return x

#define SCReturnCT(x, type)             return x

#define SCReturnPtr(x, type)            return x

/* Please use it only for debugging purposes */
#else


/**
 * \brief Macro used to log DEBUG messages. Comes under the debugging subsystem,
 *        and hence will be enabled only in the presence of the DEBUG macro.
 *
 * \retval ... Takes as argument(s), a printf style format message
 */
#define SCLogDebug(...)       SCLog(SC_LOG_DEBUG, __VA_ARGS__)

/**
 * \brief Macro used to log debug messages on function entry.  Comes under the
 *        debugging subsystem, and hence will be enabled only in the presence
 *        of the DEBUG macro.  Apart from logging function_entry logs, it also
 *        processes the FD filters, if any FD filters are registered.
 *
 * \retval f An argument can be supplied, although it is not used
 */
#define SCEnter(f)            do {                                              \
                                  char _sc_enter_msg[SC_LOG_MAX_LOG_MSG_LEN];   \
                                  char *_sc_enter_temp = _sc_enter_msg;         \
                                  if (sc_log_global_log_level >= SC_LOG_DEBUG &&\
                                      SCLogCheckFDFilterEntry(__FUNCTION__) &&  \
                                      SCLogMessage(SC_LOG_DEBUG, &_sc_enter_temp, \
                                                   __FILE__,                    \
                                                   __LINE__,                    \
                                                   __FUNCTION__) == SC_OK) {    \
                                      snprintf(_sc_enter_temp, (SC_LOG_MAX_LOG_MSG_LEN - \
                                                      (_sc_enter_msg - _sc_enter_temp)), \
                                               "%s", "Entering ... >>");        \
                                      SCLogOutputBuffer(SC_LOG_DEBUG,           \
                                                        _sc_enter_msg);         \
                                  }                                             \
                              } while(0)


/**
 * \brief Macro used to log debug messages on function exit.  Comes under the
 *        debugging sybsystem, and hence will be enabled only in the presence
 *        of the DEBUG macro.  Apart from logging function_exit logs, it also
 *        processes the FD filters, if any FD filters are registered.  This
 *        function_exit macro should be used for functions that don't return
 *        a value.
 */
#define SCReturn              do {                                           \
                                  if (sc_log_global_log_level >= SC_LOG_DEBUG) { \
                                      SCLogDebug("Returning ... <<" );       \
                                      SCLogCheckFDFilterExit(__FUNCTION__);  \
                                  }                                          \
                                  return;                                    \
                              } while(0)

/**
 * \brief Macro used to log debug messages on function exit.  Comes under the
 *        debugging sybsystem, and hence will be enabled only in the presence
 *        of the DEBUG macro.  Apart from logging function_exit logs, it also
 *        processes the FD filters, if any FD filters are registered.  This
 *        function_exit macro should be used for functions that returns an
 *        integer value.
 *
 * \retval x Variable of type 'integer' that has to be returned
 */
#define SCReturnInt(x)        do {                                           \
                                  if (sc_log_global_log_level >= SC_LOG_DEBUG) { \
                                      SCLogDebug("Returning: %"PRIdMAX" ... <<", (intmax_t)x); \
                                      SCLogCheckFDFilterExit(__FUNCTION__);  \
                                  }                                          \
                                  return x;                                  \
                              } while(0)

/**
 * \brief Macro used to log debug messages on function exit.  Comes under the
 *        debugging sybsystem, and hence will be enabled only in the presence
 *        of the DEBUG macro.  Apart from logging function_exit logs, it also
 *        processes the FD filters, if any FD filters are registered.  This
 *        function_exit macro should be used for functions that returns an
 *        unsigned integer value.
 *
 * \retval x Variable of type 'unsigned integer' that has to be returned
 */
#define SCReturnUInt(x)       do {                                           \
                                  if (sc_log_global_log_level >= SC_LOG_DEBUG) { \
                                      SCLogDebug("Returning: %"PRIuMAX" ... <<", (uintmax_t)x); \
                                      SCLogCheckFDFilterExit(__FUNCTION__);  \
                                  }                                          \
                                  return x;                                  \
                              } while(0)

/**
 * \brief Macro used to log debug messages on function exit.  Comes under the
 *        debugging sybsystem, and hence will be enabled only in the presence
 *        of the DEBUG macro.  Apart from logging function_exit logs, it also
 *        processes the FD filters, if any FD filters are registered.  This
 *        function_exit macro should be used for functions that returns a
 *        float/double value.
 *
 * \retval x Variable of type 'float/double' that has to be returned
 */
#define SCReturnDbl(x)        do {                                           \
                                  if (sc_log_global_log_level >= SC_LOG_DEBUG) { \
                                      SCLogDebug("Returning: %f ... <<", x); \
                                      SCLogCheckFDFilterExit(__FUNCTION__);  \
                                  }                                          \
                                  return x;                                  \
                              } while(0)

/**
 * \brief Macro used to log debug messages on function exit.  Comes under the
 *        debugging sybsystem, and hence will be enabled only in the presence
 *        of the DEBUG macro.  Apart from logging function_exit logs, it also
 *        processes the FD filters, if any FD filters are registered.  This
 *        function_exit macro should be used for functions that returns a var
 *        of character type.
 *
 * \retval x Variable of type 'char' that has to be returned
 */
#define SCReturnChar(x)       do {                                           \
                                  if (sc_log_global_log_level >= SC_LOG_DEBUG) { \
                                      SCLogDebug("Returning: %c ... <<", x); \
                                      SCLogCheckFDFilterExit(__FUNCTION__);  \
                                  }                                          \
                                  return x;                                  \
                              } while(0)

/**
 * \brief Macro used to log debug messages on function exit.  Comes under the
 *        debugging sybsystem, and hence will be enabled only in the presence
 *        of the DEBUG macro.  Apart from logging function_exit logs, it also
 *        processes the FD filters, if any FD filters are registered.  This
 *        function_exit macro should be used for functions that returns a
 *        character string.
 *
 * \retval x Pointer to the char string that has to be returned
 */
#define SCReturnCharPtr(x)    do {                                           \
                                  if (sc_log_global_log_level >= SC_LOG_DEBUG) { \
                                      SCLogDebug("Returning: %s ... <<", x); \
                                      SCLogCheckFDFilterExit(__FUNCTION__);  \
                                  }                                          \
                                 return x;                                   \
                              } while(0)


/**
 * \brief Macro used to log debug messages on function exit.  Comes under the
 *        debugging sybsystem, and hence will be enabled only in the presence
 *        of the DEBUG macro.  Apart from logging function_exit logs, it also
 *        processes the FD filters, if any FD filters are registered.  This
 *        function_exit macro should be used for functions that returns a var
 *        of custom type
 *
 * \retval x    Variable instance of a custom type that has to be returned
 * \retval type Pointer to a character string holding the name of the custom
 *              type(the argument x) that has to be returned
 */
#define SCReturnCT(x, type)   do {                                           \
                                  if (sc_log_global_log_level >= SC_LOG_DEBUG) { \
                                      SCLogDebug("Returning var of "         \
                                              "type %s ... <<", type);       \
                                      SCLogCheckFDFilterExit(__FUNCTION__);  \
                                  }                                          \
                                  return x;                                  \
                              } while(0)

/**
 * \brief Macro used to log debug messages on function exit.  Comes under the
 *        debugging sybsystem, and hence will be enabled only in the presence
 *        of the DEBUG macro.  Apart from logging function_exit logs, it also
 *        processes the FD filters, if any FD filters are registered.  This
 *        function_exit macro should be used for functions that returns a
 *        pointer to a custom type
 *
 * \retval x    Pointer to a variable instance of a custom type that has to be
 *              returned
 * \retval type Pointer to a character string holding the name of the custom
 *              type(the argument x) that has to be returned
 */
#define SCReturnPtr(x, type)  do {                                           \
                                  if (sc_log_global_log_level >= SC_LOG_DEBUG) { \
                                      SCLogDebug("Returning pointer %p of "  \
                                              "type %s ... <<", x, type);    \
                                      SCLogCheckFDFilterExit(__FUNCTION__);  \
                                  }                                          \
                                  return x;                                  \
                              } while(0)

#endif /* DEBUG */


SCLogInitData *SCLogAllocLogInitData(void);

SCLogOPIfaceCtx *SCLogInitOPIfaceCtx(const char *, const char *, int,
                                     const char *);

void SCLogAppendOPIfaceCtx(SCLogOPIfaceCtx *, SCLogInitData *);

void SCLogInitLogModule(SCLogInitData *);

void SCLogInitLogModuleIfEnvSet(void);

void SCLogDeInitLogModule(void);

SCError SCLogMessage(SCLogLevel, char **, const char *, unsigned, const char *);

void SCLogOutputBuffer(SCLogLevel, char *);

SCLogOPBuffer *SCLogAllocLogOPBuffer(void);

int SCLogDebugEnabled(void);

void SCLogRegisterTests(void);

void SCLogLoadConfig(void);

#endif /* __UTIL_DEBUG_H__ */
