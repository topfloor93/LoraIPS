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
 * IPV6 Address part of the detection engine.
 */

#include "suricata-common.h"

#include "decode.h"
#include "detect.h"
#include "flow-var.h"

#include "util-cidr.h"
#include "util-unittest.h"

#include "detect-engine-address.h"
#include "detect-engine-siggroup.h"
#include "detect-engine-port.h"

#include "util-debug.h"

/**
 * \brief Compares 2 ipv6 addresses and returns if the first address(a) is less
 *        than the second address(b) or not.
 *
 * \param a The first ipv6 address to be compared.
 * \param b The second ipv6 address to be compared.
 *
 * \retval 1 If a < b.
 * \retval 0 Otherwise, i.e. a >= b.
 */
int AddressIPv6Lt(uint32_t *a, uint32_t *b)
{
    int i = 0;

    for (i = 0; i < 4; i++) {
        if (ntohl(a[i]) < ntohl(b[i]))
            return 1;
        if (ntohl(a[i]) > ntohl(b[i]))
            break;
    }

    return 0;
}

/**
 * \brief Compares 2 ipv6 addresses and returns if the first address(a) is
 *        greater than the second address(b) or not.
 *
 * \param a The first ipv6 address to be compared.
 * \param b The second ipv6 address to be compared.
 *
 * \retval 1 If a > b.
 * \retval 0 Otherwise, i.e. a <= b.
 */
int AddressIPv6Gt(uint32_t *a, uint32_t *b)
{
    int i = 0;

    for (i = 0; i < 4; i++) {
        if (ntohl(a[i]) > ntohl(b[i]))
            return 1;
        if (ntohl(a[i]) < ntohl(b[i]))
            break;
    }

    return 0;
}

/**
 * \brief Compares 2 ipv6 addresses and returns if the addresses are equal
 *        or not.
 *
 * \param a The first ipv6 address to be compared.
 * \param b The second ipv6 address to be compared.
 *
 * \retval 1 If a == b.
 * \retval 0 Otherwise.
 */
int AddressIPv6Eq(uint32_t *a, uint32_t *b)
{
    int i = 0;

    for (i = 0; i < 4; i++) {
        if (a[i] != b[i])
            return 0;
    }

    return 1;
}

/**
 * \brief Compares 2 ipv6 addresses and returns if the first address(a) is less
 *        than or equal to the second address(b) or not.
 *
 * \param a The first ipv6 address to be compared.
 * \param b The second ipv6 address to be compared.
 *
 * \retval 1 If a <= b.
 * \retval 0 Otherwise, i.e. a > b.
 */
int AddressIPv6Le(uint32_t *a, uint32_t *b)
{

    if (AddressIPv6Eq(a, b) == 1)
        return 1;
    if (AddressIPv6Lt(a, b) == 1)
        return 1;

    return 0;
}

/**
 * \brief Compares 2 ipv6 addresses and returns if the first address(a) is
 *        greater than or equal to the second address(b) or not.
 *
 * \param a The first ipv6 address to be compared.
 * \param b The second ipv6 address to be compared.
 *
 * \retval 1 If a >= b.
 * \retval 0 Otherwise, i.e. a < b.
 */
int AddressIPv6Ge(uint32_t *a, uint32_t *b)
{

    if (AddressIPv6Eq(a, b) == 1)
        return 1;
    if (AddressIPv6Gt(a, b) == 1)
        return 1;

    return 0;
}

/**
 * \brief Compares 2 addresses(address ranges) and returns the relationship
 *        between the 2 addresses.
 *
 * \param a Pointer to the first address instance to be compared.
 * \param b Pointer to the second address instance to be compared.
 *
 * \retval ADDRESS_EQ If the 2 address ranges a and b, are equal.
 * \retval ADDRESS_ES b encapsulates a. b_ip1[...a_ip1...a_ip2...]b_ip2.
 * \retval ADDRESS_EB a encapsulates b. a_ip1[...b_ip1....b_ip2...]a_ip2.
 * \retval ADDRESS_LE a_ip1(...b_ip1==a_ip2...)b_ip2
 * \retval ADDRESS_LT a_ip1(...b_ip1...a_ip2...)b_ip2
 * \retval ADDRESS_GE b_ip1(...a_ip1==b_ip2...)a_ip2
 * \retval ADDRESS_GT a_ip1 > b_ip2, i.e. the address range for 'a' starts only
 *                    after the end of the address range for 'b'
 */
int DetectAddressCmpIPv6(DetectAddress *a, DetectAddress *b)
{
    if (AddressIPv6Eq(a->ip, b->ip) == 1 &&
        AddressIPv6Eq(a->ip2, b->ip2) == 1) {
        return ADDRESS_EQ;
    } else if (AddressIPv6Ge(a->ip, b->ip) == 1 &&
               AddressIPv6Le(a->ip, b->ip2) == 1 &&
               AddressIPv6Le(a->ip2, b->ip2) == 1) {
        return ADDRESS_ES;
    } else if (AddressIPv6Le(a->ip, b->ip) == 1 &&
               AddressIPv6Ge(a->ip2, b->ip2) == 1) {
        return ADDRESS_EB;
    } else if (AddressIPv6Lt(a->ip, b->ip) == 1 &&
               AddressIPv6Lt(a->ip2, b->ip2) == 1 &&
               AddressIPv6Ge(a->ip2, b->ip) == 1) {
        return ADDRESS_LE;
    } else if (AddressIPv6Lt(a->ip, b->ip) == 1 &&
               AddressIPv6Lt(a->ip2, b->ip2) == 1) {
        return ADDRESS_LT;
    } else if (AddressIPv6Gt(a->ip, b->ip) == 1 &&
               AddressIPv6Le(a->ip, b->ip2) == 1 &&
               AddressIPv6Gt(a->ip2, b->ip2) == 1) {
        return ADDRESS_GE;
    } else if (AddressIPv6Gt(a->ip, b->ip2) == 1) {
        return ADDRESS_GT;
    } else {
        /* should be unreachable */
        SCLogDebug("Internal Error: should be unreachable\n");
    }

    return ADDRESS_ER;
}

/**
 * \brief Takes an IPv6 address in a, and returns in b an IPv6 address which is
 *        one less than the IPv6 address in a.  The address sent in a is in host
 *        order, and the address in b will be returned in network order!
 *
 * \param a Pointer to an IPv6 address in host order.
 * \param b Pointer to an IPv6 address store in memory which has to be updated
 *          with the new address(a - 1).
 */
static void AddressCutIPv6CopySubOne(uint32_t *a, uint32_t *b)
{
    uint32_t t = a[3];

    b[0] = a[0];
    b[1] = a[1];
    b[2] = a[2];
    b[3] = a[3];

    b[3]--;
    if (b[3] > t) {
        t = b[2];
        b[2]--;
        if (b[2] > t) {
            t = b[1];
            b[1]--;
            if (b[1] > t)
                b[0]--;
        }
    }

    b[0] = htonl(b[0]);
    b[1] = htonl(b[1]);
    b[2] = htonl(b[2]);
    b[3] = htonl(b[3]);

    return;
}

/**
 * \brief Takes an IPv6 address in a, and returns in b an IPv6 address which is
 *        one more than the IPv6 address in a.  The address sent in a is in host
 *        order, and the address in b will be returned in network order!
 *
 * \param a Pointer to an IPv6 address in host order.
 * \param b Pointer to an IPv6 address store in memory which has to be updated
 *          with the new address(a + 1).
 */
