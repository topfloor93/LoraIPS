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
 */

#ifndef _DETECT_HTTP_COOKIE_H
#define	_DETECT_HTTP_COOKIE_H

#define DETECT_AL_HTTP_COOKIE_NOCASE   0x01
#define DETECT_AL_HTTP_COOKIE_NEGATED  0x02

typedef struct DetectHttpCookieData_ {
    uint8_t *data;
    uint8_t data_len;
    uint8_t flags;
} DetectHttpCookieData;

/* prototypes */
void DetectHttpCookieRegister (void);

int DetectHttpCookieDoMatch(DetectEngineThreadCtx *det_ctx, Signature *s,
        SigMatch *sm, Flow *f, uint8_t flags, void *state);

#endif	/* _DETECT_HTTP_COOKIE_H */

