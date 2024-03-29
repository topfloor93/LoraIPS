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
 * \author Pablo Rincon Crespo <pablo.rincon.crespo@gmail.com>
 *
 * Signatures that only inspect IP addresses are processed here
 * We use radix trees for src dst ipv4 and ipv6 adresses
 * This radix trees hold information for subnets and hosts in a
 * hierarchical distribution
 */

#include "suricata-common.h"
#include "debug.h"
#include "detect.h"
#include "decode.h"
#include "flow.h"

#include "detect-parse.h"
#include "detect-engine.h"

#include "detect-engine-siggroup.h"
#include "detect-engine-address.h"
#include "detect-engine-proto.h"
#include "detect-engine-port.h"
#include "detect-engine-mpm.h"

#include "detect-engine-threshold.h"
#include "detect-engine-iponly.h"
#include "detect-threshold.h"
#include "util-classification-config.h"
#include "util-rule-vars.h"

#include "util-debug.h"
#include "util-unittest.h"
#include "util-unittest-helper.h"

#ifdef OS_WIN32
#include <winsock.h>
#else
#include <netinet/in.h>
#endif /* OS_WIN32 */

/**
 * \brief This function creates a new IPOnlyCIDRItem
 *
 * \retval IPOnlyCIDRItem address of the new instance
 */
IPOnlyCIDRItem *IPOnlyCIDRItemNew() {
    SCEnter();
    IPOnlyCIDRItem *item = NULL;

    item = SCMalloc(sizeof(IPOnlyCIDRItem));
    if (item == NULL)
        SCReturnPtr(NULL, "NULL");
    memset(item, 0, sizeof(IPOnlyCIDRItem));

    SCReturnPtr(item, "IPOnlyCIDRItem");
}

/**
 * \brief This function insert a IPOnlyCIDRItem
 *        to a list of IPOnlyCIDRItems sorted by netmask
 *        ascending
 * \param head Pointer to the head of IPOnlyCIDRItems list
 * \param item Pointer to the item to insert in the list
 *
 * \retval IPOnlyCIDRItem address of the new head if apply
 */
IPOnlyCIDRItem *IPOnlyCIDRItemInsertReal(IPOnlyCIDRItem *head,
                                         IPOnlyCIDRItem *item)
{
    IPOnlyCIDRItem *it, *prev = NULL;

    if (item == NULL)
        return head;

    /* Compare with the head */
    if (item->netmask <= head->netmask) {
        item->next = head;
        return item;
    }

    for (prev = it = head;
         it != NULL && it->netmask < item->netmask;
         it = it->next)
        prev = it;

    if (it == NULL) {
        prev->next = item;
    } else {
        item->next = it;
        prev->next = item;
    }

    return head;
}

/**
 * \brief This function insert a IPOnlyCIDRItem list
 *        to a list of IPOnlyCIDRItems sorted by netmask
 *        ascending
 * \param head Pointer to the head of IPOnlyCIDRItems list
 * \param item Pointer to the list of items to insert in the list
 *
 * \retval IPOnlyCIDRItem address of the new head if apply
 */
IPOnlyCIDRItem *IPOnlyCIDRItemInsert(IPOnlyCIDRItem *head,
                                     IPOnlyCIDRItem *item)
{
    IPOnlyCIDRItem *it, *prev = NULL;

    /* The first element */
    if (head == NULL) {
        SCLogDebug("Head is NULL to insert item (%p)",item);
        return item;
    }

    if (item == NULL) {
        SCLogDebug("Item is NULL");
        return head;
    }

    SCLogDebug("Inserting item(%p)->netmast %u head %p", item, item->netmask, head);

    prev = item;
    while (prev != NULL) {
        it = prev->next;

        /* Separate from the item list */
        prev->next = NULL;

        head = IPOnlyCIDRItemInsertReal(head, prev);
        prev = it;
    }

    return head;
}

/**
 * \brief This function free a IPOnlyCIDRItem list
 * \param tmphead Pointer to the list
 */
void IPOnlyCIDRListFree(IPOnlyCIDRItem *tmphead) {
    SCEnter();
    uint32_t i = 0;

    IPOnlyCIDRItem *it, *next = NULL;

    if (tmphead == NULL) {
        SCLogDebug("temphead is NULL");
        return;
    }

    it = tmphead;
    next = it->next;

    while (it != NULL) {
        i++;
        SCFree(it);
        SCLogDebug("Item(%p) %"PRIu32" removed\n", it, i);
        it = next;

        if (next != NULL)
            next = next->next;
    }
    SCReturn;
}

/**
 * \brief This function update a list of IPOnlyCIDRItems
 *        setting the signature internal id (signum) to "i"
 *
 * \param tmphead Pointer to the list
 * \param i number of signature internal id
 */
void IPOnlyCIDRListSetSigNum(IPOnlyCIDRItem *tmphead, SigIntId i) {
    while (tmphead != NULL) {
        tmphead->signum = i;
        tmphead = tmphead->next;
    }
}

/**
 * \brief This function print a IPOnlyCIDRItem list
 * \param tmphead Pointer to the head of IPOnlyCIDRItems list
 */
void IPOnlyCIDRListPrint(IPOnlyCIDRItem *tmphead) {
    uint32_t i = 0;

    while (tmphead != NULL) {
        i++;
        SCLogDebug("Item %"PRIu32" has netmask %"PRIu16" negated:"
                   " %s; IP: %s; signum: %"PRIu16, i, tmphead->netmask,
                   (tmphead->negated) ? "yes":"no",
                   inet_ntoa(*(struct in_addr*)&tmphead->ip[0]),
                   tmphead->signum);
        tmphead = tmphead->next;
    }
}

/**
 * \brief This function print a SigNumArray, it's used with the
 *        radix tree print function to help debugging
 * \param tmp Pointer to the head of SigNumArray
 */
void SigNumArrayPrint(void *tmp) {
    SigNumArray *sna = (SigNumArray *)tmp;
    uint32_t u;

    for (u = 0; u < sna->size; u++) {
        uint8_t bitarray = sna->array[u];
        uint8_t i = 0;

        for (; i < 8; i++) {
            if (bitarray & 0x01)
                printf(", %"PRIu16"", u * 8 + i);
            else
                printf(", ");

            bitarray = bitarray >> 1;
        }
    }
}

/**
 * \brief This function creates a new SigNumArray with the
 *        size fixed to the io_ctx->max_idx
 * \param de_ctx Pointer to the current detection context
 * \param io_ctx Pointer to the current ip only context
 *
 * \retval SigNumArray address of the new instance
 */
SigNumArray *SigNumArrayNew(DetectEngineCtx *de_ctx,
                            DetectEngineIPOnlyCtx *io_ctx)
{
    SigNumArray *new = SCMalloc(sizeof(SigNumArray));

    if (new == NULL){
        SCLogError(SC_ERR_FATAL, "Fatal error encountered in SigNumArrayNew. Exiting...");
        exit(EXIT_FAILURE);
    }
    memset(new, 0, sizeof(SigNumArray));

    new->array = SCMalloc(io_ctx->max_idx / 8 + 1);
    if (new->array == NULL) {
       SCLogError(SC_ERR_FATAL, "Fatal error encountered in SigNumArrayNew. Exiting...");
       exit(EXIT_FAILURE);
    }

    memset(new->array, 0, io_ctx->max_idx / 8 + 1);
    new->size = io_ctx->max_idx / 8 + 1;

    SCLogDebug("max idx= %u", io_ctx->max_idx);

    return new;
}

/**
 * \brief This function creates a new SigNumArray with the
 *        same data as the argument
 *
 * \param orig Pointer to the original SigNumArray to copy
 *
 * \retval SigNumArray address of the new instance
 */
SigNumArray *SigNumArrayCopy(SigNumArray *orig) {
    SigNumArray *new = SCMalloc(sizeof(SigNumArray));

    if (new == NULL) {
        SCLogError(SC_ERR_FATAL, "Fatal error encountered in SigNumArrayCopy. Exiting...");
        exit(EXIT_FAILURE);
    }

    memset(new, 0, sizeof(SigNumArray));
    new->size = orig->size;

    new->array = SCMalloc(orig->size);
    if (new->array == NULL) {
        SCLogError(SC_ERR_FATAL, "Fatal error encountered in SigNumArrayCopy. Exiting...");
        exit(EXIT_FAILURE);
    }

    memcpy(new->array, orig->array, orig->size);
    return new;
}

/**
 * \brief This function free() a SigNumArray
 * \param orig Pointer to the original SigNumArray to copy
 */
void SigNumArrayFree(void *tmp) {
    SigNumArray *sna = (SigNumArray *)tmp;

    if (sna == NULL)
        return;

    if (sna->array != NULL)
        SCFree(sna->array);

    SCFree(sna);
}

/**
 * \brief This function parses and return a list of IPOnlyCIDRItem
 *
 * \param s Pointer to the string of the addresses
 *          (in the format of signatures)
 * \param negate flag to indicate if all this string is negated or not
 *
 * \retval 0 if success
 * \retval -1 if fails
 */