static void AddressCutIPv6CopyAddOne(uint32_t *a, uint32_t *b)
{
    uint32_t t = a[3];

    b[0] = a[0];
    b[1] = a[1];
    b[2] = a[2];
    b[3] = a[3];

    b[3]++;
    if (b[3] < t) {
        t = b[2];
        b[2]++;
        if (b[2] < t) {
            t = b[1];
            b[1]++;
            if (b[1] < t)
                b[0]++;
        }
    }

    b[0] = htonl(b[0]);
    b[1] = htonl(b[1]);
    b[2] = htonl(b[2]);
    b[3] = htonl(b[3]);

    return;
}

/**
 * \brief Copies an IPv6 address in a to the b.  The address in a is in host
 *        order and will be copied in network order to b!
 *
 * \param a Pointer to the IPv6 address to be copied.
 * \param b Pointer to an IPv6 address in memory which will be updated with the
 *          address in a.
 */
static void AddressCutIPv6Copy(uint32_t *a, uint32_t *b)
{
    b[0] = htonl(a[0]);
    b[1] = htonl(a[1]);
    b[2] = htonl(a[2]);
    b[3] = htonl(a[3]);

    return;
}

int DetectAddressCutIPv6(DetectEngineCtx *de_ctx, DetectAddress *a,
                         DetectAddress *b, DetectAddress **c)
{
    uint32_t a_ip1[4] = { ntohl(a->ip[0]), ntohl(a->ip[1]),
                          ntohl(a->ip[2]), ntohl(a->ip[3]) };
    uint32_t a_ip2[4] = { ntohl(a->ip2[0]), ntohl(a->ip2[1]),
                          ntohl(a->ip2[2]), ntohl(a->ip2[3]) };
    uint32_t b_ip1[4] = { ntohl(b->ip[0]), ntohl(b->ip[1]),
                          ntohl(b->ip[2]), ntohl(b->ip[3]) };
    uint32_t b_ip2[4] = { ntohl(b->ip2[0]), ntohl(b->ip2[1]),
                          ntohl(b->ip2[2]), ntohl(b->ip2[3]) };

    DetectPort *port = NULL;
    DetectAddress *tmp = NULL;

    /* default to NULL */
    *c = NULL;

    int r = DetectAddressCmpIPv6(a, b);
    if (r != ADDRESS_ES && r != ADDRESS_EB && r != ADDRESS_LE && r != ADDRESS_GE) {
        goto error;
    }

    /* get a place to temporary put sigs lists */
    tmp = DetectAddressInit();
    if (tmp == NULL)
        goto error;
    memset(tmp,0,sizeof(DetectAddress));

    /* we have 3 parts: [aaa[abab]bbb]
     * part a: a_ip1 <-> b_ip1 - 1
     * part b: b_ip1 <-> a_ip2
     * part c: a_ip2 + 1 <-> b_ip2
     */
    if (r == ADDRESS_LE) {
        AddressCutIPv6Copy(a_ip1, a->ip);
        AddressCutIPv6CopySubOne(b_ip1, a->ip2);

        AddressCutIPv6Copy(b_ip1, b->ip);
        AddressCutIPv6Copy(a_ip2, b->ip2);

        DetectAddress *tmp_c;
        tmp_c = DetectAddressInit();
        if (tmp_c == NULL)
            goto error;
        tmp_c->family  = AF_INET6;

        AddressCutIPv6CopyAddOne(a_ip2, tmp_c->ip);
        AddressCutIPv6Copy(b_ip2, tmp_c->ip2);

        *c = tmp_c;

        /* copy old b to c */
        SigGroupHeadCopySigs(de_ctx, b->sh, &tmp_c->sh);
        /* copy old b to a */
        SigGroupHeadCopySigs(de_ctx, a->sh, &b->sh);

        for (port = b->port; port != NULL; port = port->next)
            DetectPortInsertCopy(de_ctx, &tmp_c->port, port);
        for (port = a->port; port != NULL; port = port->next)
            DetectPortInsertCopy(de_ctx, &b->port, port);

        tmp_c->cnt += b->cnt;
        b->cnt += a->cnt;

    /* we have 3 parts: [bbb[baba]aaa]
     * part a: b_ip1 <-> a_ip1 - 1
     * part b: a_ip1 <-> b_ip2
     * part c: b_ip2 + 1 <-> a_ip2
     */
    } else if (r == ADDRESS_GE) {
        AddressCutIPv6Copy(b_ip1, a->ip);
        AddressCutIPv6CopySubOne(a_ip1, a->ip2);

        AddressCutIPv6Copy(a_ip1, b->ip);
        AddressCutIPv6Copy(b_ip2, b->ip2);

        DetectAddress *tmp_c;
        tmp_c = DetectAddressInit();
        if (tmp_c == NULL)
            goto error;
        tmp_c->family  = AF_INET6;

        AddressCutIPv6CopyAddOne(b_ip2, tmp_c->ip);
        AddressCutIPv6Copy(a_ip2, tmp_c->ip2);
        *c = tmp_c;

        /* 'a' gets clean and then 'b' sigs
         * 'b' gets clean, then 'a' then 'b' sigs
         * 'c' gets 'a' sigs */
        /* store old a list */
        SigGroupHeadCopySigs(de_ctx, a->sh, &tmp->sh);
        /* clean a list */
        SigGroupHeadClearSigs(a->sh);
        /* copy old b to c */
        SigGroupHeadCopySigs(de_ctx, tmp->sh, &tmp_c->sh);
        /* copy old b to a */
        SigGroupHeadCopySigs(de_ctx, b->sh, &a->sh);
        /* prepend old a before b */
        SigGroupHeadCopySigs(de_ctx, tmp->sh, &b->sh);

        /* clean tmp list */
        SigGroupHeadClearSigs(tmp->sh);

        for (port = a->port; port != NULL; port = port->next)
            DetectPortInsertCopy(de_ctx,&tmp->port, port);
        for (port = b->port; port != NULL; port = port->next)
            DetectPortInsertCopy(de_ctx,&a->port, port);

        for (port = tmp->port; port != NULL; port = port->next)
            DetectPortInsertCopy(de_ctx,&b->port, port);
        for (port = tmp->port; port != NULL; port = port->next)
            DetectPortInsertCopy(de_ctx,&tmp_c->port, port);

        tmp->cnt += a->cnt;
        a->cnt = 0;
        tmp_c->cnt += tmp->cnt;
        a->cnt += b->cnt;
        b->cnt += tmp->cnt;
        tmp->cnt = 0;

    /* we have 2 or three parts:
     *
     * 2 part: [[abab]bbb] or [bbb[baba]]
     * part a: a_ip1 <-> a_ip2
     * part b: a_ip2 + 1 <-> b_ip2
     *
     * part a: b_ip1 <-> a_ip1 - 1
     * part b: a_ip1 <-> a_ip2
     *
     * 3 part [bbb[aaa]bbb]
     * part a: b_ip1 <-> a_ip1 - 1
     * part b: a_ip1 <-> a_ip2
     * part c: a_ip2 + 1 <-> b_ip2
     */
    } else if (r == ADDRESS_ES) {
        if (AddressIPv6Eq(a_ip1, b_ip1) == 1) {
            AddressCutIPv6Copy(a_ip1, a->ip);
            AddressCutIPv6Copy(a_ip2, a->ip2);

            AddressCutIPv6CopyAddOne(a_ip2, b->ip);
            AddressCutIPv6Copy(b_ip2, b->ip2);

            /* 'b' overlaps 'a' so 'a' needs the 'b' sigs */
            SigGroupHeadCopySigs(de_ctx, b->sh, &a->sh);

            for (port = b->port; port != NULL; port = port->next)
                DetectPortInsertCopy(de_ctx,&a->port, port);

            a->cnt += b->cnt;

        } else if (AddressIPv6Eq(a_ip2, b_ip2) == 1) {
            AddressCutIPv6Copy(b_ip1, a->ip);
            AddressCutIPv6CopySubOne(a_ip1, a->ip2);

            AddressCutIPv6Copy(a_ip1, b->ip);
            AddressCutIPv6Copy(a_ip2, b->ip2);

            /* 'a' overlaps 'b' so 'b' needs the 'a' sigs */
            SigGroupHeadCopySigs(de_ctx, a->sh, &tmp->sh);
            SigGroupHeadClearSigs(a->sh);
            SigGroupHeadCopySigs(de_ctx, b->sh, &a->sh);
            SigGroupHeadCopySigs(de_ctx, tmp->sh, &b->sh);
            SigGroupHeadClearSigs(tmp->sh);

            for (port = a->port; port != NULL; port = port->next)
                DetectPortInsertCopy(de_ctx,&tmp->port, a->port);
            for (port = b->port; port != NULL; port = port->next)
                DetectPortInsertCopy(de_ctx,&a->port, port);
            for (port = tmp->port; port != NULL; port = port->next)
                DetectPortInsertCopy(de_ctx,&b->port, port);

            tmp->cnt += a->cnt;
            a->cnt = 0;
            a->cnt += b->cnt;
            b->cnt += tmp->cnt;
            tmp->cnt = 0;
        } else {
            AddressCutIPv6Copy(b_ip1, a->ip);
            AddressCutIPv6CopySubOne(a_ip1, a->ip2);

            AddressCutIPv6Copy(a_ip1, b->ip);
            AddressCutIPv6Copy(a_ip2, b->ip2);

            DetectAddress *tmp_c;
            tmp_c = DetectAddressInit();
            if (tmp_c == NULL) {
                goto error;
            }
            tmp_c->family = AF_INET6;
            AddressCutIPv6CopyAddOne(a_ip2, tmp_c->ip);
            AddressCutIPv6Copy(b_ip2, tmp_c->ip2);
            *c = tmp_c;

            /* 'a' gets clean and then 'b' sigs
             * 'b' gets clean, then 'a' then 'b' sigs
             * 'c' gets 'b' sigs */
            /* store old a list */
            SigGroupHeadCopySigs(de_ctx, a->sh, &tmp->sh);
            /* clean a list */
            SigGroupHeadClearSigs(a->sh);
            /* copy old b to c */
            SigGroupHeadCopySigs(de_ctx, b->sh, &tmp_c->sh);
            /* copy old b to a */
            SigGroupHeadCopySigs(de_ctx, b->sh, &a->sh);
            /* prepend old a before b */
            SigGroupHeadCopySigs(de_ctx, tmp->sh, &b->sh);

            /* clean tmp list */
            SigGroupHeadClearSigs(tmp->sh);

            for (port = a->port; port != NULL; port = port->next)
                DetectPortInsertCopy(de_ctx,&tmp->port, port);
            for (port = b->port; port != NULL; port = port->next)
                DetectPortInsertCopy(de_ctx,&tmp_c->port, port);
            for (port = b->port; port != NULL; port = port->next)
                DetectPortInsertCopy(de_ctx,&a->port, port);

            for (port = tmp->port; port != NULL; port = port->next)
                DetectPortInsertCopy(de_ctx,&b->port, port);

            tmp->cnt += a->cnt;
            a->cnt = 0;
            tmp_c->cnt += b->cnt;
            a->cnt += b->cnt;
            b->cnt += tmp->cnt;
            tmp->cnt = 0;
        }
    /* we have 2 or three parts:
     *
     * 2 part: [[baba]aaa] or [aaa[abab]]
     * part a: b_ip1 <-> b_ip2
     * part b: b_ip2 + 1 <-> a_ip2
     *
     * part a: a_ip1 <-> b_ip1 - 1
     * part b: b_ip1 <-> b_ip2
     *
     * 3 part [aaa[bbb]aaa]
     * part a: a_ip1 <-> b_ip2 - 1
     * part b: b_ip1 <-> b_ip2
     * part c: b_ip2 + 1 <-> a_ip2
     */
    } else if (r == ADDRESS_EB) {
        if (AddressIPv6Eq(a_ip1, b_ip1) == 1) {
            AddressCutIPv6Copy(b_ip1, a->ip);
            AddressCutIPv6Copy(b_ip2, a->ip2);

            AddressCutIPv6CopyAddOne(b_ip2, b->ip);
            AddressCutIPv6Copy(a_ip2, b->ip2);

            /* 'b' overlaps 'a' so a needs the 'b' sigs */
            SigGroupHeadCopySigs(de_ctx, b->sh, &tmp->sh);
            SigGroupHeadClearSigs(b->sh);
            SigGroupHeadCopySigs(de_ctx, a->sh, &b->sh);
            SigGroupHeadCopySigs(de_ctx, tmp->sh, &a->sh);
            SigGroupHeadClearSigs(tmp->sh);

            for (port = b->port; port != NULL; port = port->next)
                DetectPortInsertCopy(de_ctx,&tmp->port, b->port);
            for (port = a->port; port != NULL; port = port->next)
                DetectPortInsertCopy(de_ctx,&b->port, port);
            for (port = tmp->port; port != NULL; port = port->next)
                DetectPortInsertCopy(de_ctx,&a->port, port);

            tmp->cnt += b->cnt;
            b->cnt = 0;
            b->cnt += a->cnt;
            a->cnt += tmp->cnt;
            tmp->cnt = 0;
        } else if (AddressIPv6Eq(a_ip2, b_ip2) == 1) {
            AddressCutIPv6Copy(a_ip1, a->ip);
            AddressCutIPv6CopySubOne(b_ip1, a->ip2);

            AddressCutIPv6Copy(b_ip1, b->ip);
            AddressCutIPv6Copy(b_ip2, b->ip2);

            /* 'a' overlaps 'b' so a needs the 'a' sigs */
            SigGroupHeadCopySigs(de_ctx, a->sh, &b->sh);

            for (port = a->port; port != NULL; port = port->next)
                DetectPortInsertCopy(de_ctx,&b->port, port);

            b->cnt += a->cnt;
        } else {
            AddressCutIPv6Copy(a_ip1, a->ip);
            AddressCutIPv6CopySubOne(b_ip1, a->ip2);

            AddressCutIPv6Copy(b_ip1, b->ip);
            AddressCutIPv6Copy(b_ip2, b->ip2);

            DetectAddress *tmp_c;
            tmp_c = DetectAddressInit();
            if (tmp_c == NULL)
                goto error;

            tmp_c->family  = AF_INET6;
            AddressCutIPv6CopyAddOne(b_ip2, tmp_c->ip);
            AddressCutIPv6Copy(a_ip2, tmp_c->ip2);
            *c = tmp_c;

            /* 'a' stays the same wrt sigs
             * 'b' keeps it's own sigs and gets a's sigs prepended
             * 'c' gets 'a' sigs */
            SigGroupHeadCopySigs(de_ctx, a->sh, &b->sh);
            SigGroupHeadCopySigs(de_ctx, a->sh, &tmp_c->sh);

            for (port = a->port; port != NULL; port = port->next)
                DetectPortInsertCopy(de_ctx,&b->port, port);
            for (port = a->port; port != NULL; port = port->next)
                DetectPortInsertCopy(de_ctx,&tmp_c->port, port);

            b->cnt += a->cnt;
            tmp_c->cnt += a->cnt;
        }
    }

    if (tmp != NULL)
        DetectAddressFree(tmp);

    return 0;

error:
    if (tmp != NULL)
        DetectAddressFree(tmp);
    return -1;
}

