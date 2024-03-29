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
 * \author Brian Rectanus <brectanu@gmail.com>
 */

#ifndef __DETECT_HTTP_METHOD_H__
#define __DETECT_HTTP_METHOD_H__

typedef struct DetectHttpMethodData_ {
    uint8_t *content;     /**< Raw HTTP method content to match */
    size_t   content_len; /**< Raw HTTP method content length */
    int      method;      /**< Numeric HTTP method to match */
} DetectHttpMethodData;

/* prototypes */
void DetectHttpMethodRegister(void);
int DetectHttpMethodDoMatch(DetectEngineThreadCtx *det_ctx, Signature *s, SigMatch *sm, Flow *f, uint8_t flags, void *state);

#endif /* __DETECT_HTTP_METHOD_H__ */