IPOnlyCIDRItem *IPOnlyCIDRListParse2(char *s, int negate)
{
    size_t x = 0;
    size_t u = 0;
    int o_set = 0, n_set = 0, d_set = 0;
    int depth = 0;
    size_t size = strlen(s);
    char address[1024] = "";
    char *rule_var_address = NULL;
    char *temp_rule_var_address = NULL;
    IPOnlyCIDRItem *head;
    IPOnlyCIDRItem *subhead;
    head = subhead = NULL;

    SCLogDebug("s %s negate %s", s, negate ? "true" : "false");

    for (u = 0, x = 0; u < size && x < sizeof(address); u++) {
        address[x] = s[u];
        x++;

        if (!o_set && s[u] == '!') {
            n_set = 1;
            x--;
        } else if (s[u] == '[') {
            if (!o_set) {
                o_set = 1;
                x = 0;
            }
            depth++;
        } else if (s[u] == ']') {
            if (depth == 1) {
                address[x - 1] = '\0';
                x = 0;

                if ( (subhead = IPOnlyCIDRListParse2(address,
                                                (negate + n_set) % 2)) == NULL)
                    goto error;

                head = IPOnlyCIDRItemInsert(head, subhead);
                n_set = 0;
            }
            depth--;
        } else if (depth == 0 && s[u] == ',') {
            if (o_set == 1) {
                o_set = 0;
            } else if (d_set == 1) {
                address[x - 1] = '\0';
                x = 0;
                rule_var_address = SCRuleVarsGetConfVar(address,
                                                  SC_RULE_VARS_ADDRESS_GROUPS);
                if (rule_var_address == NULL)
                    goto error;

                temp_rule_var_address = rule_var_address;
                if ((negate + n_set) % 2) {
                    temp_rule_var_address = SCMalloc(strlen(rule_var_address) + 3);

                    if (temp_rule_var_address == NULL) {
                        goto error;
                    }

                    snprintf(temp_rule_var_address, strlen(rule_var_address) + 3,
                             "[%s]", rule_var_address);
                }

                subhead = IPOnlyCIDRListParse2(temp_rule_var_address,
                                               (negate + n_set) % 2);
                head = IPOnlyCIDRItemInsert(head, subhead);

                d_set = 0;
                n_set = 0;

                if (temp_rule_var_address != rule_var_address)
                    SCFree(temp_rule_var_address);

            } else {
                address[x - 1] = '\0';

                subhead = IPOnlyCIDRItemNew();
                if (subhead == NULL)
                    goto error;

                if (!((negate + n_set) % 2))
                    subhead->negated = 0;
                else
                    subhead->negated = 1;

                if (IPOnlyCIDRItemSetup(subhead, address) < 0) {
                    IPOnlyCIDRListFree(subhead);
                    subhead = NULL;
                    goto error;
                }
                head = IPOnlyCIDRItemInsert(head, subhead);

                n_set = 0;
            }
            x = 0;
        } else if (depth == 0 && s[u] == '$') {
            d_set = 1;
        } else if (depth == 0 && u == size - 1) {
            if (x == 1024) {
                address[x - 1] = '\0';
            } else {
                address[x] = '\0';
            }
            x = 0;

            if (d_set == 1) {
                rule_var_address = SCRuleVarsGetConfVar(address,
                                                    SC_RULE_VARS_ADDRESS_GROUPS);
                if (rule_var_address == NULL)
                    goto error;

                temp_rule_var_address = rule_var_address;
                if ((negate + n_set) % 2) {
                    temp_rule_var_address = SCMalloc(strlen(rule_var_address) + 3);
                    if (temp_rule_var_address == NULL) {
                        goto error;
                    }
                    snprintf(temp_rule_var_address, strlen(rule_var_address) + 3,
                            "[%s]", rule_var_address);
                }
                subhead = IPOnlyCIDRListParse2(temp_rule_var_address,
                                               (negate + n_set) % 2);
                head = IPOnlyCIDRItemInsert(head, subhead);

                d_set = 0;

                if (temp_rule_var_address != rule_var_address)
                    SCFree(temp_rule_var_address);
            } else {
                subhead = IPOnlyCIDRItemNew();
                if (subhead == NULL)
                    goto error;

                if (!((negate + n_set) % 2))
                    subhead->negated = 0;
                else
                    subhead->negated = 1;

                if (IPOnlyCIDRItemSetup(subhead, address) < 0) {
                    IPOnlyCIDRListFree(subhead);
                    subhead = NULL;
                    goto error;
                }
                head = IPOnlyCIDRItemInsert(head, subhead);
            }
            n_set = 0;
        }
    }

    return head;

error:
    SCLogError(SC_ERR_ADDRESS_ENGINE_GENERIC,"Error parsing addresses");
    return head;
}


/**
 * \brief Parses an address group sent as a character string and updates the
 *        IPOnlyCIDRItem list
 *
 * \param gh  Pointer to the IPOnlyCIDRItem list
 * \param str Pointer to the character string containing the address group
 *            that has to be parsed.
 *
 * \retval  0 On success.
 * \retval -1 On failure.
 */
int IPOnlyCIDRListParse(IPOnlyCIDRItem **gh, char *str)
{
    SCLogDebug("gh %p, str %s", gh, str);

    *gh = IPOnlyCIDRListParse2(str, 0);
    if (gh == NULL) {
        SCLogDebug("DetectAddressParse2 returned null");
        goto error;
    }

    return 0;

error:
    return -1;
}

/**
 * \brief Parses an address group sent as a character string and updates the
 *        IPOnlyCIDRItem lists src and dst of the Signature *s
 *
 * \param s Pointer to the signature structure
 * \param addrstr Pointer to the character string containing the address group
 *            that has to be parsed.
 * \param flag to indicate if we are parsing the src string or the dst string
 *
 * \retval  0 On success.
 * \retval -1 On failure.
 */
int IPOnlySigParseAddress(Signature *s, const char *addrstr, char flag)
{
    SCLogDebug("Address Group \"%s\" to be parsed now", addrstr);
    IPOnlyCIDRItem *tmp = NULL;

    /* pass on to the address(list) parser */
    if (flag == 0) {
        if (strcasecmp(addrstr, "any") == 0) {
            s->flags |= SIG_FLAG_SRC_ANY;

            if (IPOnlyCIDRListParse(&s->CidrSrc, (char *)"0.0.0.0/0") < 0)
                goto error;

            if (IPOnlyCIDRListParse(&tmp, (char *)"::/0") < 0)
                goto error;

            s->CidrSrc = IPOnlyCIDRItemInsert(s->CidrSrc, tmp);

        } else if (IPOnlyCIDRListParse(&s->CidrSrc, (char *)addrstr) < 0) {
            goto error;
        }

        /* IPOnlyCIDRListPrint(s->CidrSrc); */
    } else {
        if (strcasecmp(addrstr, "any") == 0) {
            s->flags |= SIG_FLAG_DST_ANY;

            if (IPOnlyCIDRListParse(&tmp, (char *)"0.0.0.0/0") < 0)
                goto error;

            if (IPOnlyCIDRListParse(&s->CidrDst, (char *)"::/0") < 0)
                goto error;

            s->CidrDst = IPOnlyCIDRItemInsert(s->CidrDst, tmp);

        } else if (IPOnlyCIDRListParse(&s->CidrDst, (char *)addrstr) < 0) {
            goto error;
        }

        /* IPOnlyCIDRListPrint(s->CidrDst); */
    }

    return 0;

error:
    return -1;
}

/**
 * \internal
 * \brief Parses an ipv4/ipv6 address string and updates the result into the
 *        IPOnlyCIDRItem instance sent as the argument.
 *
 * \param dd  Pointer to the IPOnlyCIDRItem instance which should be updated with
 *            the address (in cidr) details from the parsed ip string.
 * \param str Pointer to address string that has to be parsed.
 *
 * \retval  0 On successfully parsing the address string.
 * \retval -1 On failure.
 */
int IPOnlyCIDRItemParseSingle(IPOnlyCIDRItem *dd, char *str)
{
    char *ipdup = SCStrdup(str);
    char *ip = NULL;
    char *ip2 = NULL;
    char *mask = NULL;
    int r = 0;

    SCLogDebug("str %s", str);

    /* first handle 'any' */
    if (strcasecmp(str, "any") == 0) {
        /* if any, insert 0.0.0.0/0 and ::/0 as well */
        SCLogDebug("adding 0.0.0.0/0 and ::/0 as we\'re handling \'any\'");

        IPOnlyCIDRItemParseSingle(dd, "0.0.0.0/0");
        BUG_ON(dd->family == 0);

        dd->next = IPOnlyCIDRItemNew();
        if (dd->next == NULL)
            goto error;

        IPOnlyCIDRItemParseSingle(dd->next, "::/0");
        BUG_ON(dd->family == 0);

        SCFree(ipdup);

        SCLogDebug("address is \'any\'");
        return 0;
    }

    /* we dup so we can put a nul-termination in it later */
    ip = ipdup;
    if (ip == NULL) {
        goto error;
    }

    /* handle the negation case */
    if (ip[0] == '!') {
        dd->negated = (dd->negated)? 0 : 1;
        ip++;
    }

    /* see if the address is an ipv4 or ipv6 address */
    if ((strchr(str, ':')) == NULL) {
        /* IPv4 Address */
        struct in_addr in;

        dd->family = AF_INET;

        if ((mask = strchr(ip, '/')) != NULL) {
            /* 1.2.3.4/xxx format (either dotted or cidr notation */
            ip[mask - ip] = '\0';
            mask++;
            uint32_t netmask = 0;
            size_t u = 0;

            if ((strchr (mask, '.')) == NULL) {
                /* 1.2.3.4/24 format */

                for (u = 0; u < strlen(mask); u++) {
                    if(!isdigit(mask[u]))
                        goto error;
                }

                int cidr = atoi(mask);
                if (cidr < 0 || cidr > 32)
                    goto error;

                dd->netmask = cidr;
            } else {
                /* 1.2.3.4/255.255.255.0 format */
                r = inet_pton(AF_INET, mask, &in);
                if (r <= 0)
                    goto error;

                netmask = in.s_addr;

                /* Extract cidr netmask */
                while ((0x01 & netmask) == 0) {
                    dd->netmask++;
                    netmask = netmask >> 1;
                }
                dd->netmask = 32 - dd->netmask;
            }

            r = inet_pton(AF_INET, ip, &in);
            if (r <= 0)
                goto error;

            dd->ip[0] = in.s_addr;

        } else if ((ip2 = strchr(ip, '-')) != NULL)  {
            /* 1.2.3.4-1.2.3.6 range format */
            ip[ip2 - ip] = '\0';
            ip2++;

            uint32_t tmp_ip[4];
            uint32_t tmp_ip2[4];
            uint32_t first, last;

            r = inet_pton(AF_INET, ip, &in);
            if (r <= 0)
                goto error;
            tmp_ip[0] = in.s_addr;

            r = inet_pton(AF_INET, ip2, &in);
            if (r <= 0)
                goto error;
            tmp_ip2[0] = in.s_addr;

            /* a > b is illegal, a = b is ok */
            if (ntohl(tmp_ip[0]) > ntohl(tmp_ip2[0]))
                goto error;

            first = ntohl(tmp_ip[0]);
            last = ntohl(tmp_ip2[0]);

            dd->netmask = 32;
            dd->ip[0] =htonl(first);

            if (first < last) {
                for (first++; first <= last; first++) {
                    IPOnlyCIDRItem *new = IPOnlyCIDRItemNew();
                    if (new == NULL)
                        goto error;
                    dd->next = new;
                    new->negated = dd->negated;
                    new->family= dd->family;
                    new->netmask = dd->netmask;
                    new->ip[0] = htonl(first);
                    dd = dd->next;
                }
            }

        } else {
            /* 1.2.3.4 format */
            r = inet_pton(AF_INET, ip, &in);
            if (r <= 0)
                goto error;

            /* single host */
            dd->ip[0] = in.s_addr;
            dd->netmask = 32;
        }
    } else {
        /* IPv6 Address */
        struct in6_addr in6;
        uint32_t ip6addr[4];

        dd->family = AF_INET6;

        if ((mask = strchr(ip, '/')) != NULL)  {
            mask[0] = '\0';
            mask++;

            r = inet_pton(AF_INET6, ip, &in6);
            if (r <= 0)
                goto error;

            /* Format is cidr val */
            dd->netmask = atoi(mask);

            memcpy(dd->ip, &in6.s6_addr, sizeof(ip6addr));
        } else {
            r = inet_pton(AF_INET6, ip, &in6);
            if (r <= 0)
                goto error;

            memcpy(dd->ip, &in6.s6_addr, sizeof(dd->ip));
            dd->netmask = 128;
        }

    }

    SCFree(ipdup);

    BUG_ON(dd->family == 0);
    return 0;

error:
    if (ipdup)
        SCFree(ipdup);
    return -1;
}