#if 0
int DetectAddressCutIPv6(DetectAddressData *a, DetectAddressData *b,
                         DetectAddressData **c)
{
    uint32_t a_ip1[4] = { ntohl(a->ip[0]), ntohl(a->ip[1]),
                          ntohl(a->ip[2]), ntohl(a->ip[3]) };
    uint32_t a_ip2[4] = { ntohl(a->ip2[0]), ntohl(a->ip2[1]),
                          ntohl(a->ip2[2]), ntohl(a->ip2[3]) };
    uint32_t b_ip1[4] = { ntohl(b->ip[0]), ntohl(b->ip[1]),
                          ntohl(b->ip[2]), ntohl(b->ip[3]) };
    uint32_t b_ip2[4] = { ntohl(b->ip2[0]), ntohl(b->ip2[1]),
                          ntohl(b->ip2[2]), ntohl(b->ip2[3]) };

    /* default to NULL */
    *c = NULL;

    int r = DetectAddressCmpIPv6(a, b);
    if (r != ADDRESS_ES && r != ADDRESS_EB && r != ADDRESS_LE && r != ADDRESS_GE) {
        goto error;
    }

    /* we have 3 parts: [aaa[abab]bbb]
     * part a: a_ip1 <-> b_ip1 - 1
     * part b: b_ip1 <-> a_ip2
     * part c: a_ip2 + 1 <-> b_ip2
     */
    if (r == ADDRESS_LE) {
        AddressCutIPv6Copy(a_ip1, a->ip);
        AddressCutIPv6CopySubOne(b_ip1, a->ip2);

        AddressCutIPv6Copy(b_ip1, b->ip);
        AddressCutIPv6Copy(a_ip2, b->ip2);

        DetectAddressData *tmp_c;
        tmp_c = DetectAddressDataInit();
        if (tmp_c == NULL)
            goto error;
        tmp_c->family = AF_INET6;

        AddressCutIPv6CopyAddOne(a_ip2, tmp_c->ip);
        AddressCutIPv6Copy(b_ip2, tmp_c->ip2);

        *c = tmp_c;

    /* we have 3 parts: [bbb[baba]aaa]
     * part a: b_ip1 <-> a_ip1 - 1
     * part b: a_ip1 <-> b_ip2
     * part c: b_ip2 + 1 <-> a_ip2
     */
    } else if (r == ADDRESS_GE) {
        AddressCutIPv6Copy(b_ip1, a->ip);
        AddressCutIPv6CopySubOne(a_ip1, a->ip2);

        AddressCutIPv6Copy(a_ip1, b->ip);
        AddressCutIPv6Copy(b_ip2, b->ip2);

        DetectAddressData *tmp_c;
        tmp_c = DetectAddressDataInit();
        if (tmp_c == NULL)
            goto error;
        tmp_c->family  = AF_INET6;

        AddressCutIPv6CopyAddOne(b_ip2, tmp_c->ip);
        AddressCutIPv6Copy(a_ip2, tmp_c->ip2);

        *c = tmp_c;

    /* we have 2 or three parts:
     *
     * 2 part: [[abab]bbb] or [bbb[baba]]
     * part a: a_ip1 <-> a_ip2
     * part b: a_ip2 + 1 <-> b_ip2
     *
     * part a: b_ip1 <-> a_ip1 - 1
     * part b: a_ip1 <-> a_ip2
     *
     * 3 part [bbb[aaa]bbb]
     * part a: b_ip1 <-> a_ip1 - 1
     * part b: a_ip1 <-> a_ip2
     * part c: a_ip2 + 1 <-> b_ip2
     */
    } else if (r == ADDRESS_ES) {
        if (AddressIPv6Eq(a_ip1,b_ip1) == 1) {
            AddressCutIPv6Copy(a_ip1, a->ip);
            AddressCutIPv6Copy(a_ip2, a->ip2);

            AddressCutIPv6CopyAddOne(a_ip2, b->ip);
            AddressCutIPv6Copy(b_ip2, b->ip2);
        } else if (AddressIPv6Eq(a_ip2, b_ip2) == 1) {
            AddressCutIPv6Copy(b_ip1, a->ip);
            AddressCutIPv6CopySubOne(a_ip1, a->ip2);

            AddressCutIPv6Copy(a_ip1, b->ip);
            AddressCutIPv6Copy(a_ip2, b->ip2);
        } else {
            AddressCutIPv6Copy(b_ip1, a->ip);
            AddressCutIPv6CopySubOne(a_ip1, a->ip2);

            AddressCutIPv6Copy(a_ip1, b->ip);
            AddressCutIPv6Copy(a_ip2, b->ip2);

            DetectAddressData *tmp_c;
            tmp_c = DetectAddressDataInit();
            if (tmp_c == NULL)
                goto error;

            tmp_c->family  = AF_INET6;

            AddressCutIPv6CopyAddOne(a_ip2, tmp_c->ip);
            AddressCutIPv6Copy(b_ip2, tmp_c->ip2);
            *c = tmp_c;
        }
    /* we have 2 or three parts:
     *
     * 2 part: [[baba]aaa] or [aaa[abab]]
     * part a: b_ip1 <-> b_ip2
     * part b: b_ip2 + 1 <-> a_ip2
     *
     * part a: a_ip1 <-> b_ip1 - 1
     * part b: b_ip1 <-> b_ip2
     *
     * 3 part [aaa[bbb]aaa]
     * part a: a_ip1 <-> b_ip2 - 1
     * part b: b_ip1 <-> b_ip2
     * part c: b_ip2 + 1 <-> a_ip2
     */
    } else if (r == ADDRESS_EB) {
        if (AddressIPv6Eq(a_ip1, b_ip1) == 1) {
            AddressCutIPv6Copy(b_ip1, a->ip);
            AddressCutIPv6Copy(b_ip2, a->ip2);

            AddressCutIPv6CopyAddOne(b_ip2, b->ip);
            AddressCutIPv6Copy(a_ip2, b->ip2);
        } else if (AddressIPv6Eq(a_ip2, b_ip2) == 1) {
            AddressCutIPv6Copy(a_ip1, a->ip);
            AddressCutIPv6CopySubOne(b_ip1, a->ip2);

            AddressCutIPv6Copy(b_ip1, b->ip);
            AddressCutIPv6Copy(b_ip2, b->ip2);
        } else {
            AddressCutIPv6Copy(a_ip1, a->ip);
            AddressCutIPv6CopySubOne(b_ip1, a->ip2);

            AddressCutIPv6Copy(b_ip1, b->ip);
            AddressCutIPv6Copy(b_ip2, b->ip2);

            DetectAddressData *tmp_c;
            tmp_c = DetectAddressDataInit();
            if (tmp_c == NULL)
                goto error;
            tmp_c->family  = AF_INET6;

            AddressCutIPv6CopyAddOne(b_ip2, tmp_c->ip);
            AddressCutIPv6Copy(a_ip2, tmp_c->ip2);
            *c = tmp_c;
        }
    }

    return 0;

error:
    return -1;
}
#endif

