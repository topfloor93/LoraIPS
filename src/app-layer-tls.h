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
 * \author Gurvinder Singh <gurvindersinghdahiya@gmail.com>
 */

#ifndef __APP_LAYER_TLS_H__
#define __APP_LAYER_TLS_H__

#define TLS_FLAG_SERVER_CHANGE_CIPHER_SPEC   0x01    /**< Flag to indicate that
                                                     server will now on sends
                                                     encrypted msgs. */
#define TLS_FLAG_CLIENT_CHANGE_CIPHER_SPEC   0x02    /**< Flag to indicate that
                                                     client will now on sends
                                                     encrypted msgs. */

enum {
    TLS_FIELD_NONE = 0,

    TLS_FIELD_CLIENT_CONTENT_TYPE, /* len 1 */
    TLS_FIELD_CLIENT_VERSION,      /* len 2 */

    TLS_FIELD_SERVER_CONTENT_TYPE, /* len 1 */
    TLS_FIELD_SERVER_VERSION,      /* len 2 */

    TLS_FIELD_LENGTH,
    /* must be last */
    TLS_FIELD_MAX,
};
/* structure to store the TLS state values */
typedef struct TlsState_ {
    uint8_t client_content_type;    /**< Client content type storage field */
    uint16_t client_version;        /**< Client TLS version storage field */

    uint8_t server_content_type;    /**< Server content type storage field */
    uint16_t server_version;        /**< Server TLS version storage field */

    uint8_t flags;                  /**< Flags to indicate the current TLS
                                         sessoin state */
} TlsState;

enum {
    TLS_VERSION_INVALID = 0x0000,
    TLS_VERSION_VALID = 0x0001,
    SSL_VERSION_3 = 0x0300,
    TLS_VERSION_10 = 0x0301,
    TLS_VERSION_11 = 0x0302,
    TLS_VERSION_12 = 0x0303,
};

void RegisterTLSParsers(void);
void TLSParserRegisterTests(void);

#endif /* __APP_LAYER_TLS_H__ */