/**
 * \brief Setup a single address string, parse it and add the resulting
 *        Address items in cidr format to the list of gh
 *
 * \param gh Pointer to the IPOnlyCIDRItem list Head to which the
 *           resulting Address-Range(s) from the parsed ip string has to
 *           be added.
 * \param s  Pointer to the ip address string to be parsed.
 *
 * \retval  0 On success.
 * \retval -1 On failure.
 */
int IPOnlyCIDRItemSetup(IPOnlyCIDRItem *gh, char *s) {
    SCLogDebug("gh %p, s %s", gh, s);

    /* parse the address */
    if (IPOnlyCIDRItemParseSingle(gh, s) == -1) {
        SCLogError(SC_ERR_ADDRESS_ENGINE_GENERIC,
                   "DetectAddressParse error \"%s\"", s);
        goto error;
    }

    return 0;

error:
    SCLogError(SC_ERR_ADDRESS_ENGINE_GENERIC, "IPOnlyCIDRItemSetup error");
    /* XXX cleanup */
    return -1;
}

/**
 * \brief Setup the IP Only detection engine context
 *
 * \param de_ctx Pointer to the current detection engine
 * \param io_ctx Pointer to the current ip only detection engine
 */
void IPOnlyInit(DetectEngineCtx *de_ctx, DetectEngineIPOnlyCtx *io_ctx) {
    io_ctx->sig_init_size = DetectEngineGetMaxSigId(de_ctx) / 8 + 1;

    if ( (io_ctx->sig_init_array = SCMalloc(io_ctx->sig_init_size)) == NULL) {
        SCLogError(SC_ERR_FATAL, "Fatal error encountered in IPOnlyInit. Exiting...");
        exit(EXIT_FAILURE);
    }

    memset(io_ctx->sig_init_array, 0, io_ctx->sig_init_size);

    io_ctx->tree_ipv4src = SCRadixCreateRadixTree(SigNumArrayFree,
                                                  SigNumArrayPrint);
    io_ctx->tree_ipv4dst = SCRadixCreateRadixTree(SigNumArrayFree,
                                                  SigNumArrayPrint);
    io_ctx->tree_ipv6src = SCRadixCreateRadixTree(SigNumArrayFree,
                                                  SigNumArrayPrint);
    io_ctx->tree_ipv6dst = SCRadixCreateRadixTree(SigNumArrayFree,
                                                  SigNumArrayPrint);
}

/**
 * \brief Setup the IP Only thread detection engine context
 *
 * \param de_ctx Pointer to the current detection engine
 * \param io_ctx Pointer to the current ip only thread detection engine
 */
void DetectEngineIPOnlyThreadInit(DetectEngineCtx *de_ctx,
                                  DetectEngineIPOnlyThreadCtx *io_tctx) {
    /* initialize the signature bitarray */
    io_tctx->sig_match_size = de_ctx->io_ctx.max_idx / 8 + 1;
    io_tctx->sig_match_array = SCMalloc(io_tctx->sig_match_size);
    if (io_tctx->sig_match_array == NULL) {
        SCLogError(SC_ERR_FATAL, "Fatal error encountered in DetectEngineIPOnlyThreadInit. Exiting...");
        exit(EXIT_FAILURE);
    }

    memset(io_tctx->sig_match_array, 0, io_tctx->sig_match_size);
}

/**
 * \brief Print stats of the IP Only engine
 *
 * \param de_ctx Pointer to the current detection engine
 * \param io_ctx Pointer to the current ip only detection engine
 */
void IPOnlyPrint(DetectEngineCtx *de_ctx, DetectEngineIPOnlyCtx *io_ctx) {
    /* XXX: how are we going to print the stats now? */
}

/**
 * \brief Deinitialize the IP Only detection engine context
 *
 * \param de_ctx Pointer to the current detection engine
 * \param io_ctx Pointer to the current ip only detection engine
 */
void IPOnlyDeinit(DetectEngineCtx *de_ctx, DetectEngineIPOnlyCtx *io_ctx) {

    if (io_ctx == NULL)
        return;

    if (io_ctx->tree_ipv4src != NULL)
        SCRadixReleaseRadixTree(io_ctx->tree_ipv4src);

    if (io_ctx->tree_ipv4dst != NULL)
        SCRadixReleaseRadixTree(io_ctx->tree_ipv4dst);

    if (io_ctx->tree_ipv6src != NULL)
        SCRadixReleaseRadixTree(io_ctx->tree_ipv6src);

    if (io_ctx->tree_ipv6dst != NULL)
        SCRadixReleaseRadixTree(io_ctx->tree_ipv6dst);

    if (io_ctx->sig_init_array)
        SCFree(io_ctx->sig_init_array);

    io_ctx->sig_init_array = NULL;
}

/**
 * \brief Deinitialize the IP Only thread detection engine context
 *
 * \param de_ctx Pointer to the current detection engine
 * \param io_ctx Pointer to the current ip only detection engine
 */
void DetectEngineIPOnlyThreadDeinit(DetectEngineIPOnlyThreadCtx *io_tctx) {
    SCFree(io_tctx->sig_match_array);
}

/**
 * \brief Match a packet against the IP Only detection engine contexts
 *
 * \param de_ctx Pointer to the current detection engine
 * \param io_ctx Pointer to the current ip only detection engine
 * \param io_ctx Pointer to the current ip only thread detection engine
 * \param p Pointer to the Packet to match against
 */
void IPOnlyMatchPacket(DetectEngineCtx *de_ctx,
                       DetectEngineThreadCtx *det_ctx,
                       DetectEngineIPOnlyCtx *io_ctx,
                       DetectEngineIPOnlyThreadCtx *io_tctx, Packet *p)
{
    SCRadixNode *srcnode = NULL, *dstnode = NULL;
    SigNumArray *src = NULL;
    SigNumArray *dst = NULL;

    if (p->src.family == AF_INET) {
        srcnode = SCRadixFindKeyIPV4BestMatch((uint8_t *)&GET_IPV4_SRC_ADDR_U32(p),
                                              io_ctx->tree_ipv4src);
    } else if (p->src.family == AF_INET6) {
        srcnode = SCRadixFindKeyIPV6BestMatch((uint8_t *)&GET_IPV6_SRC_ADDR(p),
                                              io_ctx->tree_ipv6src);
    }

    if (p->dst.family == AF_INET) {
        dstnode = SCRadixFindKeyIPV4BestMatch((uint8_t *)&GET_IPV4_DST_ADDR_U32(p),
                                              io_ctx->tree_ipv4dst);
    } else if (p->dst.family == AF_INET6) {
        dstnode = SCRadixFindKeyIPV6BestMatch((uint8_t *)&GET_IPV6_DST_ADDR(p),
                                              io_ctx->tree_ipv6dst);
    }


    /* The radix trees are printed without our logging format
       comment this out if you need to debug
    printf("Src: \n");
    SCRadixPrintNodeInfo(srcnode, 4, SigNumArrayPrint);
    printf("Dst: \n");
    SCRadixPrintNodeInfo(dstnode, 4, SigNumArrayPrint);
    */

    if (srcnode != NULL && srcnode->prefix != NULL &&
        srcnode->prefix->user_data_result != NULL) {
        src = srcnode->prefix->user_data_result;
    } else {
        //SCLogError(SC_ERR_IPONLY_RADIX, "Error, no userdata found at the radix"
        //           " on src node!");
        return;
    }

    if (dstnode != NULL && dstnode->prefix != NULL &&
        dstnode->prefix->user_data_result != NULL) {
        dst = dstnode->prefix->user_data_result;
    } else {
        //SCLogError(SC_ERR_IPONLY_RADIX, "Error, no userdata found at the radix"
        //           " on dst node!");
        return;
    }

    uint32_t u;
    for (u = 0; u < src->size; u++) {
        SCLogDebug("And %"PRIu8" & %"PRIu8, src->array[u], dst->array[u]);

        /* The final results will be at io_tctx */
        io_tctx->sig_match_array[u] = dst->array[u] & src->array[u];

        /* We have to move the logic of the signature checking
         * to the main detect loop, in order to apply the
         * priority of actions (pass, drop, reject, alert) */
        if (io_tctx->sig_match_array[u] != 0) {
            /* We have a match :) Let's see from which signum's */
            uint8_t bitarray = io_tctx->sig_match_array[u];
            uint8_t i = 0;

            for (; i < 8; i++, bitarray = bitarray >> 1) {
                if (bitarray & 0x01) {
                    Signature *s = de_ctx->sig_array[u * 8 + i];

                    /* Need to check the protocol first */
                    if (!(s->proto.proto[(p->proto/8)] & (1 << (p->proto % 8))))
                        continue;

                    SCLogDebug("Signum %"PRIu16" match (sid: %"PRIu16", msg: %s)",
                               u * 8 + i, s->id, s->msg);

                    if ( !(s->flags & SIG_FLAG_NOALERT)) {
                        PacketAlertAppend(det_ctx, s, p);
                    }
                }
            }
        }
    }
}