/**
 * \brief Cuts and returns an address range, which is the complement of the
 *        address range that is supplied as the argument.
 *
 *        For example:
 *
 *        If a = ::-2000::,
 *            then a = 2000::1-FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF and b = NULL
 *        If a = 2000::1-FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF,
 *            then a = ::-2000:: and b = NULL
 *        If a = 2000::1-20FF::2,
 *            then a = ::-2000:: and
 *                 b = 20FF::3-FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF
 *
 * \param a Pointer to an address range (DetectAddress) instance whose complement
 *          has to be returned in a and b.
 * \param b Pointer to DetectAddress pointer, that will be supplied back with a
 *          new DetectAddress instance, if the complement demands so.
 *
 * \retval  0 On success.
 * \retval -1 On failure.
 */
int DetectAddressCutNotIPv6(DetectAddress *a, DetectAddress **b)
{
    uint32_t a_ip1[4] = { ntohl(a->ip[0]), ntohl(a->ip[1]),
                          ntohl(a->ip[2]), ntohl(a->ip[3]) };
    uint32_t a_ip2[4] = { ntohl(a->ip2[0]), ntohl(a->ip2[1]),
                          ntohl(a->ip2[2]), ntohl(a->ip2[3]) };
    uint32_t ip_nul[4] = { 0x00000000, 0x00000000, 0x00000000, 0x00000000 };
    uint32_t ip_max[4] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };

    /* default to NULL */
    *b = NULL;

    if (!(a_ip1[0] == 0x00000000 && a_ip1[1] == 0x00000000 &&
          a_ip1[2] == 0x00000000 && a_ip1[3] == 0x00000000) &&
        !(a_ip2[0] == 0xFFFFFFFF && a_ip2[1] == 0xFFFFFFFF &&
          a_ip2[2] == 0xFFFFFFFF && a_ip2[3] == 0xFFFFFFFF)) {
        AddressCutIPv6Copy(ip_nul, a->ip);
        AddressCutIPv6CopySubOne(a_ip1, a->ip2);

        DetectAddress *tmp_b = DetectAddressInit();
        if (tmp_b == NULL)
            goto error;

        tmp_b->family  = AF_INET6;
        AddressCutIPv6CopyAddOne(a_ip2, tmp_b->ip);
        AddressCutIPv6Copy(ip_max, tmp_b->ip2);
        *b = tmp_b;
    } else if ((a_ip1[0] == 0x00000000 && a_ip1[1] == 0x00000000 &&
                a_ip1[2] == 0x00000000 && a_ip1[3] == 0x00000000) &&
               !(a_ip2[0] == 0xFFFFFFFF && a_ip2[1] == 0xFFFFFFFF &&
                a_ip2[2] == 0xFFFFFFFF && a_ip2[3] == 0xFFFFFFFF)) {
        AddressCutIPv6CopyAddOne(a_ip2, a->ip);
        AddressCutIPv6Copy(ip_max, a->ip2);
    } else if (!(a_ip1[0] == 0x00000000 && a_ip1[1] == 0x00000000 &&
                 a_ip1[2] == 0x00000000 && a_ip1[3] == 0x00000000) &&
               (a_ip2[0] == 0xFFFFFFFF && a_ip2[1] == 0xFFFFFFFF &&
                a_ip2[2] == 0xFFFFFFFF && a_ip2[3] == 0xFFFFFFFF)) {
        AddressCutIPv6Copy(ip_nul, a->ip);
        AddressCutIPv6CopySubOne(a_ip1, a->ip2);
    } else {
        goto error;
    }

    return 0;