/**
 * \brief Build the radix trees from the lists of parsed adresses in CIDR format
 *        the result should be 4 radix trees: src/dst ipv4 and src/dst ipv6
 *        holding SigNumArrays, each of them with a hierarchical relation
 *        of subnets and hosts
 *
 * \param de_ctx Pointer to the current detection engine
 */
void IPOnlyPrepare(DetectEngineCtx *de_ctx) {
    SCLogDebug("Preparing Final Lists");

    /*
       IPOnlyCIDRListPrint((de_ctx->io_ctx).ip_src);
       IPOnlyCIDRListPrint((de_ctx->io_ctx).ip_dst);
     */

    IPOnlyCIDRItem *src, *dst;
    SCRadixNode *node = NULL;

    /* Prepare Src radix trees */
    for (src = (de_ctx->io_ctx).ip_src; src != NULL; ) {
        if (src->family == AF_INET) {
            /*
            SCLogDebug("To IPv4");
            SCLogDebug("Item has netmask %"PRIu16" negated: %s; IP: %s; "
                       "signum: %"PRIu16, src->netmask,
                        (src->negated) ? "yes":"no",
                        inet_ntoa( *(struct in_addr*)&src->ip[0]),
                        src->signum);
            */

            if (src->netmask == 32)
                node = SCRadixFindKeyIPV4ExactMatch((uint8_t *)&src->ip[0],
                                                    (de_ctx->io_ctx).tree_ipv4src);
            else
                node = SCRadixFindKeyIPV4Netblock((uint8_t *)&src->ip[0],
                                                  (de_ctx->io_ctx).tree_ipv4src,
                                                  src->netmask);

            if (node == NULL) {
                SCLogDebug("Exact match not found");

                /** Not found, look if there's a subnet of this range with
                 * bigger netmask */
                node = SCRadixFindKeyIPV4BestMatch((uint8_t *)&src->ip[0],
                                                   (de_ctx->io_ctx).tree_ipv4src);

                if (node == NULL) {
                    SCLogDebug("best match not found");

                    /* Not found, insert a new one */
                    SigNumArray *sna = SigNumArrayNew(de_ctx, &de_ctx->io_ctx);

                    /* Update the sig */
                    uint8_t tmp = 1 << (src->signum % 8);

                    if (src->negated > 0)
                        /* Unset it */
                        sna->array[src->signum / 8] &= ~tmp;
                    else
                        /* Set it */
                        sna->array[src->signum / 8] |= tmp;

                    if (src->netmask == 32)
                        node = SCRadixAddKeyIPV4((uint8_t *)&src->ip[0],
                                                 (de_ctx->io_ctx).tree_ipv4src, sna);
                    else
                        node = SCRadixAddKeyIPV4Netblock((uint8_t *)&src->ip[0],
                                                         (de_ctx->io_ctx).tree_ipv4src,
                                                         sna, src->netmask);

                    if (node == NULL)
                        SCLogError(SC_ERR_IPONLY_RADIX, "Error inserting in the "
                                                        "src ipv4 radix tree");
                } else {
                    SCLogDebug("Best match found");

                    /* Found, copy the sig num table, add this signum and insert */
                    SigNumArray *sna = NULL;
                    sna = SigNumArrayCopy((SigNumArray *) node->prefix->user_data_result);

                    /* Update the sig */
                    uint8_t tmp = 1 << (src->signum % 8);

                    if (src->negated > 0)
                        /* Unset it */
                        sna->array[src->signum / 8] &= ~tmp;
                    else
                        /* Set it */
                        sna->array[src->signum / 8] |= tmp;

                    if (src->netmask == 32)
                        node = SCRadixAddKeyIPV4((uint8_t *)&src->ip[0],
                                                 (de_ctx->io_ctx).tree_ipv4src, sna);
                    else
                        node = SCRadixAddKeyIPV4Netblock((uint8_t *)&src->ip[0],
                                                         (de_ctx->io_ctx).tree_ipv4src, sna,
                                                         src->netmask);

                    if (node == NULL)
                        SCLogError(SC_ERR_IPONLY_RADIX, "Error inserting in the"
                                   " src ipv4 radix tree");
                }
            } else {
                SCLogDebug("Exact match found");

                /* it's already inserted. Update it */
                SigNumArray *sna = (SigNumArray *)node->prefix->user_data_result;

                /* Update the sig */
                uint8_t tmp = 1 << (src->signum % 8);

                if (src->negated > 0)
                    /* Unset it */
                    sna->array[src->signum / 8] &= ~tmp;
                else
                    /* Set it */
                    sna->array[src->signum / 8] |= tmp;
            }
        } else if (src->family == AF_INET6) {
            SCLogDebug("To IPv6");

            if (src->netmask == 128)
                node = SCRadixFindKeyIPV6ExactMatch((uint8_t *)&src->ip[0],
                                                    (de_ctx->io_ctx).tree_ipv6src);
            else
                node = SCRadixFindKeyIPV6Netblock((uint8_t *)&src->ip[0],
                                                  (de_ctx->io_ctx).tree_ipv6src,
                                                  src->netmask);

            if (node == NULL) {
                /* Not found, look if there's a subnet of this range with bigger netmask */
                node = SCRadixFindKeyIPV6BestMatch((uint8_t *)&src->ip[0],
                                                   (de_ctx->io_ctx).tree_ipv6src);

                if (node == NULL) {
                    /* Not found, insert a new one */
                    SigNumArray *sna = SigNumArrayNew(de_ctx, &de_ctx->io_ctx);

                    /* Update the sig */
                    uint8_t tmp = 1 << (src->signum % 8);

                    if (src->negated > 0)
                        /* Unset it */
                        sna->array[src->signum / 8] &= ~tmp;
                    else
                        /* Set it */
                        sna->array[src->signum / 8] |= tmp;

                    if (src->netmask == 128)
                        node = SCRadixAddKeyIPV6((uint8_t *)&src->ip[0],
                                                 (de_ctx->io_ctx).tree_ipv6src, sna);
                    else
                        node = SCRadixAddKeyIPV6Netblock((uint8_t *)&src->ip[0],
                                                         (de_ctx->io_ctx).tree_ipv6src,
                                                         sna, src->netmask);
                    if (node == NULL)
                        SCLogError(SC_ERR_IPONLY_RADIX, "Error inserting in the src "
                                   "ipv6 radix tree");
                } else {
                    /* Found, copy the sig num table, add this signum and insert */
                    SigNumArray *sna = NULL;
                    sna = SigNumArrayCopy((SigNumArray *)node->prefix->user_data_result);

                    /* Update the sig */
                    uint8_t tmp = 1 << (src->signum % 8);
                    if (src->negated > 0)
                        /* Unset it */
                        sna->array[src->signum / 8] &= ~tmp;
                    else
                        /* Set it */
                        sna->array[src->signum / 8] |= tmp;

                    if (src->netmask == 128)
                        node = SCRadixAddKeyIPV6((uint8_t *)&src->ip[0],
                                                 (de_ctx->io_ctx).tree_ipv6src, sna);
                    else
                        node = SCRadixAddKeyIPV6Netblock((uint8_t *)&src->ip[0],
                                                         (de_ctx->io_ctx).tree_ipv6src,
                                                         sna, src->netmask);
                    if (node == NULL)
                        SCLogError(SC_ERR_IPONLY_RADIX, "Error inserting in the src "
                                   "ipv6 radix tree");
                }
            } else {
                /* it's already inserted. Update it */
                SigNumArray *sna = (SigNumArray *)node->prefix->user_data_result;

                /* Update the sig */
                uint8_t tmp = 1 << (src->signum % 8);
                if (src->negated > 0)
                    /* Unset it */
                    sna->array[src->signum / 8] &= ~tmp;
                else
                    /* Set it */
                    sna->array[src->signum / 8] |= tmp;
            }
        }
        IPOnlyCIDRItem *tmpaux = src;
        src = src->next;
        SCFree(tmpaux);
    }

    SCLogDebug("dsts:");

    /* Prepare Dst radix trees */
    for (dst = (de_ctx->io_ctx).ip_dst; dst != NULL; ) {
        if (dst->family == AF_INET) {

            SCLogDebug("To IPv4");
            SCLogDebug("Item has netmask %"PRIu16" negated: %s; IP: %s; signum:"
                       " %"PRIu16"", dst->netmask, (dst->negated)?"yes":"no",
                       inet_ntoa(*(struct in_addr*)&dst->ip[0]), dst->signum);

            if (dst->netmask == 32)
                node = SCRadixFindKeyIPV4ExactMatch((uint8_t *) &dst->ip[0],
                                                    (de_ctx->io_ctx).tree_ipv4dst);
            else
                node = SCRadixFindKeyIPV4Netblock((uint8_t *) &dst->ip[0],
                                                  (de_ctx->io_ctx).tree_ipv4dst,
                                                  dst->netmask);

            if (node == NULL) {
                SCLogDebug("Exact match not found");

                /**
                 * Not found, look if there's a subnet of this range
                 * with bigger netmask
                 */
                node = SCRadixFindKeyIPV4BestMatch((uint8_t *)&dst->ip[0],
                                                   (de_ctx->io_ctx).tree_ipv4dst);
                if (node == NULL) {
                    SCLogDebug("Best match not found");

                    /** Not found, insert a new one */
                    SigNumArray *sna = SigNumArrayNew(de_ctx, &de_ctx->io_ctx);

                    /** Update the sig */
                    uint8_t tmp = 1 << (dst->signum % 8);
                    if (dst->negated > 0)
                        /** Unset it */
                        sna->array[dst->signum / 8] &= ~tmp;
                    else
                        /** Set it */
                        sna->array[dst->signum / 8] |= tmp;

                    if (dst->netmask == 32)
                        node = SCRadixAddKeyIPV4((uint8_t *)&dst->ip[0],
                                                 (de_ctx->io_ctx).tree_ipv4dst, sna);
                    else
                        node = SCRadixAddKeyIPV4Netblock((uint8_t *)&dst->ip[0],
                                                         (de_ctx->io_ctx).tree_ipv4dst,
                                                         sna, dst->netmask);

                    if (node == NULL)
                        SCLogError(SC_ERR_IPONLY_RADIX, "Error inserting in the dst "
                                   "ipv4 radix tree");
                } else {
                    SCLogDebug("Best match found");

                    /* Found, copy the sig num table, add this signum and insert */
                    SigNumArray *sna = NULL;
                    sna = SigNumArrayCopy((SigNumArray *)node->prefix->user_data_result);

                    /* Update the sig */
                    uint8_t tmp = 1 << (dst->signum % 8);
                    if (dst->negated > 0)
                        /* Unset it */
                        sna->array[dst->signum / 8] &= ~tmp;
                    else
                        /* Set it */
                        sna->array[dst->signum / 8] |= tmp;

                    if (dst->netmask == 32)
                        node = SCRadixAddKeyIPV4((uint8_t *)&dst->ip[0],
                                                 (de_ctx->io_ctx).tree_ipv4dst, sna);
                    else
                        node = SCRadixAddKeyIPV4Netblock((uint8_t *)&dst->ip[0],
                                                         (de_ctx->io_ctx).tree_ipv4dst,
                                                          sna, dst->netmask);

                    if (node == NULL)
                        SCLogError(SC_ERR_IPONLY_RADIX, "Error inserting in the dst "
                                   "ipv4 radix tree");
                }
            } else {
                SCLogDebug("Exact match found");

                /* it's already inserted. Update it */
                SigNumArray *sna = (SigNumArray *)node->prefix->user_data_result;

                /* Update the sig */
                uint8_t tmp = 1 << (dst->signum % 8);
                if (dst->negated > 0)
                    /* Unset it */
                    sna->array[dst->signum / 8] &= ~tmp;
                else
                    /* Set it */
                    sna->array[dst->signum / 8] |= tmp;
            }
        } else if (dst->family == AF_INET6) {
            SCLogDebug("To IPv6");

            if (dst->netmask == 128)
                node = SCRadixFindKeyIPV6ExactMatch((uint8_t *)&dst->ip[0],
                                                    (de_ctx->io_ctx).tree_ipv6dst);
            else
                node = SCRadixFindKeyIPV6Netblock((uint8_t *)&dst->ip[0],
                                                  (de_ctx->io_ctx).tree_ipv6dst,
                                                  dst->netmask);

            if (node == NULL) {
                /** Not found, look if there's a subnet of this range with
                 * bigger netmask
                 */
                node = SCRadixFindKeyIPV6BestMatch((uint8_t *)&dst->ip[0],
                                                   (de_ctx->io_ctx).tree_ipv6dst);

                if (node == NULL) {
                    /* Not found, insert a new one */
                    SigNumArray *sna = SigNumArrayNew(de_ctx, &de_ctx->io_ctx);

                    /* Update the sig */
                    uint8_t tmp = 1 << (dst->signum % 8);
                    if (dst->negated > 0)
                        /* Unset it */
                        sna->array[dst->signum / 8] &= ~tmp;
                    else
                        /* Set it */
                        sna->array[dst->signum / 8] |= tmp;

                    if (dst->netmask == 128)
                        node = SCRadixAddKeyIPV6((uint8_t *)&dst->ip[0],
                                                 (de_ctx->io_ctx).tree_ipv6dst, sna);
                    else
                        node = SCRadixAddKeyIPV6Netblock((uint8_t *)&dst->ip[0],
                                                         (de_ctx->io_ctx).tree_ipv6dst,
                                                          sna, dst->netmask);

                    if (node == NULL)
                        SCLogError(SC_ERR_IPONLY_RADIX, "Error inserting in the dst "
                                   "ipv6 radix tree");
                } else {
                    /* Found, copy the sig num table, add this signum and insert */
                    SigNumArray *sna = NULL;
                    sna = SigNumArrayCopy((SigNumArray *)node->prefix->user_data_result);

                    /* Update the sig */
                    uint8_t tmp = 1 << (dst->signum % 8);
                    if (dst->negated > 0)
                        /* Unset it */
                        sna->array[dst->signum / 8] &= ~tmp;
                    else
                        /* Set it */
                        sna->array[dst->signum / 8] |= tmp;

                    if (dst->netmask == 128)
                        node = SCRadixAddKeyIPV6((uint8_t *)&dst->ip[0],
                                                 (de_ctx->io_ctx).tree_ipv6dst, sna);
                    else
                        node = SCRadixAddKeyIPV6Netblock((uint8_t *)&dst->ip[0],
                                                         (de_ctx->io_ctx).tree_ipv6dst,
                                                         sna, dst->netmask);

                    if (node == NULL)
                        SCLogError(SC_ERR_IPONLY_RADIX, "Error inserting in the dst "
                                   "ipv6 radix tree");
                }
            } else {
                /* it's already inserted. Update it */
                SigNumArray *sna = (SigNumArray *)node->prefix->user_data_result;

                /* Update the sig */
                uint8_t tmp = 1 << (dst->signum % 8);
                if (dst->negated > 0)
                    /* Unset it */
                    sna->array[dst->signum / 8] &= ~tmp;
                else
                    /* Set it */
                    sna->array[dst->signum / 8] |= tmp;
            }
        }
        IPOnlyCIDRItem *tmpaux = dst;
        dst = dst->next;
        SCFree(tmpaux);
    }

    /* print all the trees: for debuggin it might print too much info
    SCLogDebug("Radix tree src ipv4:");
    SCRadixPrintTree((de_ctx->io_ctx).tree_ipv4src);
    SCLogDebug("Radix tree src ipv6:");
    SCRadixPrintTree((de_ctx->io_ctx).tree_ipv6src);
    SCLogDebug("__________________");

    SCLogDebug("Radix tree dst ipv4:");
    SCRadixPrintTree((de_ctx->io_ctx).tree_ipv4dst);
    SCLogDebug("Radix tree dst ipv6:");
    SCRadixPrintTree((de_ctx->io_ctx).tree_ipv6dst);
    SCLogDebug("__________________");
    */
}