error:
    return -1;
}

/**
 * \brief Extends a target address range if the the source address range is
 *        wider than the target address range on either sides.
 *
 *        Every address is a range, i.e. address->ip1....address->ip2.  For
 *        example 2000::-2010::
 *        if source->ip1 is smaller than target->ip1, it indicates that the
 *        source's left address limit is greater(range wise) than the target's
 *        left address limit, and hence we reassign the target's left address
 *        limit to source's left address limit.
 *        Similary if source->ip2 is greater than target->ip2, it indicates that
 *        the source's right address limit is greater(range wise) than the
 *        target's right address limit, and hence we reassign the target's right
 *        address limit to source's right address limit.
 *
 * \param de_ctx Pointer to the detection engine context.
 * \param target Pointer to the target DetectAddress instance that has to be
 *               updated.
 * \param source Pointer to the source DetectAddress instance that is used
 *               to decided whether we extend the target's address range.
 *
 * \retval  0 On success.
 * \retval -1 On failure.
 */
int DetectAddressJoinIPv6(DetectEngineCtx *de_ctx, DetectAddress *target,
                          DetectAddress *source)
{
    if (AddressIPv6Lt(source->ip, target->ip)) {
        target->ip[0] = source->ip[0];
        target->ip[1] = source->ip[1];
        target->ip[2] = source->ip[2];
        target->ip[3] = source->ip[3];
    }

    if (AddressIPv6Gt(source->ip, target->ip)) {
        target->ip2[0] = source->ip2[0];
        target->ip2[1] = source->ip2[1];
        target->ip2[2] = source->ip2[2];
        target->ip2[3] = source->ip2[3];
    }

    return 0;
}


/***************************************Unittests******************************/

#ifdef UNITTESTS

int AddressTestIPv6Gt01(void)
{
    int result = 0;

    uint32_t a[4] = { 1, 2, 3, 4 };
    uint32_t b[4] = { 0, 2, 3, 4 };

    if (AddressIPv6Gt(a, b) == 1)
        result = 1;

    return result;
}

int AddressTestIPv6Gt02(void)
{
    int result = 0;

    uint32_t a[4] = { 0, 2, 3, 4 };
    uint32_t b[4] = { 1, 2, 3, 4 };

    if (AddressIPv6Gt(a, b) == 0)
        result = 1;

    return result;
}

int AddressTestIPv6Gt03(void)
{
    int result = 0;

    uint32_t a[4] = { 1, 2, 3, 4 };
    uint32_t b[4] = { 1, 2, 3, 4 };

    if (AddressIPv6Gt(a, b) == 0)
        result = 1;

    return result;
}

int AddressTestIPv6Gt04(void)
{
    int result = 0;

    uint32_t a[4] = { 1, 2, 3, 5 };
    uint32_t b[4] = { 1, 2, 3, 4 };

    if (AddressIPv6Gt(a, b) == 1)
        result = 1;

    return result;
}

int AddressTestIPv6Lt01(void)
{
    int result = 0;

    uint32_t a[4] = { 0, 2, 3, 4 };
    uint32_t b[4] = { 1, 2, 3, 4 };

    if (AddressIPv6Lt(a, b) == 1)
        result = 1;

    return result;
}

int AddressTestIPv6Lt02(void)
{
    int result = 0;

    uint32_t a[4] = { 1, 2, 3, 4 };
    uint32_t b[4] = { 0, 2, 3, 4 };

    if (AddressIPv6Lt(a, b) == 0)
        result = 1;

    return result;
}

int AddressTestIPv6Lt03(void)
{
    int result = 0;

    uint32_t a[4] = { 1, 2, 3, 4 };
    uint32_t b[4] = { 1, 2, 3, 4 };

    if (AddressIPv6Lt(a, b) == 0)
        result = 1;

    return result;
}

int AddressTestIPv6Lt04(void)
{
    int result = 0;

    uint32_t a[4] = { 1, 2, 3, 4 };
    uint32_t b[4] = { 1, 2, 3, 5 };

    if (AddressIPv6Lt(a, b) == 1)
        result = 1;

    return result;
}

int AddressTestIPv6Eq01(void)
{
    int result = 0;

    uint32_t a[4] = { 0, 2, 3, 4 };
    uint32_t b[4] = { 1, 2, 3, 4 };

    if (AddressIPv6Eq(a, b) == 0)
        result = 1;

    return result;
}

int AddressTestIPv6Eq02(void)
{
    int result = 0;

    uint32_t a[4] = { 1, 2, 3, 4 };
    uint32_t b[4] = { 0, 2, 3, 4 };

    if (AddressIPv6Eq(a, b) == 0)
        result = 1;

    return result;
}

int AddressTestIPv6Eq03(void)
{
    int result = 0;

    uint32_t a[4] = { 1, 2, 3, 4 };
    uint32_t b[4] = { 1, 2, 3, 4 };

    if (AddressIPv6Eq(a, b) == 1)
        result = 1;

    return result;
}

int AddressTestIPv6Eq04(void)
{
    int result = 0;

    uint32_t a[4] = { 1, 2, 3, 4 };
    uint32_t b[4] = { 1, 2, 3, 5 };

    if (AddressIPv6Eq(a, b) == 0)
        result = 1;

    return result;
}

int AddressTestIPv6Le01(void)
{
    int result = 0;

    uint32_t a[4] = { 0, 2, 3, 4 };
    uint32_t b[4] = { 1, 2, 3, 4 };

    if (AddressIPv6Le(a, b) == 1)
        result = 1;

    return result;
}

int AddressTestIPv6Le02(void)
{
    int result = 0;

    uint32_t a[4] = { 1, 2, 3, 4 };
    uint32_t b[4] = { 0, 2, 3, 4 };

    if (AddressIPv6Le(a, b) == 0)
        result = 1;

    return result;
}

int AddressTestIPv6Le03(void)
{
    int result = 0;

    uint32_t a[4] = { 1, 2, 3, 4 };
    uint32_t b[4] = { 1, 2, 3, 4 };

    if (AddressIPv6Le(a, b) == 1)
        result = 1;

    return result;
}

int AddressTestIPv6Le04(void)
{
    int result = 0;

    uint32_t a[4] = { 1, 2, 3, 4 };
    uint32_t b[4] = { 1, 2, 3, 5 };

    if (AddressIPv6Le(a, b) == 1)
        result = 1;

    return result;
}

int AddressTestIPv6Le05(void)
{
    int result = 0;

    uint32_t a[4];
    uint32_t b[4];
    struct in6_addr in6;

    inet_pton(AF_INET6, "1999:ffff:ffff:ffff:ffff:ffff:ffff:ffff", &in6);
    memcpy(&a, &in6.s6_addr, sizeof(in6.s6_addr));

    inet_pton(AF_INET6, "2000::0", &in6);
    memcpy(&b, &in6.s6_addr, sizeof(in6.s6_addr));

    if (AddressIPv6Le(a, b) == 1)
        result = 1;

    return result;
}

int AddressTestIPv6Ge01(void)
{
    int result = 0;

    uint32_t a[4] = { 0, 2, 3, 4 };
    uint32_t b[4] = { 1, 2, 3, 4 };

    if (AddressIPv6Ge(a, b) == 0)
        result = 1;

    return result;
}

int AddressTestIPv6Ge02(void)
{
    int result = 0;

    uint32_t a[4] = { 1, 2, 3, 4 };
    uint32_t b[4] = { 0, 2, 3, 4 };

    if (AddressIPv6Ge(a, b) == 1)
        result = 1;

    return result;
}

int AddressTestIPv6Ge03(void)
{
    int result = 0;

    uint32_t a[4] = { 1, 2, 3, 4 };
    uint32_t b[4] = { 1, 2, 3, 4 };

    if (AddressIPv6Ge(a, b) == 1)
        result = 1;

    return result;
}

int AddressTestIPv6Ge04(void)
{
    int result = 0;

    uint32_t a[4] = { 1, 2, 3, 4 };
    uint32_t b[4] = { 1, 2, 3, 5 };

    if (AddressIPv6Ge(a, b) == 0)
        result = 1;

    return result;
}

int AddressTestIPv6Ge05(void)
{
    int result = 0;

    uint32_t a[4];
    uint32_t b[4];
    struct in6_addr in6;

    inet_pton(AF_INET6, "1999:ffff:ffff:ffff:ffff:ffff:ffff:ffff", &in6);
    memcpy(&a, &in6.s6_addr, sizeof(in6.s6_addr));

    inet_pton(AF_INET6, "2000::0", &in6);
    memcpy(&b, &in6.s6_addr, sizeof(in6.s6_addr));

    if (AddressIPv6Ge(a, b) == 0)
        result = 1;

    return result;
}

int AddressTestIPv6SubOne01(void)
{
    int result = 0;

    uint32_t a[4], b[4];
    struct in6_addr in6;

    inet_pton(AF_INET6, "2000::1", &in6);
    memcpy(a, in6.s6_addr, sizeof(in6.s6_addr));

    a[0] = ntohl(a[0]);
    a[1] = ntohl(a[1]);
    a[2] = ntohl(a[2]);
    a[3] = ntohl(a[3]);

    AddressCutIPv6CopySubOne(a, b);

    inet_pton(AF_INET6, "2000::0", &in6);
    memcpy(a, in6.s6_addr, sizeof(in6.s6_addr));
    if (b[0] == a[0] && b[1] == a[1] &&
        b[2] == a[2] && b[3] == a[3]) {
        result = 1;
    }

    return result;
}

int AddressTestIPv6SubOne02(void)
{
    int result = 0;

    uint32_t a[4], b[4];
    struct in6_addr in6;

    inet_pton(AF_INET6, "2000::0", &in6);
    memcpy(a, in6.s6_addr, sizeof(in6.s6_addr));

    a[0] = ntohl(a[0]);
    a[1] = ntohl(a[1]);
    a[2] = ntohl(a[2]);
    a[3] = ntohl(a[3]);

    AddressCutIPv6CopySubOne(a, b);

    inet_pton(AF_INET6, "1FFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF", &in6);
    memcpy(a, in6.s6_addr, sizeof(in6.s6_addr));
    if (b[0] == a[0] && b[1] == a[1] &&
        b[2] == a[2] && b[3] == a[3]) {
        result = 1;
    }

    return result;
}

int AddressTestIPv6AddOne01(void)
{
    int result = 0;

    uint32_t a[4], b[4];
    struct in6_addr in6;

    inet_pton(AF_INET6, "2000::0", &in6);
    memcpy(a, in6.s6_addr, sizeof(in6.s6_addr));

    a[0] = ntohl(a[0]);
    a[1] = ntohl(a[1]);
    a[2] = ntohl(a[2]);
    a[3] = ntohl(a[3]);

    AddressCutIPv6CopyAddOne(a, b);

    inet_pton(AF_INET6, "2000::1", &in6);
    memcpy(a, in6.s6_addr, sizeof(in6.s6_addr));
    if (b[0] == a[0] && b[1] == a[1] &&
        b[2] == a[2] && b[3] == a[3]) {
        result = 1;
    }

    return result;
}

int AddressTestIPv6AddOne02(void)
{
    int result = 0;

    uint32_t a[4], b[4];
    struct in6_addr in6;

    inet_pton(AF_INET6, "1FFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF", &in6);
    memcpy(a, in6.s6_addr, sizeof(in6.s6_addr));

    a[0] = ntohl(a[0]);
    a[1] = ntohl(a[1]);
    a[2] = ntohl(a[2]);
    a[3] = ntohl(a[3]);

    AddressCutIPv6CopyAddOne(a, b);

    inet_pton(AF_INET6, "2000::0", &in6);
     memcpy(a, in6.s6_addr, sizeof(in6.s6_addr));
    if (b[0] == a[0] && b[1] == a[1] &&
        b[2] == a[2] && b[3] == a[3]) {
        result = 1;
    }

    return result;
}