/**
 * \brief Add a signature to the lists of Adrresses in CIDR format (sorted)
 *        this step is necesary to build the radix tree with a hierarchical
 *        relation between nodes
 * \param de_ctx Pointer to the current detection engine context
 * \param de_ctx Pointer to the current ip only detection engine contest
 * \param s Pointer to the current signature
 */
void IPOnlyAddSignature(DetectEngineCtx *de_ctx, DetectEngineIPOnlyCtx *io_ctx,
                        Signature *s) {
    if (!(s->flags & SIG_FLAG_IPONLY))
        return;

    /* Set the internal signum to the list before merging */
    IPOnlyCIDRListSetSigNum(s->CidrSrc, s->num);

    IPOnlyCIDRListSetSigNum(s->CidrDst, s->num);

    /**
     * ipv4 and ipv6 are mixed, but later we will separate them into
     * different trees
     */
    io_ctx->ip_src = IPOnlyCIDRItemInsert(io_ctx->ip_src, s->CidrSrc);
    io_ctx->ip_dst = IPOnlyCIDRItemInsert(io_ctx->ip_dst, s->CidrDst);

    if (s->num > io_ctx->max_idx)
        io_ctx->max_idx = s->num;

    /* enable the sig in the bitarray */
    io_ctx->sig_init_array[(s->num/8)] |= 1 << (s->num % 8);
}

#ifdef UNITTESTS
/**
 * \test check that we set a Signature as IPOnly because it has no rule
 *       option appending a SigMatch and no port is fixed
 */

static int IPOnlyTestSig01(void) {
    int result = 0;
    DetectEngineCtx de_ctx;

    memset(&de_ctx, 0, sizeof(DetectEngineCtx));

    de_ctx.flags |= DE_QUIET;

    Signature *s = SigInit(&de_ctx,"alert tcp any any -> any any (msg:\"SigTest40-01 sig is IPOnly \"; sid:400001; rev:1;)");
    if (s == NULL) {
        goto end;
    }
    if(SignatureIsIPOnly(&de_ctx, s))
        result = 1;
    else
        printf("expected a IPOnly signature: ");

    SigFree(s);
end:
    return result;
}

/**
 * \test check that we dont set a Signature as IPOnly because it has no rule
 *       option appending a SigMatch but a port is fixed
 */

static int IPOnlyTestSig02 (void) {
    int result = 0;
    DetectEngineCtx de_ctx;
    memset (&de_ctx, 0, sizeof(DetectEngineCtx));

    memset(&de_ctx, 0, sizeof(DetectEngineCtx));

    de_ctx.flags |= DE_QUIET;

    Signature *s = SigInit(&de_ctx,"alert tcp any any -> any 80 (msg:\"SigTest40-02 sig is not IPOnly \"; sid:400001; rev:1;)");
    if (s == NULL) {
        goto end;
    }
    if(!(SignatureIsIPOnly(&de_ctx, s)))
        result=1;
    else
        printf("got a IPOnly signature: ");

    SigFree(s);

end:
    return result;
}

/**
 * \test check that we set dont set a Signature as IPOnly
 *  because it has rule options appending a SigMatch like content, and pcre
 */