static int AddressTestIPv6AddressCmp01(void)
{
    DetectAddress *a = DetectAddressInit();
    DetectAddress *b = DetectAddressInit();
    struct in6_addr in6;
    int result = 1;

    if (a == NULL || b == NULL)
        goto error;

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) == ADDRESS_EQ);

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::0", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) == ADDRESS_ES);

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::11", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) == ADDRESS_ES);

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::0", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) == ADDRESS_ES);

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::0", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::11", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) == ADDRESS_ES);

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::11", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) != ADDRESS_ES);

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::11", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) == ADDRESS_EB);

    if (inet_pton(AF_INET6, "2000::0", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) == ADDRESS_EB);

    if (inet_pton(AF_INET6, "2000::0", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::11", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) == ADDRESS_EB);

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::11", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::0", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) != ADDRESS_EB);

    if (inet_pton(AF_INET6, "2000::0", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) == ADDRESS_LE);

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::15", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) == ADDRESS_LE);

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) != ADDRESS_LE);

    if (inet_pton(AF_INET6, "2000::0", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) == ADDRESS_LE);

    if (inet_pton(AF_INET6, "2000::0", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) != ADDRESS_LE);

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::15", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) == ADDRESS_LT);

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::15", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    /* we could get a LE */
    result &= (DetectAddressCmpIPv6(a, b) != ADDRESS_LT);

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    /* we could get a LE */
    result &= (DetectAddressCmpIPv6(a, b) != ADDRESS_LT);

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::19", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) != ADDRESS_LT);

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) != ADDRESS_LT);

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) != ADDRESS_LT);

    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) == ADDRESS_GE);

    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::15", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) == ADDRESS_GE);

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::15", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) != ADDRESS_GE);

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::0", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) == ADDRESS_GE);

    if (inet_pton(AF_INET6, "2000::0", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::19", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) != ADDRESS_GE);

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::0", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) != ADDRESS_GE);

    if (inet_pton(AF_INET6, "2000::15", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) == ADDRESS_GT);

    if (inet_pton(AF_INET6, "2000::15", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::15", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) != ADDRESS_GT);

    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(b->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(b->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCmpIPv6(a, b) != ADDRESS_GT);

    if (a != NULL)
        DetectAddressFree(a);
    if (b != NULL)
        DetectAddressFree(b);
    return result;

 error:
    if (a != NULL)
        DetectAddressFree(a);
    if (b != NULL)
        DetectAddressFree(b);
    return 0;
}

static int AddressTestIPv6CutNot01(void)
{
    DetectAddress *a = NULL;
    DetectAddress *b = NULL;
    struct in6_addr in6;
    int result = 1;

    if ( (a = DetectAddressInit()) == NULL)
        goto error;

    if (inet_pton(AF_INET6, "::", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCutNotIPv6(a, &b) == -1);

    if (a != NULL)
        DetectAddressFree(a);
    if (b != NULL)
        DetectAddressFree(b);
    return result;

 error:
    if (a != NULL)
        DetectAddressFree(a);
    if (b != NULL)
        DetectAddressFree(b);
    return 0;
}

static int AddressTestIPv6CutNot02(void)
{
    DetectAddress *a = NULL;
    DetectAddress *b = NULL;
    DetectAddress *temp = NULL;
    struct in6_addr in6;
    int result = 1;

    if ( (a = DetectAddressInit()) == NULL)
        goto error;
    if ( (temp = DetectAddressInit()) == NULL)
        goto error;

    if (inet_pton(AF_INET6, "::", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::0", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCutNotIPv6(a, &b) == 0);

    result &= (b == NULL);

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(temp->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF", &in6) < 0)
        goto error;
    memcpy(temp->ip2, in6.s6_addr, sizeof(in6.s6_addr));

    result = (DetectAddressCmpIPv6(a, temp) == ADDRESS_EQ);

    if (a != NULL)
        DetectAddressFree(a);
    if (b != NULL)
        DetectAddressFree(b);
    if (temp != NULL)
        DetectAddressFree(temp);
    return result;

 error:
    if (a != NULL)
        DetectAddressFree(a);
    if (b != NULL)
        DetectAddressFree(b);
    if (temp != NULL)
        DetectAddressFree(temp);
    return 0;
}

static int AddressTestIPv6CutNot03(void)
{
    DetectAddress *a = NULL;
    DetectAddress *b = NULL;
    DetectAddress *temp = NULL;
    struct in6_addr in6;
    int result = 1;

    if ( (a = DetectAddressInit()) == NULL)
        goto error;
    if ( (temp = DetectAddressInit()) == NULL)
        goto error;

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCutNotIPv6(a, &b) == 0);

    result &= (b == NULL);

    if (inet_pton(AF_INET6, "::", &in6) < 0)
        goto error;
    memcpy(temp->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::0", &in6) < 0)
        goto error;
    memcpy(temp->ip2, in6.s6_addr, sizeof(in6.s6_addr));

    result = (DetectAddressCmpIPv6(a, temp) == ADDRESS_EQ);

    if (a != NULL)
        DetectAddressFree(a);
    if (b != NULL)
        DetectAddressFree(b);
    if (temp != NULL)
        DetectAddressFree(temp);
    return result;

 error:
    if (a != NULL)
        DetectAddressFree(a);
    if (b != NULL)
        DetectAddressFree(b);
    if (temp != NULL)
        DetectAddressFree(temp);
    return 0;
}

static int AddressTestIPv6CutNot04(void)
{
    DetectAddress *a = NULL;
    DetectAddress *b = NULL;
    DetectAddress *temp = NULL;
    struct in6_addr in6;
    int result = 1;

    if ( (a = DetectAddressInit()) == NULL)
        goto error;
    if ( (temp = DetectAddressInit()) == NULL)
        goto error;

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCutNotIPv6(a, &b) == 0);

    if (inet_pton(AF_INET6, "::", &in6) < 0)
        goto error;
    memcpy(temp->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::0", &in6) < 0)
        goto error;
    memcpy(temp->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result = (DetectAddressCmpIPv6(a, temp) == ADDRESS_EQ);

    result &= (b != NULL);
    if (result == 0)
        goto error;
    if (inet_pton(AF_INET6, "2000::2", &in6) < 0)
        goto error;
    memcpy(temp->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF", &in6) < 0)
        goto error;
    memcpy(temp->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result = (DetectAddressCmpIPv6(b, temp) == ADDRESS_EQ);

    if (a != NULL)
        DetectAddressFree(a);
    if (b != NULL)
        DetectAddressFree(b);
    if (temp != NULL)
        DetectAddressFree(temp);
    return result;

 error:
    if (a != NULL)
        DetectAddressFree(a);
    if (b != NULL)
        DetectAddressFree(b);
    if (temp != NULL)
        DetectAddressFree(temp);
    return 0;
}

static int AddressTestIPv6CutNot05(void)
{
    DetectAddress *a = NULL;
    DetectAddress *b = NULL;
    DetectAddress *temp = NULL;
    struct in6_addr in6;
    int result = 1;

    if ( (a = DetectAddressInit()) == NULL)
        goto error;
    if ( (temp = DetectAddressInit()) == NULL)
        goto error;

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(a->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(a->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result &= (DetectAddressCutNotIPv6(a, &b) == 0);

    if (inet_pton(AF_INET6, "::", &in6) < 0)
        goto error;
    memcpy(temp->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::0", &in6) < 0)
        goto error;
    memcpy(temp->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result = (DetectAddressCmpIPv6(a, temp) == ADDRESS_EQ);

    result &= (b != NULL);
    if (result == 0)
        goto error;
    if (inet_pton(AF_INET6, "2000::21", &in6) < 0)
        goto error;
    memcpy(temp->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF:FFFF", &in6) < 0)
        goto error;
    memcpy(temp->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result = (DetectAddressCmpIPv6(b, temp) == ADDRESS_EQ);

    if (a != NULL)
        DetectAddressFree(a);
    if (b != NULL)
        DetectAddressFree(b);
    if (temp != NULL)
        DetectAddressFree(temp);
    return result;

 error:
    if (a != NULL)
        DetectAddressFree(a);
    if (b != NULL)
        DetectAddressFree(b);
    if (temp != NULL)
        DetectAddressFree(temp);
    return 0;
}

static int AddressTestIPv6Join01(void)
{
    DetectAddress *source = DetectAddressInit();
    DetectAddress *target = DetectAddressInit();
    DetectAddress *temp = DetectAddressInit();
    struct in6_addr in6;
    int result = 1;

    if (source == NULL || target == NULL || temp == NULL)
        goto error;

    /* case 1 */
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(target->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(target->ip2, in6.s6_addr, sizeof(in6.s6_addr));

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(source->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000:20", &in6) < 0)
        goto error;
    memcpy(source->ip2, in6.s6_addr, sizeof(in6.s6_addr));

    result &= (DetectAddressJoinIPv6(NULL, target, source) == 0);
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(temp->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(temp->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result = (DetectAddressCmpIPv6(target, temp) == ADDRESS_EQ);

    /* case 2 */
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(target->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(target->ip2, in6.s6_addr, sizeof(in6.s6_addr));

    if (inet_pton(AF_INET6, "2000::2", &in6) < 0)
        goto error;
    memcpy(source->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::19", &in6) < 0)
        goto error;
    memcpy(source->ip2, in6.s6_addr, sizeof(in6.s6_addr));

    result &= (DetectAddressJoinIPv6(NULL, target, source) == 0);
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(temp->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(temp->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result = (DetectAddressCmpIPv6(target, temp) == ADDRESS_EQ);

    /* case 3 */
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(target->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::15", &in6) < 0)
        goto error;
    memcpy(target->ip2, in6.s6_addr, sizeof(in6.s6_addr));

    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(source->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(source->ip2, in6.s6_addr, sizeof(in6.s6_addr));

    result &= (DetectAddressJoinIPv6(NULL, target, source) == 0);
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(temp->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(temp->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result = (DetectAddressCmpIPv6(target, temp) == ADDRESS_EQ);

    /* case 4 */
    if (inet_pton(AF_INET6, "2000::10", &in6) < 0)
        goto error;
    memcpy(target->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(target->ip2, in6.s6_addr, sizeof(in6.s6_addr));

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(source->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(source->ip2, in6.s6_addr, sizeof(in6.s6_addr));

    result &= (DetectAddressJoinIPv6(NULL, target, source) == 0);
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(temp->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(temp->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result = (DetectAddressCmpIPv6(target, temp) == ADDRESS_EQ);

    /* case 5 */
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(target->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(target->ip2, in6.s6_addr, sizeof(in6.s6_addr));

    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(source->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(source->ip2, in6.s6_addr, sizeof(in6.s6_addr));

    result &= (DetectAddressJoinIPv6(NULL, target, source) == 0);
    if (inet_pton(AF_INET6, "2000::1", &in6) < 0)
        goto error;
    memcpy(temp->ip, in6.s6_addr, sizeof(in6.s6_addr));
    if (inet_pton(AF_INET6, "2000::20", &in6) < 0)
        goto error;
    memcpy(temp->ip2, in6.s6_addr, sizeof(in6.s6_addr));
    result = (DetectAddressCmpIPv6(target, temp) == ADDRESS_EQ);

    if (source != NULL)
        DetectAddressFree(source);
    if (target != NULL)
        DetectAddressFree(target);
    if (temp != NULL)
        DetectAddressFree(temp);
    return result;

 error:
    if (source != NULL)
        DetectAddressFree(source);
    if (target != NULL)
        DetectAddressFree(target);
    if (temp != NULL)
        DetectAddressFree(temp);
    return 0;
}

#endif /* UNITTESTS */

void DetectAddressIPv6Tests(void)
{

#ifdef UNITTESTS
    UtRegisterTest("AddressTestIPv6Gt01", AddressTestIPv6Gt01, 1);
    UtRegisterTest("AddressTestIPv6Gt02", AddressTestIPv6Gt02, 1);
    UtRegisterTest("AddressTestIPv6Gt03", AddressTestIPv6Gt03, 1);
    UtRegisterTest("AddressTestIPv6Gt04", AddressTestIPv6Gt04, 1);

    UtRegisterTest("AddressTestIPv6Lt01", AddressTestIPv6Lt01, 1);
    UtRegisterTest("AddressTestIPv6Lt02", AddressTestIPv6Lt02, 1);
    UtRegisterTest("AddressTestIPv6Lt03", AddressTestIPv6Lt03, 1);
    UtRegisterTest("AddressTestIPv6Lt04", AddressTestIPv6Lt04, 1);

    UtRegisterTest("AddressTestIPv6Eq01", AddressTestIPv6Eq01, 1);
    UtRegisterTest("AddressTestIPv6Eq02", AddressTestIPv6Eq02, 1);
    UtRegisterTest("AddressTestIPv6Eq03", AddressTestIPv6Eq03, 1);
    UtRegisterTest("AddressTestIPv6Eq04", AddressTestIPv6Eq04, 1);

    UtRegisterTest("AddressTestIPv6Le01", AddressTestIPv6Le01, 1);
    UtRegisterTest("AddressTestIPv6Le02", AddressTestIPv6Le02, 1);
    UtRegisterTest("AddressTestIPv6Le03", AddressTestIPv6Le03, 1);
    UtRegisterTest("AddressTestIPv6Le04", AddressTestIPv6Le04, 1);
    UtRegisterTest("AddressTestIPv6Le05", AddressTestIPv6Le05, 1);

    UtRegisterTest("AddressTestIPv6Ge01", AddressTestIPv6Ge01, 1);
    UtRegisterTest("AddressTestIPv6Ge02", AddressTestIPv6Ge02, 1);
    UtRegisterTest("AddressTestIPv6Ge03", AddressTestIPv6Ge03, 1);
    UtRegisterTest("AddressTestIPv6Ge04", AddressTestIPv6Ge04, 1);
    UtRegisterTest("AddressTestIPv6Ge05", AddressTestIPv6Ge05, 1);

    UtRegisterTest("AddressTestIPv6SubOne01", AddressTestIPv6SubOne01, 1);
    UtRegisterTest("AddressTestIPv6SubOne02", AddressTestIPv6SubOne02, 1);

    UtRegisterTest("AddressTestIPv6AddOne01", AddressTestIPv6AddOne01, 1);
    UtRegisterTest("AddressTestIPv6AddOne02", AddressTestIPv6AddOne02, 1);

    UtRegisterTest("AddressTestIPv6AddressCmp01",
                   AddressTestIPv6AddressCmp01, 1);

    UtRegisterTest("AddressTestIPv6CutNot01", AddressTestIPv6CutNot01, 1);
    UtRegisterTest("AddressTestIPv6CutNot02", AddressTestIPv6CutNot02, 1);
    UtRegisterTest("AddressTestIPv6CutNot03", AddressTestIPv6CutNot03, 1);
    UtRegisterTest("AddressTestIPv6CutNot04", AddressTestIPv6CutNot04, 1);
    UtRegisterTest("AddressTestIPv6CutNot05", AddressTestIPv6CutNot05, 1);

    UtRegisterTest("AddressTestIPv6Join01", AddressTestIPv6Join01, 1);
#endif /* UNITTESTS */

    return;
}