static int IPOnlyTestSig03 (void) {
    int result = 1;
    DetectEngineCtx *de_ctx;
    Signature *s=NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;
    de_ctx->flags |= DE_QUIET;

    /* combination of pcre and content */
    s = SigInit(de_ctx,"alert tcp any any -> any any (msg:\"SigTest40-03 sig is not IPOnly (pcre and content) \"; content:\"php\"; pcre:\"/require(_once)?/i\"; classtype:misc-activity; sid:400001; rev:1;)");
    if (s == NULL) {
        goto end;
    }
    if(SignatureIsIPOnly(de_ctx, s))
    {
        printf("got a IPOnly signature (content): ");
        result=0;
    }
    SigFree(s);

    /* content */
    s = SigInit(de_ctx,"alert tcp any any -> any any (msg:\"SigTest40-03 sig is not IPOnly (content) \"; content:\"match something\"; classtype:misc-activity; sid:400001; rev:1;)");
    if (s == NULL) {
        goto end;
    }
    if(SignatureIsIPOnly(de_ctx, s))
    {
        printf("got a IPOnly signature (content): ");
        result=0;
    }
    SigFree(s);

    /* uricontent */
    s = SigInit(de_ctx,"alert tcp any any -> any any (msg:\"SigTest40-03 sig is not IPOnly (uricontent) \"; uricontent:\"match something\"; classtype:misc-activity; sid:400001; rev:1;)");
    if (s == NULL) {
        goto end;
    }
    if(SignatureIsIPOnly(de_ctx, s))
    {
        printf("got a IPOnly signature (uricontent): ");
        result=0;
    }
    SigFree(s);

    /* pcre */
    s = SigInit(de_ctx,"alert tcp any any -> any any (msg:\"SigTest40-03 sig is not IPOnly (pcre) \"; pcre:\"/e?idps rule[sz]/i\"; classtype:misc-activity; sid:400001; rev:1;)");
    if (s == NULL) {
        goto end;
    }
    if(SignatureIsIPOnly(de_ctx, s))
    {
        printf("got a IPOnly signature (pcre): ");
        result=0;
    }
    SigFree(s);

    /* flow */
    s = SigInit(de_ctx,"alert tcp any any -> any any (msg:\"SigTest40-03 sig is not IPOnly (flow) \"; flow:to_server; classtype:misc-activity; sid:400001; rev:1;)");
    if (s == NULL) {
        goto end;
    }
    if(SignatureIsIPOnly(de_ctx, s))
    {
        printf("got a IPOnly signature (flow): ");
        result=0;
    }
    SigFree(s);

    /* dsize */
    s = SigInit(de_ctx,"alert tcp any any -> any any (msg:\"SigTest40-03 sig is not IPOnly (dsize) \"; dsize:100; classtype:misc-activity; sid:400001; rev:1;)");
    if (s == NULL) {
        goto end;
    }
    if(SignatureIsIPOnly(de_ctx, s))
    {
        printf("got a IPOnly signature (dsize): ");
        result=0;
    }
    SigFree(s);

    /* flowbits */
    s = SigInit(de_ctx,"alert tcp any any -> any any (msg:\"SigTest40-03 sig is not IPOnly (flowbits) \"; flowbits:unset; classtype:misc-activity; sid:400001; rev:1;)");
    if (s == NULL) {
        goto end;
    }
    if(SignatureIsIPOnly(de_ctx, s))
    {
        printf("got a IPOnly signature (flowbits): ");
        result=0;
    }
    SigFree(s);

    /* flowvar */
    s = SigInit(de_ctx,"alert tcp any any -> any any (msg:\"SigTest40-03 sig is not IPOnly (flowvar) \"; pcre:\"/(?<flow_var>.*)/i\"; flowvar:var,\"str\"; classtype:misc-activity; sid:400001; rev:1;)");
    if (s == NULL) {
        goto end;
    }
    if(SignatureIsIPOnly(de_ctx, s))
    {
        printf("got a IPOnly signature (flowvar): ");
        result=0;
    }
    SigFree(s);

    /* pktvar */
    s = SigInit(de_ctx,"alert tcp any any -> any any (msg:\"SigTest40-03 sig is not IPOnly (pktvar) \"; pcre:\"/(?<pkt_var>.*)/i\"; pktvar:var,\"str\"; classtype:misc-activity; sid:400001; rev:1;)");
    if (s == NULL) {
        goto end;
    }
    if(SignatureIsIPOnly(de_ctx, s))
    {
        printf("got a IPOnly signature (pktvar): ");
        result=0;
    }
    SigFree(s);

end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test
 */
static int IPOnlyTestSig04 (void) {
    int result = 1;

    IPOnlyCIDRItem *head = NULL;
    IPOnlyCIDRItem *new;

    new = IPOnlyCIDRItemNew();
    new->netmask= 10;

    head = IPOnlyCIDRItemInsert(head, new);

    new = IPOnlyCIDRItemNew();
    new->netmask= 11;

    head = IPOnlyCIDRItemInsert(head, new);

    new = IPOnlyCIDRItemNew();
    new->netmask= 9;

    head = IPOnlyCIDRItemInsert(head, new);

    new = IPOnlyCIDRItemNew();
    new->netmask= 10;

    head = IPOnlyCIDRItemInsert(head, new);

    new = IPOnlyCIDRItemNew();
    new->netmask= 10;

    head = IPOnlyCIDRItemInsert(head, new);

    IPOnlyCIDRListPrint(head);
    new = head;
    if (new->netmask != 9) {
        result = 0;
        goto end;
    }
    new = new->next;
    if (new->netmask != 10) {
        result = 0;
        goto end;
    }
    new = new->next;
    if (new->netmask != 10) {
        result = 0;
        goto end;
    }
    new = new->next;
    if (new->netmask != 10) {
        result = 0;
        goto end;
    }
    new = new->next;
    if (new->netmask != 11) {
        result = 0;
        goto end;
    }

end:
    IPOnlyCIDRListFree(head);
    return result;
}

/**
 * \test Test a set of ip only signatures making use a lot of
 * addresses for src and dst (all should match)
 */
int IPOnlyTestSig05(void) {
    int result = 0;
    uint8_t *buf = (uint8_t *)"Hi all!";
    uint16_t buflen = strlen((char *)buf);

    uint8_t numpkts = 1;
    uint8_t numsigs = 7;

    Packet *p[1];

    p[0] = UTHBuildPacket((uint8_t *)buf, buflen, IPPROTO_TCP);

    char *sigs[numsigs];
    sigs[0]= "alert tcp 192.168.1.5 any -> any any (msg:\"Testing src ip (sid 1)\"; sid:1;)";
    sigs[1]= "alert tcp any any -> 192.168.1.1 any (msg:\"Testing dst ip (sid 2)\"; sid:2;)";
    sigs[2]= "alert tcp 192.168.1.5 any -> 192.168.1.1 any (msg:\"Testing src/dst ip (sid 3)\"; sid:3;)";
    sigs[3]= "alert tcp 192.168.1.5 any -> 192.168.1.1 any (msg:\"Testing src/dst ip (sid 4)\"; sid:4;)";
    sigs[4]= "alert tcp 192.168.1.0/24 any -> any any (msg:\"Testing src/dst ip (sid 5)\"; sid:5;)";
    sigs[5]= "alert tcp any any -> 192.168.0.0/16 any (msg:\"Testing src/dst ip (sid 6)\"; sid:6;)";
    sigs[6]= "alert tcp 192.168.1.0/24 any -> 192.168.0.0/16 any (msg:\"Testing src/dst ip (sid 7)\"; content:\"Hi all\";sid:7;)";

    /* Sid numbers (we could extract them from the sig) */
    uint32_t sid[7] = { 1, 2, 3, 4, 5, 6, 7};
    uint32_t results[7] = { 1, 1, 1, 1, 1, 1, 1};

    result = UTHGenericTest(p, numpkts, sigs, sid, (uint32_t *) results, numsigs);

    UTHFreePackets(p, numpkts);

    return result;
}

/**
 * \test Test a set of ip only signatures making use a lot of
 * addresses for src and dst (none should match)
 */
int IPOnlyTestSig06(void) {
    int result = 0;
    uint8_t *buf = (uint8_t *)"Hi all!";
    uint16_t buflen = strlen((char *)buf);

    uint8_t numpkts = 1;
    uint8_t numsigs = 7;

    Packet *p[1];

    p[0] = UTHBuildPacketSrcDst((uint8_t *)buf, buflen, IPPROTO_TCP, "80.58.0.33", "195.235.113.3");

    char *sigs[numsigs];
    sigs[0]= "alert tcp 192.168.1.5 any -> any any (msg:\"Testing src ip (sid 1)\"; sid:1;)";
    sigs[1]= "alert tcp any any -> 192.168.1.1 any (msg:\"Testing dst ip (sid 2)\"; sid:2;)";
    sigs[2]= "alert tcp 192.168.1.5 any -> 192.168.1.1 any (msg:\"Testing src/dst ip (sid 3)\"; sid:3;)";
    sigs[3]= "alert tcp 192.168.1.5 any -> 192.168.1.1 any (msg:\"Testing src/dst ip (sid 4)\"; sid:4;)";
    sigs[4]= "alert tcp 192.168.1.0/24 any -> any any (msg:\"Testing src/dst ip (sid 5)\"; sid:5;)";
    sigs[5]= "alert tcp any any -> 192.168.0.0/16 any (msg:\"Testing src/dst ip (sid 6)\"; sid:6;)";
    sigs[6]= "alert tcp 192.168.1.0/24 any -> 192.168.0.0/16 any (msg:\"Testing src/dst ip (sid 7)\"; content:\"Hi all\";sid:7;)";

    /* Sid numbers (we could extract them from the sig) */
    uint32_t sid[7] = { 1, 2, 3, 4, 5, 6, 7};
    uint32_t results[7] = { 0, 0, 0, 0, 0, 0, 0};

    result = UTHGenericTest(p, numpkts, sigs, sid, (uint32_t *) results, numsigs);

    UTHFreePackets(p, numpkts);

    return result;
}

/**
 * \test Test a set of ip only signatures making use a lot of
 * addresses for src and dst (all should match)
 */
int IPOnlyTestSig07(void) {
    int result = 0;
    uint8_t *buf = (uint8_t *)"Hi all!";
    uint16_t buflen = strlen((char *)buf);

    uint8_t numpkts = 1;
    uint8_t numsigs = 7;

    Packet *p[1];

    p[0] = UTHBuildPacket((uint8_t *)buf, buflen, IPPROTO_TCP);

    char *sigs[numsigs];
    sigs[0]= "alert tcp 192.168.1.5 any -> 192.168.0.0/16 any (msg:\"Testing src/dst ip (sid 1)\"; sid:1;)";
    sigs[1]= "alert tcp [192.168.1.2,192.168.1.5,192.168.1.4] any -> 192.168.1.1 any (msg:\"Testing src/dst ip (sid 2)\"; sid:2;)";
    sigs[2]= "alert tcp [192.168.1.0/24,!192.168.1.1] any -> 192.168.1.1 any (msg:\"Testing src/dst ip (sid 3)\"; sid:3;)";
    sigs[3]= "alert tcp [192.0.0.0/8,!192.168.0.0/16,192.168.1.0/24,!192.168.1.1] any -> [192.168.1.0/24,!192.168.1.5] any (msg:\"Testing src/dst ip (sid 4)\"; sid:4;)";
    sigs[4]= "alert tcp any any -> any any (msg:\"Testing src/dst ip (sid 5)\"; sid:5;)";
    sigs[5]= "alert tcp any any -> [192.168.0.0/16,!192.168.1.0/24,192.168.1.1] any (msg:\"Testing src/dst ip (sid 6)\"; sid:6;)";
    sigs[6]= "alert tcp [78.129.202.0/24,192.168.1.5,78.129.205.64,78.129.214.103,78.129.223.19,78.129.233.17,78.137.168.33,78.140.132.11,78.140.133.15,78.140.138.105,78.140.139.105,78.140.141.107,78.140.141.114,78.140.143.103,78.140.143.13,78.140.145.144,78.140.170.164,78.140.23.18,78.143.16.7,78.143.46.124,78.157.129.71] any -> 192.168.1.1 any (msg:\"ET RBN Known Russian Business Network IP TCP - BLOCKING (246)\"; sid:7;)"; /* real sid:"2407490" */

    /* Sid numbers (we could extract them from the sig) */
    uint32_t sid[7] = { 1, 2, 3, 4, 5, 6, 7};
    uint32_t results[7] = { 1, 1, 1, 1, 1, 1, 1};

    result = UTHGenericTest(p, numpkts, sigs, sid, (uint32_t *) results, numsigs);

    UTHFreePackets(p, numpkts);

    return result;
}

/**
 * \test Test a set of ip only signatures making use a lot of
 * addresses for src and dst (none should match)
 */
int IPOnlyTestSig08(void) {
    int result = 0;
    uint8_t *buf = (uint8_t *)"Hi all!";
    uint16_t buflen = strlen((char *)buf);

    uint8_t numpkts = 1;
    uint8_t numsigs = 7;

    Packet *p[1];

    p[0] = UTHBuildPacketSrcDst((uint8_t *)buf, buflen, IPPROTO_TCP,"192.168.1.1","192.168.1.5");

    char *sigs[numsigs];
    sigs[0]= "alert tcp 192.168.1.5 any -> 192.168.0.0/16 any (msg:\"Testing src/dst ip (sid 1)\"; sid:1;)";
    sigs[1]= "alert tcp [192.168.1.2,192.168.1.5,192.168.1.4] any -> 192.168.1.1 any (msg:\"Testing src/dst ip (sid 2)\"; sid:2;)";
    sigs[2]= "alert tcp [192.168.1.0/24,!192.168.1.1] any -> 192.168.1.1 any (msg:\"Testing src/dst ip (sid 3)\"; sid:3;)";
    sigs[3]= "alert tcp [192.0.0.0/8,!192.168.0.0/16,192.168.1.0/24,!192.168.1.1] any -> [192.168.1.0/24,!192.168.1.5] any (msg:\"Testing src/dst ip (sid 4)\"; sid:4;)";
    sigs[4]= "alert tcp any any -> !192.168.1.5 any (msg:\"Testing src/dst ip (sid 5)\"; sid:5;)";
    sigs[5]= "alert tcp any any -> [192.168.0.0/16,!192.168.1.0/24,192.168.1.1] any (msg:\"Testing src/dst ip (sid 6)\"; sid:6;)";
    sigs[6]= "alert tcp [78.129.202.0/24,192.168.1.5,78.129.205.64,78.129.214.103,78.129.223.19,78.129.233.17,78.137.168.33,78.140.132.11,78.140.133.15,78.140.138.105,78.140.139.105,78.140.141.107,78.140.141.114,78.140.143.103,78.140.143.13,78.140.145.144,78.140.170.164,78.140.23.18,78.143.16.7,78.143.46.124,78.157.129.71] any -> 192.168.1.1 any (msg:\"ET RBN Known Russian Business Network IP TCP - BLOCKING (246)\"; sid:7;)"; /* real sid:"2407490" */

    /* Sid numbers (we could extract them from the sig) */
    uint32_t sid[7] = { 1, 2, 3, 4, 5, 6, 7};
    uint32_t results[7] = { 0, 0, 0, 0, 0, 0, 0};

    result = UTHGenericTest(p, numpkts, sigs, sid, (uint32_t *) results, numsigs);

    UTHFreePackets(p, numpkts);

    return result;
}

/**
 * \test Test a set of ip only signatures making use a lot of
 * addresses for src and dst (all should match)
 */
int IPOnlyTestSig09(void) {
    int result = 0;
    uint8_t *buf = (uint8_t *)"Hi all!";
    uint16_t buflen = strlen((char *)buf);

    uint8_t numpkts = 1;
    uint8_t numsigs = 7;

    Packet *p[1];

    p[0] = UTHBuildPacketIPV6SrcDst((uint8_t *)buf, buflen, IPPROTO_TCP, "3FFE:FFFF:7654:FEDA:1245:BA98:3210:4565", "3FFE:FFFF:7654:FEDA:1245:BA98:3210:4562");

    char *sigs[numsigs];
    sigs[0]= "alert tcp 3FFE:FFFF:7654:FEDA:1245:BA98:3210:4565 any -> any any (msg:\"Testing src ip (sid 1)\"; sid:1;)";
    sigs[1]= "alert tcp any any -> 3FFE:FFFF:7654:FEDA:1245:BA98:3210:4562 any (msg:\"Testing dst ip (sid 2)\"; sid:2;)";
    sigs[2]= "alert tcp 3FFE:FFFF:7654:FEDA:1245:BA98:3210:4565 any -> 3FFE:FFFF:7654:FEDA:1245:BA98:3210:4562 any (msg:\"Testing src/dst ip (sid 3)\"; sid:3;)";
    sigs[3]= "alert tcp 3FFE:FFFF:7654:FEDA:1245:BA98:3210:4565 any -> 3FFE:FFFF:7654:FEDA:1245:BA98:3210:0/96 any (msg:\"Testing src/dst ip (sid 4)\"; sid:4;)";
    sigs[4]= "alert tcp 3FFE:FFFF:7654:FEDA:0:0:0:0/64 any -> any any (msg:\"Testing src/dst ip (sid 5)\"; sid:5;)";
    sigs[5]= "alert tcp any any -> 3FFE:FFFF:7654:FEDA:0:0:0:0/64 any (msg:\"Testing src/dst ip (sid 6)\"; sid:6;)";
    sigs[6]= "alert tcp 3FFE:FFFF:7654:FEDA:0:0:0:0/64 any -> 3FFE:FFFF:7654:FEDA:0:0:0:0/64 any (msg:\"Testing src/dst ip (sid 7)\"; content:\"Hi all\";sid:7;)";

    /* Sid numbers (we could extract them from the sig) */
    uint32_t sid[7] = { 1, 2, 3, 4, 5, 6, 7};
    uint32_t results[7] = { 1, 1, 1, 1, 1, 1, 1};

    result = UTHGenericTest(p, numpkts, sigs, sid, (uint32_t *) results, numsigs);

    UTHFreePackets(p, numpkts);

    return result;
}

/**
 * \test Test a set of ip only signatures making use a lot of
 * addresses for src and dst (none should match)
 */
int IPOnlyTestSig10(void) {
    int result = 0;
    uint8_t *buf = (uint8_t *)"Hi all!";
    uint16_t buflen = strlen((char *)buf);

    uint8_t numpkts = 1;
    uint8_t numsigs = 7;

    Packet *p[1];

    p[0] = UTHBuildPacketIPV6SrcDst((uint8_t *)buf, buflen, IPPROTO_TCP, "3FFE:FFFF:7654:FEDA:1245:BA98:3210:4562", "3FFE:FFFF:7654:FEDA:1245:BA98:3210:4565");

    char *sigs[numsigs];
    sigs[0]= "alert tcp 3FFE:FFFF:7654:FEDA:1245:BA98:3210:4565 any -> any any (msg:\"Testing src ip (sid 1)\"; sid:1;)";
    sigs[1]= "alert tcp any any -> 3FFE:FFFF:7654:FEDA:1245:BA98:3210:4562 any (msg:\"Testing dst ip (sid 2)\"; sid:2;)";
    sigs[2]= "alert tcp 3FFE:FFFF:7654:FEDA:1245:BA98:3210:4565 any -> 3FFE:FFFF:7654:FEDA:1245:BA98:3210:4562 any (msg:\"Testing src/dst ip (sid 3)\"; sid:3;)";
    sigs[3]= "alert tcp 3FFE:FFFF:7654:FEDA:1245:BA98:3210:4565 any -> !3FFE:FFFF:7654:FEDA:1245:BA98:3210:4562/96 any (msg:\"Testing src/dst ip (sid 4)\"; sid:4;)";
    sigs[4]= "alert tcp !3FFE:FFFF:7654:FEDA:0:0:0:0/64 any -> any any (msg:\"Testing src/dst ip (sid 5)\"; sid:5;)";
    sigs[5]= "alert tcp any any -> !3FFE:FFFF:7654:FEDA:0:0:0:0/64 any (msg:\"Testing src/dst ip (sid 6)\"; sid:6;)";
    sigs[6]= "alert tcp 3FFE:FFFF:7654:FEDA:0:0:0:0/64 any -> 3FFE:FFFF:7654:FEDB:0:0:0:0/64 any (msg:\"Testing src/dst ip (sid 7)\"; content:\"Hi all\";sid:7;)";

    /* Sid numbers (we could extract them from the sig) */
    uint32_t sid[7] = { 1, 2, 3, 4, 5, 6, 7};
    uint32_t results[7] = { 0, 0, 0, 0, 0, 0, 0};

    result = UTHGenericTest(p, numpkts, sigs, sid, (uint32_t *) results, numsigs);

    UTHFreePackets(p, numpkts);

    return result;
}

/**
 * \test Test a set of ip only signatures making use a lot of
 * addresses for src and dst (all should match) with ipv4 and ipv6 mixed
 */
int IPOnlyTestSig11(void) {
    int result = 0;
    uint8_t *buf = (uint8_t *)"Hi all!";
    uint16_t buflen = strlen((char *)buf);

    uint8_t numpkts = 2;
    uint8_t numsigs = 7;

    Packet *p[2];

    p[0] = UTHBuildPacketIPV6SrcDst((uint8_t *)buf, buflen, IPPROTO_TCP, "3FFE:FFFF:7654:FEDA:1245:BA98:3210:4565", "3FFE:FFFF:7654:FEDA:1245:BA98:3210:4562");
    p[1] = UTHBuildPacketSrcDst((uint8_t *)buf, buflen, IPPROTO_TCP,"192.168.1.1","192.168.1.5");

    char *sigs[numsigs];
    sigs[0]= "alert tcp 3FFE:FFFF:7654:FEDA:1245:BA98:3210:4565,192.168.1.1 any -> 3FFE:FFFF:7654:FEDA:0:0:0:0/64,192.168.1.5 any (msg:\"Testing src/dst ip (sid 1)\"; sid:1;)";
    sigs[1]= "alert tcp [192.168.1.1,3FFE:FFFF:7654:FEDA:1245:BA98:3210:4565,192.168.1.4,192.168.1.5,!192.168.1.0/24] any -> [3FFE:FFFF:7654:FEDA:1245:BA98:3210:4562,192.168.1.0/24] any (msg:\"Testing src/dst ip (sid 2)\"; sid:2;)";
    sigs[2]= "alert tcp [3FFE:FFFF:7654:FEDA:0:0:0:0/64,!3FFE:FFFF:7654:FEDA:1245:BA98:3210:4562,192.168.1.1] any -> [3FFE:FFFF:7654:FEDA:1245:BA98:3210:4562,192.168.1.5] any (msg:\"Testing src/dst ip (sid 3)\"; sid:3;)";
    sigs[3]= "alert tcp [3FFE:FFFF:0:0:0:0:0:0/32,!3FFE:FFFF:7654:FEDA:0:0:0:0/64,3FFE:FFFF:7654:FEDA:0:0:0:0/64,!3FFE:FFFF:7654:FEDA:1245:BA98:3210:4562,192.168.1.1] any -> [3FFE:FFFF:7654:FEDA:0:0:0:0/64,192.168.1.0/24,!3FFE:FFFF:7654:FEDA:1245:BA98:3210:4565] any (msg:\"Testing src/dst ip (sid 4)\"; sid:4;)";
    sigs[4]= "alert tcp any any -> any any (msg:\"Testing src/dst ip (sid 5)\"; sid:5;)";
    sigs[5]= "alert tcp any any -> [3FFE:FFFF:7654:FEDA:0:0:0:0/64,!3FFE:FFFF:7654:FEDA:0:0:0:0/64,3FFE:FFFF:7654:FEDA:1245:BA98:3210:4562,192.168.1.5] any (msg:\"Testing src/dst ip (sid 6)\"; sid:6;)";
    sigs[6]= "alert tcp [78.129.202.0/24,3FFE:FFFF:7654:FEDA:1245:BA98:3210:4565,192.168.1.1,78.129.205.64,78.129.214.103,78.129.223.19,78.129.233.17,78.137.168.33,78.140.132.11,78.140.133.15,78.140.138.105,78.140.139.105,78.140.141.107,78.140.141.114,78.140.143.103,78.140.143.13,78.140.145.144,78.140.170.164,78.140.23.18,78.143.16.7,78.143.46.124,78.157.129.71] any -> [3FFE:FFFF:7654:FEDA:1245:BA98:3210:4562,192.0.0.0/8] any (msg:\"ET RBN Known Russian Business Network IP TCP - BLOCKING (246)\"; sid:7;)"; /* real sid:"2407490" */

    /* Sid numbers (we could extract them from the sig) */
    uint32_t sid[7] = { 1, 2, 3, 4, 5, 6, 7};
    uint32_t results[2][7] = {{ 1, 1, 1, 1, 1, 1, 1}, { 1, 1, 1, 1, 1, 1, 1}};

    result = UTHGenericTest(p, numpkts, sigs, sid, (uint32_t *) results, numsigs);

    UTHFreePackets(p, numpkts);

    return result;
}

/**
 * \test Test a set of ip only signatures making use a lot of
 * addresses for src and dst (none should match) with ipv4 and ipv6 mixed
 */
int IPOnlyTestSig12(void) {
    int result = 0;
    uint8_t *buf = (uint8_t *)"Hi all!";
    uint16_t buflen = strlen((char *)buf);

    uint8_t numpkts = 2;
    uint8_t numsigs = 7;

    Packet *p[2];

    p[0] = UTHBuildPacketIPV6SrcDst((uint8_t *)buf, buflen, IPPROTO_TCP,"3FBE:FFFF:7654:FEDA:1245:BA98:3210:4562","3FBE:FFFF:7654:FEDA:1245:BA98:3210:4565");
    p[1] = UTHBuildPacketSrcDst((uint8_t *)buf, buflen, IPPROTO_TCP,"195.85.1.1","80.198.1.5");

    char *sigs[numsigs];
    sigs[0]= "alert tcp 3FFE:FFFF:7654:FEDA:1245:BA98:3210:4565,192.168.1.1 any -> 3FFE:FFFF:7654:FEDA:0:0:0:0/64,192.168.1.5 any (msg:\"Testing src/dst ip (sid 1)\"; sid:1;)";
    sigs[1]= "alert tcp [192.168.1.1,3FFE:FFFF:7654:FEDA:1245:BA98:3210:4565,192.168.1.4,192.168.1.5,!192.168.1.0/24] any -> [3FFE:FFFF:7654:FEDA:1245:BA98:3210:4562,192.168.1.0/24] any (msg:\"Testing src/dst ip (sid 2)\"; sid:2;)";
    sigs[2]= "alert tcp [3FFE:FFFF:7654:FEDA:0:0:0:0/64,!3FFE:FFFF:7654:FEDA:1245:BA98:3210:4562,192.168.1.1] any -> [3FFE:FFFF:7654:FEDA:1245:BA98:3210:4562,192.168.1.5] any (msg:\"Testing src/dst ip (sid 3)\"; sid:3;)";
    sigs[3]= "alert tcp [3FFE:FFFF:0:0:0:0:0:0/32,!3FFE:FFFF:7654:FEDA:0:0:0:0/64,3FFE:FFFF:7654:FEDA:0:0:0:0/64,!3FFE:FFFF:7654:FEDA:1245:BA98:3210:4562,192.168.1.1] any -> [3FFE:FFFF:7654:FEDA:0:0:0:0/64,192.168.1.0/24,!3FFE:FFFF:7654:FEDA:1245:BA98:3210:4565] any (msg:\"Testing src/dst ip (sid 4)\"; sid:4;)";
    sigs[4]= "alert tcp any any -> [!3FBE:FFFF:7654:FEDA:1245:BA98:3210:4565,!80.198.1.5] any (msg:\"Testing src/dst ip (sid 5)\"; sid:5;)";
    sigs[5]= "alert tcp any any -> [3FFE:FFFF:7654:FEDA:0:0:0:0/64,!3FFE:FFFF:7654:FEDA:0:0:0:0/64,3FFE:FFFF:7654:FEDA:1245:BA98:3210:4562,192.168.1.5] any (msg:\"Testing src/dst ip (sid 6)\"; sid:6;)";
    sigs[6]= "alert tcp [78.129.202.0/24,3FFE:FFFF:7654:FEDA:1245:BA98:3210:4565,192.168.1.1,78.129.205.64,78.129.214.103,78.129.223.19,78.129.233.17,78.137.168.33,78.140.132.11,78.140.133.15,78.140.138.105,78.140.139.105,78.140.141.107,78.140.141.114,78.140.143.103,78.140.143.13,78.140.145.144,78.140.170.164,78.140.23.18,78.143.16.7,78.143.46.124,78.157.129.71] any -> [3FFE:FFFF:7654:FEDA:1245:BA98:3210:4562,192.0.0.0/8] any (msg:\"ET RBN Known Russian Business Network IP TCP - BLOCKING (246)\"; sid:7;)"; /* real sid:"2407490" */

    /* Sid numbers (we could extract them from the sig) */
    uint32_t sid[7] = { 1, 2, 3, 4, 5, 6, 7};
    uint32_t results[2][7] = {{ 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0}};

    result = UTHGenericTest(p, numpkts, sigs, sid, (uint32_t *) results, numsigs);

    UTHFreePackets(p, numpkts);

    return result;
}

#endif /* UNITTESTS */

void IPOnlyRegisterTests(void) {
#ifdef UNITTESTS
    UtRegisterTest("IPOnlyTestSig01", IPOnlyTestSig01, 1);
    UtRegisterTest("IPOnlyTestSig02", IPOnlyTestSig02, 1);
    UtRegisterTest("IPOnlyTestSig03", IPOnlyTestSig03, 1);
    UtRegisterTest("IPOnlyTestSig04", IPOnlyTestSig04, 1);

    UtRegisterTest("IPOnlyTestSig05", IPOnlyTestSig05, 1);
    UtRegisterTest("IPOnlyTestSig06", IPOnlyTestSig06, 1);
    UtRegisterTest("IPOnlyTestSig07", IPOnlyTestSig07, 1);
    UtRegisterTest("IPOnlyTestSig08", IPOnlyTestSig08, 1);

    UtRegisterTest("IPOnlyTestSig09", IPOnlyTestSig09, 1);
    UtRegisterTest("IPOnlyTestSig10", IPOnlyTestSig10, 1);
    UtRegisterTest("IPOnlyTestSig11", IPOnlyTestSig11, 1);
    UtRegisterTest("IPOnlyTestSig12", IPOnlyTestSig12, 1);
#endif
}

