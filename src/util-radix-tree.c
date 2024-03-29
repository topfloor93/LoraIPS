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
 *
 * Implementation of radix trees
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "suricata-common.h"

#include "suricata-common.h"
#ifdef OS_FREEBSD
#include <netinet/in.h>
#endif /* OS_FREEBSD */
#ifdef OS_DARWIN
#include <netinet/in.h>
#endif /* OS_FREEBSD */

#include "util-radix-tree.h"
#include "util-debug.h"
#include "util-error.h"
#include "util-unittest.h"

/**
 * \brief Validates an IPV4 address and returns the network endian arranged
 *        version of the IPV4 address
 *
 * \param addr Pointer to a character string containing an IPV4 address.  A
 *             valid IPV4 address is a character string containing a dotted
 *             format of "ddd.ddd.ddd.ddd"
 *
 * \retval Pointer to an in_addr instance containing the network endian format
 *         of the IPV4 address
 * \retval NULL if the IPV4 address is invalid
 */
struct in_addr *SCRadixValidateIPV4Address(const char *addr_str)
{
    struct in_addr *addr = NULL;

    if ( (addr = SCMalloc(sizeof(struct in_addr))) == NULL) {
        SCLogError(SC_ERR_FATAL, "Fatal error encountered in SCRadixValidateIPV4Address. Exiting...");
        exit(EXIT_FAILURE);
    }

    if (inet_pton(AF_INET, addr_str, addr) <= 0) {
        SCFree(addr);
        return NULL;
    }

    return addr;
}

/**
 * \brief Validates an IPV6 address and returns the network endian arranged
 *        version of the IPV6 addresss
 *
 * \param addr Pointer to a character string containing an IPV6 address
 *
 * \retval Pointer to a in6_addr instance containing the network endian format
 *         of the IPV6 address
 * \retval NULL if the IPV6 address is invalid
 */
struct in6_addr *SCRadixValidateIPV6Address(const char *addr_str)
{
    struct in6_addr *addr = NULL;

    if ( (addr = SCMalloc(sizeof(struct in6_addr))) == NULL) {
        SCLogError(SC_ERR_FATAL, "Fatal error encountered in SCRadixValidateIPV6Address. Exiting...");
        exit(EXIT_FAILURE);
    }

    if (inet_pton(AF_INET6, addr_str, addr) <= 0) {
        SCFree(addr);
        return NULL;
    }

    return addr;
}

/**
 * \brief Chops an ip address against a netmask.  For example an ip address
 *        192.168.240.1 would be chopped to 192.168.224.0 against a netmask
 *        value of 19.
 *
 * \param stream  Pointer the ip address that has to be chopped.
 * \param netmask The netmask value (cidr) to which the ip address has to be chopped.
 */
void SCRadixChopIPAddressAgainstNetmask(uint8_t *stream, uint8_t netmask,
                                               uint16_t key_bitlen)
{
    int mask = 0;
    int i = 0;
    int bytes = key_bitlen / 8;

    for (i = 0; i < bytes; i++) {
        mask = -1;
        if ( ((i + 1) * 8) > netmask) {
            if ( ((i + 1) * 8 - netmask) < 8)
                mask = -1 << ((i + 1) * 8 - netmask);
            else
                mask = 0;
        }
        stream[i] &= mask;
    }

    return;
}

/**
 * \brief Allocates and returns a new instance of SCRadixUserData.
 *
 * \param netmask The netmask entry (cidr) that has to be made in the new
 *                SCRadixUserData instance
 * \param user    The user data that has to be set for the above
 *                netmask in the newly created SCRadixUserData instance.
 *
 * \retval user_data Pointer to a new instance of SCRadixUserData.
 */
static SCRadixUserData *SCRadixAllocSCRadixUserData(uint8_t netmask, void *user)
{
    SCRadixUserData *user_data = SCMalloc(sizeof(SCRadixUserData));
    if (user_data == NULL) {
        return NULL;
    }

    memset(user_data, 0, sizeof(SCRadixUserData));

    user_data->netmask = netmask;
    user_data->user = user;

    return user_data;
}

/**
 * \brief Deallocates an instance of SCRadixUserData.
 *
 * \param user_data Pointer to the instance of SCRadixUserData that has to be
 *                  freed.
 */
static void SCRadixDeAllocSCRadixUserData(SCRadixUserData *user_data)
{
    SCFree(user_data);

    return;
}

/**
 * \brief Appends a user_data instance(SCRadixUserData) to a
 *        user_data(SCRadixUserData) list.  We add the new entry in descending
 *        order with respect to the netmask contained in the SCRadixUserData.
 *
 * \param new  Pointer to the SCRadixUserData to be added to the list.
 * \param list Pointer to the SCRadixUserData list head, to which "new" has to
 *             be appended.
 */
static void SCRadixAppendToSCRadixUserDataList(SCRadixUserData *new,
                                               SCRadixUserData **list)
{
    SCRadixUserData *temp = NULL;
    SCRadixUserData *prev = NULL;

    if (new == NULL || list == NULL) {
        SCLogError(SC_ERR_INVALID_ARGUMENTS, "new or list supplied as NULL");
        exit(EXIT_FAILURE);
    }

    /* add to the list in descending order.  The reason we do this is for
     * optimizing key retrieval for a ip key under a netblock */
    prev = temp = *list;
    while (temp != NULL) {
        if (new->netmask > temp->netmask)
            break;
        prev = temp;
        temp = temp->next;
    }

    if (temp == *list) {
        new->next = *list;
        *list = new;
    } else {
        new->next = prev->next;
        prev->next = new;
    }

    return;
}

/**
 * \brief Creates a new Prefix for a key.  Used internally by the API.
 *
 * \param key_stream Data that has to be wrapped in a SCRadixPrefix instance to
 *                   be processed for insertion/lookup/removal of a node by the
 *                   radix tree
 * \param key_bitlen The bitlen of the the above stream.  For example if the
 *                   stream holds the ipv4 address(4 bytes), bitlen would be 32
 * \param user       Pointer to the user data that has to be associated with
 *                   this key
 *
 * \retval prefix The newly created prefix instance on success; NULL on failure
 */
static SCRadixPrefix *SCRadixCreatePrefix(uint8_t *key_stream,
                                          uint16_t key_bitlen, void *user,
                                          uint8_t netmask)
{
    SCRadixPrefix *prefix = NULL;

    if ((key_bitlen % 8 != 0)) {
        SCLogError(SC_ERR_INVALID_ARGUMENT, "Invalid argument bitlen - %d",
                   key_bitlen);
        return NULL;
    }

    if (key_stream == NULL) {
        SCLogError(SC_ERR_INVALID_ARGUMENT, "Argument \"stream\" NULL");
        return NULL;
    }

    if ( (prefix = SCMalloc(sizeof(SCRadixPrefix))) == NULL)
        goto error;

    memset(prefix, 0, sizeof(SCRadixPrefix));

    if ( (prefix->stream = SCMalloc(key_bitlen / 8)) == NULL)
        goto error;

    memset(prefix->stream, 0, key_bitlen / 8);

    memcpy(prefix->stream, key_stream, key_bitlen / 8);
    prefix->bitlen = key_bitlen;

    prefix->user_data = SCRadixAllocSCRadixUserData(netmask, user);
    if (prefix->user_data == NULL) {
        goto error;
    }

    return prefix;

error:
    if (prefix != NULL) {
        if (prefix->stream != NULL) {
            SCFree(prefix->stream);
        }
        SCFree(prefix);
    }

    return NULL;
}

/**
 * \brief Adds a netmask and its user_data for a particular prefix stream.
 *
 * \param prefix  The prefix stream to which the netmask and its corresponding
 *                user data has to be added.
 * \param netmask The netmask value (cidr) that has to be added to the prefix.
 * \param user    The pointer to the user data corresponding to the above
 *                netmask.
 */
static void SCRadixAddNetmaskUserDataToPrefix(SCRadixPrefix *prefix,
                                              uint8_t netmask,
                                              void *user)
{
    if (prefix == NULL || user == NULL) {
        SCLogError(SC_ERR_INVALID_ARGUMENTS, "prefix or user NULL");
        exit(EXIT_FAILURE);
    }

    SCRadixAppendToSCRadixUserDataList(SCRadixAllocSCRadixUserData(netmask, user),
                                       &prefix->user_data);

    return;
}

/**
 * \brief Removes a particular user_data corresponding to a particular netmask
 *        entry, from a prefix.
 *
 * \param prefix  Pointer to the prefix from which the user_data/netmask entry
 *                has to be removed.
 * \param netmask The netmask value (cidr) whose user_data has to be deleted.
 */
static void SCRadixRemoveNetmaskUserDataFromPrefix(SCRadixPrefix *prefix,
                                                   uint8_t netmask)
{
    SCRadixUserData *temp = NULL, *prev = NULL;

    if (prefix == NULL) {
        SCLogError(SC_ERR_INVALID_ARGUMENTS, "prefix NULL");
        exit(EXIT_FAILURE);
    }

    prev = temp = prefix->user_data;
    while (temp != NULL) {
        if (temp->netmask == netmask) {
            if (temp == prefix->user_data)
                prefix->user_data = temp->next;
            else
                prev->next = temp->next;

            SCRadixDeAllocSCRadixUserData(temp);
            break;
        }
        prev = temp;
        temp = temp->next;
    }

    return;
}

/**
 * \brief Indicates if prefix contains an entry for an ip with a specific netmask.
 *
 * \param prefix  Pointer to the ip prefix that is being checked.
 * \param netmask The netmask value (cidr) that has to be checked for
 *                presence in the prefix.
 *
 * \retval 1 On match.
 * \retval 0 On no match.
 */
static int SCRadixPrefixContainNetmask(SCRadixPrefix *prefix, uint8_t netmask)
{
    SCRadixUserData *user_data = NULL;

    if (prefix == NULL) {
        SCLogError(SC_ERR_INVALID_ARGUMENT, "prefix is NULL");
        goto no_match;
    }

    user_data = prefix->user_data;
    while (user_data != NULL) {
        if (user_data->netmask == netmask)
            return 1;
        user_data = user_data->next;
    }

 no_match:
    return 0;
}

/**
 * \brief Returns the total netmask count for this prefix.
 *
 * \param prefix Pointer to the prefix
 *
 * \retval count The total netmask count for this prefix.
 */
static int SCRadixPrefixNetmaskCount(SCRadixPrefix *prefix)
{
    SCRadixUserData *user_data = NULL;
    uint32_t count = 0;

    if (prefix == NULL) {
        SCLogError(SC_ERR_INVALID_ARGUMENT, "prefix is NULL");
        return 0;
    }

    user_data = prefix->user_data;
    while (user_data != NULL) {
        count++;
        user_data = user_data->next;
    }

    return count;
}

/**
 * \brief Indicates if prefix contains an entry for an ip with a specific netmask
 *        and if it does, it sets the user data field
 *        SCRadixPrefix->user_data_result to the netmask user_data entry.
 *
 * \param prefix      Pointer to the ip prefix that is being checked.
 * \param netmask     The netmask value for which we will have to return the user_data
 * \param exact_match Bool flag which indicates if we should check if the prefix
 *                    holds proper netblock(< 32 for ipv4 and < 128 for ipv6) or not.
 *
 * \retval 1 On match.
 * \retval 0 On no match.
 */
static int SCRadixPrefixContainNetmaskAndSetUserData(SCRadixPrefix *prefix,
                                                     uint16_t netmask,
                                                     int exact_match)
{
    SCRadixUserData *user_data = NULL;

    if (prefix == NULL) {
        SCLogError(SC_ERR_INVALID_ARGUMENT, "prefix is NULL");
        goto no_match;
    }

    user_data = prefix->user_data;
    /* Check if we have a match for an exact ip.  An exact ip as in not a proper
     * netblock, i.e. an ip with a netmask of 32(ipv4) or 128(ipv6) */
    if (exact_match) {
        if (user_data->netmask == netmask) {
            prefix->user_data_result = user_data->user;
            return 1;
        } else {
            goto no_match;
        }
    }

    /* Check for the user_data entry for this netmask_value */
    while (user_data != NULL) {
        if (user_data->netmask == netmask) {
            prefix->user_data_result = user_data->user;
            return 1;
        }
        user_data = user_data->next;
    }

no_match:
    return 0;
}

/**
 * \brief Frees a SCRadixPrefix instance
 *
 * \param prefix Pointer to a prefix instance
 * \param tree   Pointer to the Radix tree to which this prefix belongs
 */
static void SCRadixReleasePrefix(SCRadixPrefix *prefix, SCRadixTree *tree)
{
    SCRadixUserData *user_data_temp1 = NULL;
    SCRadixUserData *user_data_temp2 = NULL;

    if (prefix != NULL) {
        if (prefix->stream != NULL)
            SCFree(prefix->stream);

        user_data_temp1 = prefix->user_data;
        user_data_temp2 = user_data_temp1;
        if (tree->Free != NULL) {
            while (user_data_temp1 != NULL) {
                user_data_temp2 = user_data_temp1;
                user_data_temp1 = user_data_temp1->next;
                tree->Free(user_data_temp2->user);
                SCRadixDeAllocSCRadixUserData(user_data_temp2);
            }
        } else if (user_data_temp1 != NULL) {
            SCFree(user_data_temp1);
        }

        SCFree(prefix);
    }

    return;
}

/**
 * \brief Creates a new node for the Radix tree
 *
 * \retval node The newly created node for the radix tree
 */
static inline SCRadixNode *SCRadixCreateNode()
{
    SCRadixNode *node = NULL;

    if ( (node = SCMalloc(sizeof(SCRadixNode))) == NULL) {
        SCLogError(SC_ERR_FATAL, "Fatal error encountered in SCRadixCreateNode. Exiting...");
        exit(EXIT_FAILURE);
    }
    memset(node, 0, sizeof(SCRadixNode));

    return node;
}

/**
 * \brief Frees a Radix tree node
 *
 * \param node Pointer to a Radix tree node
 * \param tree Pointer to the Radix tree to which this node belongs
 */
static void SCRadixReleaseNode(SCRadixNode *node, SCRadixTree *tree)
{
    if (node != NULL) {
        SCRadixReleasePrefix(node->prefix, tree);

        if (node->netmasks != NULL)
            SCFree(node->netmasks);

        SCFree(node);
    }

    return;
}

/**
 * \brief Creates a new Radix tree
 *
 * \param Free Function pointer supplied by the user to be used by the Radix
 *             cleanup API to free the user suppplied data
 *
 * \retval tree The newly created radix tree on success
 */
SCRadixTree *SCRadixCreateRadixTree(void (*Free)(void*), void (*PrintData)(void*))
{
    SCRadixTree *tree = NULL;

    if ( (tree = SCMalloc(sizeof(SCRadixTree))) == NULL) {
        SCLogError(SC_ERR_FATAL, "Fatal error encountered in SCRadixCreateRadixTree. Exiting...");
        exit(EXIT_FAILURE);
    }
    memset(tree, 0, sizeof(SCRadixTree));

    tree->Free = Free;
    tree->PrintData = PrintData;

    return tree;
}

/**
 * \brief Internal helper function used by SCRadixReleaseRadixTree to free a
 *        subtree
 *
 * \param node Pointer to the root of the subtree that has to be freed
 * \param tree Pointer to the Radix tree to which this subtree belongs
 */
static void SCRadixReleaseRadixSubtree(SCRadixNode *node, SCRadixTree *tree)
{
    if (node != NULL) {
        SCRadixReleaseRadixSubtree(node->left, tree);
        SCRadixReleaseRadixSubtree(node->right, tree);
        SCRadixReleaseNode(node, tree);
    }

    return;
}

/**
 * \brief Frees a Radix tree and all its nodes
 *
 * \param tree Pointer to the Radix tree that has to be freed
 */
void SCRadixReleaseRadixTree(SCRadixTree *tree)
{
    if (tree == NULL)
        return;

    SCRadixReleaseRadixSubtree(tree->head, tree);
    tree->head = NULL;

    return;
}

/**
 * \brief Adds a key to the Radix tree.  Used internally by the API.
 *
 * \param key_stream Data that has to added to the Radix tree
 * \param key_bitlen The bitlen of the the above stream.  For example if the
 *                   stream is the string "abcd", the bitlen would be 32.  If
 *                   the stream is an IPV6 address bitlen would be 128
 * \param tree       Pointer to the Radix tree
 * \param user       Pointer to the user data that has to be associated with
 *                   this key
 * \param netmask    The netmask (cidr) if we are adding an IP netblock; 255
 *                   if we are not adding an IP netblock
 *
 * \retval node Pointer to the newly created node
 */
static SCRadixNode *SCRadixAddKey(uint8_t *key_stream, uint16_t key_bitlen,
                                  SCRadixTree *tree, void *user, uint8_t netmask)
{
    SCRadixNode *node = NULL;
    SCRadixNode *new_node = NULL;
    SCRadixNode *parent = NULL;
    SCRadixNode *inter_node = NULL;
    SCRadixNode *bottom_node = NULL;

    SCRadixPrefix *prefix = NULL;

    uint8_t *stream = NULL;
    uint8_t bitlen = 0;

    int check_bit = 0;
    int differ_bit = 0;

    int i = 0;
    int j = 0;
    int temp = 0;

    if (tree == NULL) {
        SCLogError(SC_ERR_INVALID_ARGUMENT, "Argument \"tree\" NULL");
        return NULL;
    }

    /* chop the ip address against a netmask */
    SCRadixChopIPAddressAgainstNetmask(key_stream, netmask, key_bitlen);

    if ( (prefix = SCRadixCreatePrefix(key_stream, key_bitlen, user,
                                       netmask)) == NULL) {
        SCLogError(SC_ERR_RADIX_TREE_GENERIC, "Error creating prefix");
        return NULL;
    }

    /* the very first element in the radix tree */
    if (tree->head == NULL) {
        node = SCRadixCreateNode();
        node->prefix = prefix;
        node->bit = prefix->bitlen;
        tree->head = node;
        if (netmask == 255 || (netmask == 32 && key_bitlen == 32) || (netmask == 128 && key_bitlen == 128))
            return node;

        /* if we have reached here, we are actually having a proper netblock in
         * our hand(i.e. < 32 for ipv4 and < 128 for ipv6).  Add the netmask for
         * this node.  The reason we add netmasks other than 32 and 128, is
         * because we need those netmasks in case of searches for ips contained
         * in netblocks.  If the netmask is 32 or 128, either ways we will be
         * having an exact match for that ip value.  If it is not, we start
         * chopping the incoming search ip key using the netmask values added
         * into the tree and then verify for a match */
        node->netmask_cnt++;
        if ( (node->netmasks = SCRealloc(node->netmasks, (node->netmask_cnt *
                                                        sizeof(uint8_t)))) == NULL) {
            SCLogError(SC_ERR_FATAL, "Fatal error encountered in SCRadixAddKey. Exiting...");
            exit(EXIT_FAILURE);
        }
        node->netmasks[0] = netmask;
        return node;
    }

    node = tree->head;
    stream = prefix->stream;
    bitlen = prefix->bitlen;

    /* we walk down the tree only when we satisfy 2 conditions.  The first one
     * being the incoming prefix is shorter than the differ bit of the current
     * node.  In case we fail in this aspect, we walk down to the tree, till we
     * arrive at a node that ends in a prefix */
    while (node->bit < bitlen || node->prefix == NULL) {
        /* if the bitlen isn't long enough to handle the bit test, we just walk
         * down along one of the paths, since either paths should end up with a
         * node that has a common prefix whose differ bit is greater than the
         * bitlen of the incoming prefix */
        if (bitlen < node->bit) {
            if (node->right == NULL)
                break;
            node = node->right;
        } else {
            if (SC_RADIX_BITTEST(stream[node->bit >> 3],
                                 (0x80 >> (node->bit % 8))) ) {
                if (node->right == NULL)
                    break;
                node = node->right;
            } else {
                if (node->left == NULL)
                    break;
                node = node->left;
            }
        }
    }

    /* we need to keep a reference to the bottom-most node, that actually holds
     * the prefix */
    bottom_node = node;

    /* get the first bit position where the ips differ */
    check_bit = (node->bit < bitlen)? node->bit: bitlen;
    for (i = 0; (i * 8) < check_bit; i++) {
        if ((temp = (stream[i] ^ bottom_node->prefix->stream[i])) == 0) {
            differ_bit = (i + 1) * 8;
            continue;
        }

        /* find out the position where the first bit differs.  This method is
         * faster, but at the cost of being larger.  But with larger caches
         * these days we don't have to worry about cache misses */
        temp = temp * 2;
        if (temp >= 256)
            j = 0;
        else if (temp >= 128)
            j = 1;
        else if (temp >= 64)
            j = 2;
        else if (temp >= 32)
            j = 3;
        else if (temp >= 16)
            j = 4;
        else if (temp >= 8)
            j = 5;
        else if (temp >= 4)
            j = 6;
        else if (temp >= 2)
            j = 7;

        differ_bit = i * 8 + j;
        break;
    }
    if (check_bit < differ_bit)
        differ_bit = check_bit;

    /* walk up the tree till we find the position, to fit our new node in */
    parent = node->parent;
    while (parent && differ_bit <= parent->bit) {
        node = parent;
        parent = node->parent;
    }

    /* We already have the node in the tree with the same differing bit pstn */
    if (differ_bit == bitlen && node->bit == bitlen) {
        if (node->prefix != NULL) {
            /* Check if we already have this netmask entry covered by this prefix */
            if (SCRadixPrefixContainNetmask(node->prefix, netmask)) {
                /* Basically we already have this stream prefix, as well as the
                 * netblock entry for this.  A perfect duplicate. */
                SCLogDebug("Duplicate entry for this ip address/netblock");
            } else {
                /* Basically we already have this stream prefix, but we don't
                 * have an entry for this particular netmask value for this
                 * prefix.  For example, we have an entry for 192.168.0.0 and
                 * 192.168.0.0/16 and now we are trying to enter 192.168.0.0/20 */
                SCRadixAddNetmaskUserDataToPrefix(node->prefix, netmask, user);

                /* if we are adding a netmask of 32(for ipv4) or 128(for ipv6)
                 * it indicates we are adding an exact host ip into the radix
                 * tree, in which case we don't need to add the netmask value
                 * into the tree */
                if (netmask == 255 || (netmask == 32 && bitlen == 32) || (netmask == 128 && bitlen == 128))
                    return node;

                /* looks like we have a netmask which is != 32 or 128, in which
                 * case we walk up the tree to insert this netmask value in the
                 * correct node */
                parent = node->parent;
                while (parent != NULL && netmask < (parent->bit + 1)) {
                    node = parent;
                    parent = parent->parent;
                }

                node->netmask_cnt++;
                if ( (node->netmasks = SCRealloc(node->netmasks, (node->netmask_cnt *
                                                                sizeof(uint8_t)))) == NULL) {
                    SCLogError(SC_ERR_FATAL, "Fatal error encountered in SCRadixAddKey. Exiting...");
                    exit(EXIT_FAILURE);
                }

                if (node->netmask_cnt == 1) {
                    node->netmasks[0] = netmask;
                    return new_node;
                }

                node->netmasks[node->netmask_cnt - 1] = netmask;

                for (i = node->netmask_cnt - 2; i >= 0; i--) {
                    if (netmask < node->netmasks[i]) {
                        node->netmasks[i + 1] = netmask;
                        break;
                    }

                    node->netmasks[i + 1] = node->netmasks[i];
                    node->netmasks[i] = netmask;
                }
            }
        } else {
            node->prefix = SCRadixCreatePrefix(prefix->stream, prefix->bitlen,
                                               user, 255);
        }
        return node;
    }

    /* create the leaf node for the new key */
    new_node = SCRadixCreateNode();
    new_node->prefix = prefix;
    new_node->bit = prefix->bitlen;

    /* indicates that we have got a key that has length that is already covered
     * by a prefix of some other key in the tree.  We create a new intermediate
     * node with a single child and stick it in.  We need the if only in the
     * case of variable length keys */
    if (differ_bit == bitlen) {
        if (SC_RADIX_BITTEST(bottom_node->prefix->stream[differ_bit >> 3],
                             (0x80 >> (differ_bit % 8))) ) {
            new_node->right = node;
        } else {
            new_node->left = node;
        }
        new_node->parent = node->parent;

        if (node->parent == NULL)
            tree->head = new_node;
        else if (node->parent->right == node)
            node->parent->right = new_node;
        else
            node->parent->left = new_node;

        node->parent = new_node;
        /* stick our new_node into the tree.  Create a node that holds the
         * differing bit position and break the branch.  Also handle the
         * tranfer of netmasks between node and inter_node(explained in more
         * detail below) */
    } else {
        inter_node = SCRadixCreateNode();
        inter_node->prefix = NULL;
        inter_node->bit = differ_bit;
        inter_node->parent = node->parent;

        if (node->netmasks != NULL) {
            for (i = 0; i < node->netmask_cnt; i++) {
                if (node->netmasks[i] < differ_bit + 1)
                    break;
            }

            if ( (inter_node->netmasks = SCMalloc((node->netmask_cnt - i) *
                                                sizeof(uint8_t))) == NULL) {
                SCLogError(SC_ERR_FATAL, "Fatal error encountered in SCRadixAddKey. Exiting...");
                exit(EXIT_FAILURE);
            }

            for (j = 0; j < (node->netmask_cnt - i); j++)
                inter_node->netmasks[j] = node->netmasks[i + j];

            inter_node->netmask_cnt = (node->netmask_cnt - i);
            node->netmask_cnt = i;

            if (node->netmask_cnt == 0) {
                SCFree(node->netmasks);
                node->netmasks = NULL;
            }
        }

        if (SC_RADIX_BITTEST(stream[differ_bit >> 3],
                             (0x80 >> (differ_bit % 8))) ) {
            inter_node->left = node;
            inter_node->right = new_node;
        } else {
            inter_node->left = new_node;
            inter_node->right = node;
        }
        new_node->parent = inter_node;

        if (node->parent == NULL)
            tree->head = inter_node;
        else if (node->parent->right == node)
            node->parent->right = inter_node;
        else
            node->parent->left = inter_node;

        node->parent = inter_node;
    }

    /* insert the netmask into the tree */
    if (netmask != 255 && (netmask != 32 || (netmask == 32 && bitlen != 32)) && netmask != 128) {
        node = new_node;
        parent = new_node->parent;
        while (parent != NULL && netmask < (parent->bit + 1)) {
            node = parent;
            parent = parent->parent;
        }

        node->netmask_cnt++;
        if ( (node->netmasks = SCRealloc(node->netmasks, (node->netmask_cnt *
                                                        sizeof(uint8_t)))) == NULL) {
            SCLogError(SC_ERR_FATAL, "Fatal error encountered in SCRadixAddKey. Exiting...");
            exit(EXIT_FAILURE);
        }

        if (node->netmask_cnt == 1) {
            node->netmasks[0] = netmask;
            return new_node;
        }

        node->netmasks[node->netmask_cnt - 1] = netmask;

        for (i = node->netmask_cnt - 2; i >= 0; i--) {
            if (netmask < node->netmasks[i]) {
                node->netmasks[i + 1] = netmask;
                break;
            }

            node->netmasks[i + 1] = node->netmasks[i];
            node->netmasks[i] = netmask;
        }
    }

    return new_node;
}

/**
 * \brief Adds a new generic key to the Radix tree
 *
 * \param key_stream Data that has to be added to the Radix tree
 * \param key_bitlen The bitlen of the the above stream.  For example if the
 *                   stream is the string "abcd", the bitlen would be 32
 * \param tree       Pointer to the Radix tree
 * \param user       Pointer to the user data that has to be associated with the
 *                   key
 *
 * \retval node Pointer to the newly created node
 */
SCRadixNode *SCRadixAddKeyGeneric(uint8_t *key_stream, uint16_t key_bitlen,
                                  SCRadixTree *tree, void *user)
{
    SCRadixNode *node = SCRadixAddKey(key_stream, key_bitlen, tree, user, 255);

    return node;
}

/**
 * \brief Adds a new IPV4 address to the Radix tree
 *
 * \param key_stream Data that has to be added to the Radix tree.  In this case
 *                   a pointer to an IPV4 address
 * \param tree       Pointer to the Radix tree
 * \param user       Pointer to the user data that has to be associated with the
 *                   key
 *
 * \retval node Pointer to the newly created node
 */
SCRadixNode *SCRadixAddKeyIPV4(uint8_t *key_stream, SCRadixTree *tree,
                               void *user)
{
    SCRadixNode *node = SCRadixAddKey(key_stream, 32, tree, user, 32);

    return node;
}

/**
 * \brief Adds a new IPV6 address to the Radix tree
 *
 * \param key_stream Data that has to be added to the Radix tree.  In this case
 *                   the pointer to an IPV6 address
 * \param tree       Pointer to the Radix tree
 * \param user       Pointer to the user data that has to be associated with the
 *                   key
 *
 * \retval node Pointer to the newly created node
 */
SCRadixNode *SCRadixAddKeyIPV6(uint8_t *key_stream, SCRadixTree *tree,
                               void *user)
{
    SCRadixNode *node = SCRadixAddKey(key_stream, 128, tree, user, 128);

    return node;
}

/**
 * \brief Adds a new IPV4 netblock to the Radix tree
 *
 * \param key_stream Data that has to be added to the Radix tree.  In this case
 *                   a pointer to an IPV4 netblock
 * \param tree       Pointer to the Radix tree
 * \param user       Pointer to the user data that has to be associated with the
 *                   key
 * \param netmask    The netmask (cidr) if we are adding a netblock
 *
 * \retval node Pointer to the newly created node
 */
SCRadixNode *SCRadixAddKeyIPV4Netblock(uint8_t *key_stream, SCRadixTree *tree,
                                       void *user, uint8_t netmask)
{
    SCRadixNode *node = SCRadixAddKey(key_stream, 32, tree, user, netmask);

    return node;
}

/**
 * \brief Adds a new IPV6 netblock to the Radix tree
 *
 * \param key_stream Data that has to be added to the Radix tree.  In this case
 *                   a pointer to an IPV6 netblock
 * \param tree       Pointer to the Radix tree
 * \param user       Pointer to the user data that has to be associated with the
 *                   key
 * \param netmask    The netmask (cidr) if we are adding a netblock
 *
 * \retval node Pointer to the newly created node
 */
SCRadixNode *SCRadixAddKeyIPV6Netblock(uint8_t *key_stream, SCRadixTree *tree,
                                       void *user, uint8_t netmask)
{
    SCRadixNode *node = SCRadixAddKey(key_stream, 128, tree, user, netmask);

    return node;
}

/**
 * \brief Adds a new IPV4/netblock to the Radix tree from a string
 *
 * \param str        IPV4 string with optional /cidr netmask
 * \param tree       Pointer to the Radix tree
 * \param user       Pointer to the user data that has to be associated with
 *                   the key
 *
 * \retval node Pointer to the newly created node
 */
SCRadixNode *SCRadixAddKeyIPV4String(const char *str, SCRadixTree *tree, void *user) {
    uint32_t ip;
    uint8_t netmask = 32;
    char ip_str[32]; /* Max length for full ipv4/mask string with NUL */
    char *mask_str = NULL;
    struct in_addr addr;

    /* Make a copy of the string so it can be modified */
    strlcpy(ip_str, str, sizeof(ip_str) - 2);
    *(ip_str + (sizeof(ip_str) - 1)) = '\0';

    /* Does it have a mask? */
    if (NULL != (mask_str = strchr(ip_str, '/'))) {
        int cidr;
        *(mask_str++) = '\0';

        /* Dotted type netmask not supported (yet) */
        if (strchr(mask_str, '.') != NULL) {
            return NULL;
        }

        /* Get binary values for cidr mask */
        cidr = atoi(mask_str);
        if ((cidr < 0) || (cidr > 32)) {
            return NULL;
        }
        netmask = (uint8_t)cidr;
    }

    /* Validate the IP */
    int r;
    if ((r = inet_pton(AF_INET, ip_str, &addr)) <= 0) {
        return NULL;
    }
    ip = addr.s_addr;

    return SCRadixAddKey((uint8_t *)&ip, 32, tree, user, netmask);
}

/**
 * \brief Adds a new IPV6/netblock to the Radix tree from a string
 *
 * \param str        IPV6 string with optional /cidr netmask
 * \param tree       Pointer to the Radix tree
 * \param user       Pointer to the user data that has to be associated with
 *                   the key
 *
 * \retval node Pointer to the newly created node
 */
SCRadixNode *SCRadixAddKeyIPV6String(const char *str, SCRadixTree *tree, void *user) {
    uint8_t netmask = 128;
    char ip_str[80]; /* Max length for full ipv6/mask string with NUL */
    char *mask_str = NULL;
    struct in6_addr addr;

    /* Make a copy of the string so it can be modified */
    strlcpy(ip_str, str, sizeof(ip_str) - 2);
    *(ip_str + sizeof(ip_str) - 1) = '\0';

    /* Does it have a mask? */
    if (NULL != (mask_str = strchr(ip_str, '/'))) {
        int cidr;
        *(mask_str++) = '\0';

        /* Dotted type netmask not supported (yet) */
        if (strchr(mask_str, '.') != NULL) {
            return NULL;
        }

        /* Get binary values for cidr mask */
        cidr = atoi(mask_str);
        if ((cidr < 0) || (cidr > 32)) {
            return NULL;
        }
        netmask = (uint8_t)cidr;
    }

    /* Validate the IP */
    if (inet_pton(AF_INET6, ip_str, &addr) <= 0) {
        return NULL;
    }

    return SCRadixAddKey(addr.s6_addr, 128, tree, user, netmask);
}

static void SCRadixTransferNetmasksBWNodes(SCRadixNode *dest, SCRadixNode *src)
{
    int i = 0, j = 0;

    if (src == NULL || dest == NULL) {
        SCLogError(SC_ERR_INVALID_ARGUMENTS, "src or dest NULL");
        return;
    }

    /* no netmasks in the source node, to transfer to the destination node */
    if (src->netmasks == NULL)
        return;

    if ( (dest->netmasks = SCRealloc(dest->netmasks,
                                   (src->netmask_cnt + dest->netmask_cnt) *
                                   sizeof(uint8_t))) == NULL)
        return;

    for (i = dest->netmask_cnt, j = 0; j < src->netmask_cnt; i++, j++)
        dest->netmasks[i] = src->netmasks[j];

    return;
}

/**
 * \brief Removes a netblock entry from an ip node.  The function first
 *        deletes the netblock/user_data entry for the prefix and then
 *        removes the netmask entry that has been made in the tree, by
 *        walking up the tree and deleting the entry from the specific node.
 *
 * \param node    The node from which the netblock entry has to be removed.
 * \param netmask The netmask entry (cidr) that has to be removed.
 */
static void SCRadixRemoveNetblockEntry(SCRadixNode *node, uint8_t netmask)
{
    SCRadixNode *parent = NULL;
    SCRadixNode *local_node = node;
    int i = 0;

    if (node == NULL) {
        SCLogError(SC_ERR_INVALID_ARGUMENTS, "Invalid argument.  Node is NULL");
        return;
    }

    SCRadixRemoveNetmaskUserDataFromPrefix(node->prefix, netmask);

    if (netmask == 32 || netmask == 128)
        return;

    local_node = node;
    parent = local_node->parent;
    while (parent != NULL && netmask < (parent->bit + 1)) {
        local_node = parent;
        parent = parent->parent;
    }

    for (i = 0; i < node->netmask_cnt; i++) {
        if (node->netmasks[i] == netmask)
            break;
    }

    if (i == node->netmask_cnt) {
        SCLogDebug("Something's wrong with the tree.  We are unable to find the "
                   "netmask entry");
        return;
    }

    for ( ; i < node->netmask_cnt - 1; i++)
        node->netmasks[i] = node->netmasks[i + 1];

    node->netmask_cnt--;
    if (node->netmask_cnt == 0) {
        SCFree(node->netmasks);
        node->netmasks = NULL;
        return;
    }

    node->netmasks = SCRealloc(node->netmasks, node->netmask_cnt * sizeof(uint8_t));
    if (node->netmasks == NULL)
        return;

    return;
}

/**
 * \brief Removes a key from the Radix tree
 *
 * \param key_stream Data that has to be removed from the Radix tree
 * \param key_bitlen The bitlen of the the above stream.  For example if the
 *                   stream holds an IPV4 address(4 bytes), bitlen would be 32
 * \param tree       Pointer to the Radix tree from which the key has to be
 *                   removed
 */
static void SCRadixRemoveKey(uint8_t *key_stream, uint16_t key_bitlen,
                             SCRadixTree *tree, uint8_t netmask)
{
    SCRadixNode *node = tree->head;
    SCRadixNode *parent = NULL;
    SCRadixNode *temp_dest = NULL;

    SCRadixPrefix *prefix = NULL;

    int mask = 0;
    int i = 0;

    if (node == NULL)
        return;

    if ( (prefix = SCRadixCreatePrefix(key_stream, key_bitlen, NULL, 255)) == NULL)
        return;

    while (node->bit < prefix->bitlen) {
        if (SC_RADIX_BITTEST(prefix->stream[node->bit >> 3],
                             (0x80 >> (node->bit % 8))) ) {
            node = node->right;
        } else {
            node = node->left;
        }

        if (node == NULL) {
            SCRadixReleasePrefix(prefix, tree);
            return;
        }
    }

    if (node->bit != prefix->bitlen || node->prefix == NULL) {
        SCRadixReleasePrefix(prefix, tree);
        return;
    }

    i = prefix->bitlen / 8;
    if (memcmp(node->prefix->stream, prefix->stream, i) == 0) {
        mask = -1 << (8 - prefix->bitlen % 8);

        if (prefix->bitlen % 8 == 0 ||
            (node->prefix->stream[i] & mask) == (prefix->stream[i] & mask)) {
            if (!SCRadixPrefixContainNetmask(node->prefix, netmask)) {
                SCLogDebug("The ip key exists in the Radix Tree, but this(%d) "
                           "netblock entry doesn't exist", netmask);
                SCRadixReleasePrefix(prefix, tree);
                return;
            }
        } else {
            SCLogDebug("You are trying to remove a key that doesn't exist in "
                       "the Radix Tree");
            SCRadixReleasePrefix(prefix, tree);
            return;
        }
    } else {
        SCLogDebug("You are trying to remove a key that doesn't exist in the "
                   "Radix Tree");
        SCRadixReleasePrefix(prefix, tree);
        return;
    }

    /* The ip node does exist, and the netblock entry does exist in this node, if
     * we have reached this point.  If we have more than one netblock entry, it
     * indicates we have multiple entries for this key.  So we delete that
     * particular netblock entry, and make our way out of this function */
    if (SCRadixPrefixNetmaskCount(node->prefix) > 1) {
        SCRadixRemoveNetblockEntry(node, netmask);
        SCRadixReleasePrefix(prefix, tree);
        return;
    }

    /* we are deleting the root of the tree.  This would be the only node left
     * in the tree */
    if (tree->head == node) {
        SCFree(node);
        tree->head = NULL;
        SCRadixReleasePrefix(prefix, tree);
        return;
    }

    parent = node->parent;
    /* parent->parent is not the root of the tree */
    if (parent->parent != NULL) {
        if (parent->parent->left == parent) {
            if (node->parent->left == node) {
                temp_dest = parent->right;
                parent->parent->left = parent->right;
                parent->right->parent = parent->parent;
            } else {
                temp_dest = parent->left;
                parent->parent->left = parent->left;
                parent->left->parent = parent->parent;
            }
        } else {
            if (node->parent->left == node) {
                temp_dest = parent->right;
                parent->parent->right = parent->right;
                parent->right->parent = parent->parent;
            } else {
                temp_dest = parent->left;
                parent->parent->right = parent->left;
                parent->left->parent = parent->parent;
            }
        }
        /* parent is the root of the tree */
    } else {
        if (parent->left == node) {
            temp_dest = tree->head->right;
            tree->head->right->parent = NULL;
            tree->head = tree->head->right;
        } else {
            temp_dest = tree->head->left;
            tree->head->left->parent = NULL;
            tree->head = tree->head->left;
        }
    }
    /* We need to shift the netmask entries from the node that would be
     * deleted to its immediate descendant */
    SCRadixTransferNetmasksBWNodes(temp_dest, parent);
    /* release the nodes */
    SCRadixReleaseNode(parent, tree);
    SCRadixReleaseNode(node, tree);
    SCRadixReleasePrefix(prefix, tree);

    return;
}

/**
 * \brief Removes a key from the Radix tree
 *
 * \param key_stream Data that has to be removed from the Radix tree
 * \param key_bitlen The bitlen of the the above stream.
 * \param tree       Pointer to the Radix tree from which the key has to be
 *                   removed
 */
void SCRadixRemoveKeyGeneric(uint8_t *key_stream, uint16_t key_bitlen,
                             SCRadixTree *tree)
{
    return SCRadixRemoveKey(key_stream, key_bitlen, tree, 255);
}

/**
 * \brief Removes an IPV4 address netblock key from the Radix tree.
 *
 * \param key_stream Data that has to be removed from the Radix tree.  In this
 *                   case an IPV4 address
 * \param tree       Pointer to the Radix tree from which the key has to be
 *                   removed
 */
void SCRadixRemoveKeyIPV4Netblock(uint8_t *key_stream, SCRadixTree *tree,
                                  uint8_t netmask)
{
    return SCRadixRemoveKey(key_stream, 32, tree, netmask);
}

/**
 * \brief Removes an IPV4 address key(not a netblock) from the Radix tree.
 *        Instead of using this function, we can also used
 *        SCRadixRemoveKeyIPV4Netblock(), by supplying a netmask value of 32.
 *
 * \param key_stream Data that has to be removed from the Radix tree.  In this
 *                   case an IPV4 address
 * \param tree       Pointer to the Radix tree from which the key has to be
 *                   removed
 */
void SCRadixRemoveKeyIPV4(uint8_t *key_stream, SCRadixTree *tree)
{
    return SCRadixRemoveKey(key_stream, 32, tree, 32);
}

/**
 * \brief Removes an IPV6 netblock address key from the Radix tree.
 *
 * \param key_stream Data that has to be removed from the Radix tree.  In this
 *                   case an IPV6 address
 * \param tree       Pointer to the Radix tree from which the key has to be
 *                   removed
 */
void SCRadixRemoveKeyIPV6Netblock(uint8_t *key_stream, SCRadixTree *tree,
                                  uint8_t netmask)
{
    return SCRadixRemoveKey(key_stream, 128, tree, netmask);
}

/**
 * \brief Removes an IPV6 address key(not a netblock) from the Radix tree.
 *        Instead of using this function, we can also used
 *        SCRadixRemoveKeyIPV6Netblock(), by supplying a netmask value of 128.
 *
 * \param key_stream Data that has to be removed from the Radix tree.  In this
 *                   case an IPV6 address
 * \param tree       Pointer to the Radix tree from which the key has to be
 *                   removed
 */
void SCRadixRemoveKeyIPV6(uint8_t *key_stream, SCRadixTree *tree)
{
    return SCRadixRemoveKey(key_stream, 128, tree, 128);
}

/**
 * \brief Checks if an IP prefix falls under a netblock, in the path to the root
 *        of the tree, from the node.  Used internally by SCRadixFindKey()
 *
 * \param prefix Pointer to the prefix that contains the ip address
 * \param node   Pointer to the node from where we have to climb the tree
 */
static inline SCRadixNode *SCRadixFindKeyIPNetblock(uint8_t *key_stream, uint8_t key_bitlen,
                                                    SCRadixNode *node)
{
    SCRadixNode *netmask_node = NULL;
    int mask = 0;
    int bytes = 0;
    int i = 0;
    int j = 0;

    while (node != NULL && node->netmasks == NULL)
        node = node->parent;

    if (node == NULL)
        return NULL;
    /* hold the node found containing a netmask.  We will need it when we call
     * this function recursively */
    netmask_node = node;

    for (j = 0; j < netmask_node->netmask_cnt; j++) {
        bytes = key_bitlen / 8;
        for (i = 0; i < bytes; i++) {
            mask = -1;
            if ( ((i + 1) * 8) > netmask_node->netmasks[j]) {
                if ( ((i + 1) * 8 - netmask_node->netmasks[j]) < 8)
                    mask = -1 << ((i + 1) * 8 - netmask_node->netmasks[j]);
                else
                    mask = 0;
            }
            key_stream[i] &= mask;
        }

        while (node->bit < key_bitlen) {
            if (SC_RADIX_BITTEST(key_stream[node->bit >> 3],
                                 (0x80 >> (node->bit % 8))) ) {
                node = node->right;
            } else {
                node = node->left;
            }

            if (node == NULL)
                return NULL;
        }

        if (node->bit != key_bitlen || node->prefix == NULL)
            return NULL;

        if (memcmp(node->prefix->stream, key_stream, bytes) == 0) {
            mask = -1 << (8 - key_bitlen % 8);

            if (key_bitlen % 8 == 0 ||
                (node->prefix->stream[bytes] & mask) == (key_stream[bytes] & mask)) {
                if (SCRadixPrefixContainNetmaskAndSetUserData(node->prefix, netmask_node->netmasks[j], 0))
                    return node;
            }
        }
    }

    return SCRadixFindKeyIPNetblock(key_stream, key_bitlen, netmask_node->parent);
}

/**
 * \brief Checks if an IP address key is present in the tree.  The function
 *        apart from handling any normal data, also handles ipv4/ipv6 netblocks
 *
 * \param key_stream  Data that has to be found in the Radix tree
 * \param key_bitlen  The bitlen of the above stream.
 * \param tree        Pointer to the Radix tree
 * \param exact_match The key to be searched is an ip address
 */
static SCRadixNode *SCRadixFindKey(uint8_t *key_stream, uint16_t key_bitlen,
                                   SCRadixTree *tree, int exact_match)
{
    if (tree == NULL || tree->head == NULL)
        return NULL;

    SCRadixNode *node = tree->head;
    int mask = 0;
    int bytes = 0;
    uint8_t tmp_stream[255];

    if (key_bitlen > 255)
        return NULL;

    memset(tmp_stream, 0, 255);
    memcpy(tmp_stream, key_stream, key_bitlen / 8);

    while (node->bit < key_bitlen) {
        if (SC_RADIX_BITTEST(tmp_stream[node->bit >> 3],
                             (0x80 >> (node->bit % 8))) ) {
            node = node->right;
        } else {
            node = node->left;
        }

        if (node == NULL) {
            return NULL;
        }
    }

    if (node->bit != key_bitlen || node->prefix == NULL) {
        return NULL;
    }

    bytes = key_bitlen / 8;
    if (memcmp(node->prefix->stream, tmp_stream, bytes) == 0) {
        mask = -1 << (8 - key_bitlen % 8);

        if (key_bitlen % 8 == 0 ||
            (node->prefix->stream[bytes] & mask) == (tmp_stream[bytes] & mask)) {
            if (SCRadixPrefixContainNetmaskAndSetUserData(node->prefix, key_bitlen, 1)) {
                return node;
            }
        }
    }

    /* if you are not an ip key, get out of here */
    if (exact_match) {
        return NULL;
    }

    SCRadixNode *ret = SCRadixFindKeyIPNetblock(tmp_stream, key_bitlen, node);
    return ret;
}

/**
 * \brief Checks if a key is present in the tree
 *
 * \param key_stream Data that has to be found in the Radix tree
 * \param key_bitlen The bitlen of the the above stream.
 * \param tree       Pointer to the Radix tree instance
 */
SCRadixNode *SCRadixFindKeyGeneric(uint8_t *key_stream, uint16_t key_bitlen,
                                   SCRadixTree *tree)
{
    return SCRadixFindKey(key_stream, key_bitlen, tree, 1);
}

/**
 * \brief Checks if an IPV4 address is present in the tree
 *
 * \param key_stream Data that has to be found in the Radix tree.  In this case
 *                   an IPV4 address
 * \param tree       Pointer to the Radix tree instance
 */
SCRadixNode *SCRadixFindKeyIPV4ExactMatch(uint8_t *key_stream, SCRadixTree *tree)
{
    return SCRadixFindKey(key_stream, 32, tree, 1);
}

/**
 * \brief Checks if an IPV4 address is present in the tree under a netblock
 *
 * \param key_stream Data that has to be found in the Radix tree.  In this case
 *                   an IPV4 address
 * \param tree       Pointer to the Radix tree instance
 */
SCRadixNode *SCRadixFindKeyIPV4BestMatch(uint8_t *key_stream, SCRadixTree *tree)
{
    return SCRadixFindKey(key_stream, 32, tree, 0);
}

/**
 * \brief Checks if an IPV4 Netblock address is present in the tree
 *
 * \param key_stream Data that has to be found in the Radix tree.  In this case
 *                   an IPV4  netblock address
 * \param tree       Pointer to the Radix tree instance
 */
SCRadixNode *SCRadixFindKeyIPV4Netblock(uint8_t *key_stream, SCRadixTree *tree,
                                        uint8_t netmask)
{
    SCRadixNode *node = NULL;
    node = SCRadixFindKey(key_stream, 32, tree, 0);
    if (node == NULL)
        return node;

    if (SCRadixPrefixContainNetmaskAndSetUserData(node->prefix, netmask, 1))
        return node;
    else
        return NULL;
}

/**
 * \brief Checks if an IPV6 Netblock address is present in the tree
 *
 * \param key_stream Data that has to be found in the Radix tree.  In this case
 *                   an IPV6  netblock address
 * \param tree       Pointer to the Radix tree instance
 */
SCRadixNode *SCRadixFindKeyIPV6Netblock(uint8_t *key_stream, SCRadixTree *tree,
                                        uint8_t netmask)
{
    SCRadixNode *node = NULL;
    node = SCRadixFindKey(key_stream, 128, tree, 0);
    if (node == NULL)
        return node;

    if (SCRadixPrefixContainNetmaskAndSetUserData(node->prefix, (uint16_t)netmask, 1))
        return node;
    else
        return NULL;
}

/**
 * \brief Checks if an IPV6 address is present in the tree
 *
 * \param key_stream Data that has to be found in the Radix tree.  In this case
 *                   an IPV6 address
 * \param tree       Pointer to the Radix tree instance
 */
SCRadixNode *SCRadixFindKeyIPV6ExactMatch(uint8_t *key_stream, SCRadixTree *tree)
{
    return SCRadixFindKey(key_stream, 128, tree, 1);
}

/**
 * \brief Checks if an IPV6 address is present in the tree under a netblock
 *
 * \param key_stream Data that has to be found in the Radix tree.  In this case
 *                   an IPV6 address
 * \param tree       Pointer to the Radix tree instance
 */
SCRadixNode *SCRadixFindKeyIPV6BestMatch(uint8_t *key_stream, SCRadixTree *tree)
{
    return SCRadixFindKey(key_stream, 128, tree, 0);
}

/**
 * \brief Prints the node information from a Radix tree
 *
 * \param node  Pointer to the Radix node whose information has to be printed
 * \param level Used for indentation purposes
 */
void SCRadixPrintNodeInfo(SCRadixNode *node, int level,  void (*PrintData)(void*))
{
    int i = 0;

    if (node == NULL)
        return;

    for (i = 0; i < level; i++)
        printf("   ");

    printf("%d [", node->bit);

    if (node->netmasks == NULL) {
        printf("%d, ", -1);
    } else {
        for (i = 0; i < node->netmask_cnt; i++)
            printf("%s%d", (0 == i ? "" : ", "), node->netmasks[i]);
    }

    printf("] (");
    if (node->prefix != NULL) {
        for (i = 0; i * 8 < node->prefix->bitlen; i++)
            printf("%s%d", (0 == i ? "" : "."), node->prefix->stream[i]);
        printf(")\n");

        if (PrintData != NULL) {
            SCRadixUserData *ud = NULL;
            do {
                ud = node->prefix->user_data;
                printf(" [%d], ", ud->netmask);
                PrintData(ud->user);
                ud = ud->next;
            } while (ud != NULL);
        } else {
            printf("No print function provided");
        }
        printf("\n");
    } else {
        printf("NULL)\n");
    }

    return;
}

/**
 * \brief Helper function used by SCRadixPrintTree.  Prints the subtree with
 *        node as the root of the subtree
 *
 * \param node  Pointer to the node that is the root of the subtree to be printed
 * \param level Used for indentation purposes
 */
static void SCRadixPrintRadixSubtree(SCRadixNode *node, int level, void (*PrintData)(void*))
{
    if (node != NULL) {
        SCRadixPrintNodeInfo(node, level, PrintData);
        SCRadixPrintRadixSubtree(node->left, level + 1, PrintData);
        SCRadixPrintRadixSubtree(node->right, level + 1, PrintData);
    }

    return;
}

/**
 * \brief Prints the Radix Tree. While printing the radix tree we use the
 *        following format
 *
 *        Parent_0
 *            Left_Child_1
 *                Left_Child_2
 *                Right_Child_2
 *            Right_Child_1
 *                Left_Child_2
 *                Right_Child_2     and so on
 *
 *        Each node printed out holds details on the next bit that differs
 *        amongst its children, and if the node holds a prefix, the perfix is
 *        printed as well.
 *
 * \param tree Pointer to the Radix tree that has to be printed
 */
void SCRadixPrintTree(SCRadixTree *tree)
{
    printf("Printing the Radix Tree: \n");

    SCRadixPrintRadixSubtree(tree->head, 0, tree->PrintData);

    return;
}

/*------------------------------------Unit_Tests------------------------------*/

#ifdef UNITTESTS

int SCRadixTestInsertion01(void)
{
    SCRadixTree *tree = NULL;
    SCRadixNode *node[2];

    int result = 1;

    tree = SCRadixCreateRadixTree(free, NULL);


    node[0] = SCRadixAddKeyGeneric((uint8_t *)"abaa", 32, tree, NULL);
    node[1] = SCRadixAddKeyGeneric((uint8_t *)"abab", 32, tree, NULL);

    result &= (tree->head->bit == 30);
    result &= (tree->head->right == node[0]);
    result &= (tree->head->left == node[1]);

    SCRadixReleaseRadixTree(tree);

    return result;
}

int SCRadixTestInsertion02(void)
{
    SCRadixTree *tree = NULL;
    int result = 1;

    tree = SCRadixCreateRadixTree(free, NULL);

    SCRadixAddKeyGeneric((uint8_t *)"aaaaaa", 48, tree, NULL);
    SCRadixAddKeyGeneric((uint8_t *)"aaaaab", 48, tree, NULL);
    SCRadixAddKeyGeneric((uint8_t *)"aaaaaba", 56, tree, NULL);
    SCRadixAddKeyGeneric((uint8_t *)"abab", 32, tree, NULL);
    SCRadixReleaseRadixTree(tree);

    /* If we don't have a segfault till here we have succeeded :) */
    return result;
}

int SCRadixTestIPV4Insertion03(void)
{
    SCRadixTree *tree = NULL;
    struct sockaddr_in servaddr;
    int result = 1;

    tree = SCRadixCreateRadixTree(free, NULL);

    /* add the keys */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.1", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.2", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.167.1.3", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.167.1.4", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    /* add a key that already exists in the tree */
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    /* test for the existance of a key */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.6", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) == NULL);

    /* test for the existance of a key */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.167.1.4", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) != NULL);

    /* continue adding keys */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "220.168.1.2", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.5", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.18", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    /* test the existence of keys */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.3", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "127.234.2.62", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.1", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.5", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.2", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.167.1.3", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.167.1.4", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "220.168.1.2", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.18", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) != NULL);

    SCRadixReleaseRadixTree(tree);

    return result;
}

int SCRadixTestIPV4Removal04(void)
{
    SCRadixTree *tree = NULL;
    struct sockaddr_in servaddr;
    int result = 1;

    tree = SCRadixCreateRadixTree(free, NULL);

    /* add the keys */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.1", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.2", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.167.1.3", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.167.1.4", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "220.168.1.2", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.5", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.18", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    /* remove the keys from the tree */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.1", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixRemoveKeyIPV4((uint8_t *)&servaddr.sin_addr, tree);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.167.1.3", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixRemoveKeyIPV4((uint8_t *)&servaddr.sin_addr, tree);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.167.1.4", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixRemoveKeyIPV4((uint8_t *)&servaddr.sin_addr, tree);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.18", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixRemoveKeyIPV4((uint8_t *)&servaddr.sin_addr, tree);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.167.1.1", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.2", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.167.1.3", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixRemoveKeyIPV4((uint8_t *)&servaddr.sin_addr, tree);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "220.168.1.2", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixRemoveKeyIPV4((uint8_t *)&servaddr.sin_addr, tree);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.5", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.2", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.2", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixRemoveKeyIPV4((uint8_t *)&servaddr.sin_addr, tree);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.5", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixRemoveKeyIPV4((uint8_t *)&servaddr.sin_addr, tree);

    result &= (tree->head == NULL);

    SCRadixReleaseRadixTree(tree);

    return result;
}

int SCRadixTestCharacterInsertion05(void)
{
    SCRadixTree *tree = NULL;
    int result = 1;

    tree = SCRadixCreateRadixTree(free, NULL);

    /* Let us have our team here ;-) */
    SCRadixAddKeyGeneric((uint8_t *)"Victor", 48, tree, NULL);
    SCRadixAddKeyGeneric((uint8_t *)"Matt", 32, tree, NULL);
    SCRadixAddKeyGeneric((uint8_t *)"Josh", 32, tree, NULL);
    SCRadixAddKeyGeneric((uint8_t *)"Margaret", 64, tree, NULL);
    SCRadixAddKeyGeneric((uint8_t *)"Pablo", 40, tree, NULL);
    SCRadixAddKeyGeneric((uint8_t *)"Brian", 40, tree, NULL);
    SCRadixAddKeyGeneric((uint8_t *)"Jasonish", 64, tree, NULL);
    SCRadixAddKeyGeneric((uint8_t *)"Jasonmc", 56, tree, NULL);
    SCRadixAddKeyGeneric((uint8_t *)"Nathan", 48, tree, NULL);
    SCRadixAddKeyGeneric((uint8_t *)"Anoop", 40, tree, NULL);

    result &= (SCRadixFindKeyGeneric((uint8_t *)"Victor", 48, tree) != NULL);
    result &= (SCRadixFindKeyGeneric((uint8_t *)"Matt", 32, tree) != NULL);
    result &= (SCRadixFindKeyGeneric((uint8_t *)"Josh", 32, tree) != NULL);
    result &= (SCRadixFindKeyGeneric((uint8_t *)"Margaret", 64, tree) != NULL);
    result &= (SCRadixFindKeyGeneric((uint8_t *)"Pablo", 40, tree) != NULL);
    result &= (SCRadixFindKeyGeneric((uint8_t *)"Brian", 40, tree) != NULL);
    result &= (SCRadixFindKeyGeneric((uint8_t *)"Jasonish", 64, tree) != NULL);
    result &= (SCRadixFindKeyGeneric((uint8_t *)"Jasonmc", 56, tree) != NULL);
    result &= (SCRadixFindKeyGeneric((uint8_t *)"Nathan", 48, tree) != NULL);
    result &= (SCRadixFindKeyGeneric((uint8_t *)"Anoop", 40, tree) != NULL);

    result &= (SCRadixFindKeyGeneric((uint8_t *)"bamboo", 48, tree) != NULL);
    result &= (SCRadixFindKeyGeneric((uint8_t *)"bool", 32, tree) == NULL);
    result &= (SCRadixFindKeyGeneric((uint8_t *)"meerkat", 56, tree) == NULL);
    result &= (SCRadixFindKeyGeneric((uint8_t *)"Victor", 48, tree) == NULL);

    SCRadixReleaseRadixTree(tree);

    return result;
}

int SCRadixTestCharacterRemoval06(void)
{
    SCRadixTree *tree = NULL;
    int result = 1;

    tree = SCRadixCreateRadixTree(free, NULL);

    /* Let us have our team here ;-) */
    SCRadixAddKeyGeneric((uint8_t *)"Victor", 48, tree, NULL);
    SCRadixAddKeyGeneric((uint8_t *)"Matt", 32, tree, NULL);
    SCRadixAddKeyGeneric((uint8_t *)"Josh", 32, tree, NULL);
    SCRadixAddKeyGeneric((uint8_t *)"Margaret", 64, tree, NULL);
    SCRadixAddKeyGeneric((uint8_t *)"Pablo", 40, tree, NULL);
    SCRadixAddKeyGeneric((uint8_t *)"Brian", 40, tree, NULL);
    SCRadixAddKeyGeneric((uint8_t *)"Jasonish", 64, tree, NULL);
    SCRadixAddKeyGeneric((uint8_t *)"Jasonmc", 56, tree, NULL);
    SCRadixAddKeyGeneric((uint8_t *)"Nathan", 48, tree, NULL);
    SCRadixAddKeyGeneric((uint8_t *)"Anoop", 40, tree, NULL);

    SCRadixRemoveKeyGeneric((uint8_t *)"Nathan", 48, tree);
    SCRadixRemoveKeyGeneric((uint8_t *)"Brian", 40, tree);
    SCRadixRemoveKeyGeneric((uint8_t *)"Margaret", 64, tree);

    result &= (SCRadixFindKeyGeneric((uint8_t *)"Victor", 48, tree) != NULL);
    result &= (SCRadixFindKeyGeneric((uint8_t *)"Matt", 32, tree) != NULL);
    result &= (SCRadixFindKeyGeneric((uint8_t *)"Josh", 32, tree) != NULL);
    result &= (SCRadixFindKeyGeneric((uint8_t *)"Margaret", 64, tree) == NULL);
    result &= (SCRadixFindKeyGeneric((uint8_t *)"Brian", 40, tree) == NULL);
    result &= (SCRadixFindKeyGeneric((uint8_t *)"Nathan", 48, tree) == NULL);

    SCRadixRemoveKeyGeneric((uint8_t *)"Victor", 48, tree);
    SCRadixRemoveKeyGeneric((uint8_t *)"Josh", 32, tree);
    SCRadixRemoveKeyGeneric((uint8_t *)"Jasonmc", 56, tree);
    SCRadixRemoveKeyGeneric((uint8_t *)"Matt", 32, tree);

    result &= (SCRadixFindKeyGeneric((uint8_t *)"Pablo", 40, tree) != NULL);
    result &= (SCRadixFindKeyGeneric((uint8_t *)"Jasonish", 64, tree) != NULL);
    result &= (SCRadixFindKeyGeneric((uint8_t *)"Anoop", 40, tree) != NULL);

    SCRadixRemoveKeyGeneric((uint8_t *)"Pablo", 40, tree);
    SCRadixRemoveKeyGeneric((uint8_t *)"Jasonish", 64, tree);
    SCRadixRemoveKeyGeneric((uint8_t *)"Anoop", 40, tree);

    result &= (SCRadixFindKeyGeneric((uint8_t *)"Pablo", 40, tree) == NULL);
    result &= (SCRadixFindKeyGeneric((uint8_t *)"Jasonish", 64, tree) == NULL);
    result &= (SCRadixFindKeyGeneric((uint8_t *)"Anoop", 40, tree) == NULL);

    result &= (tree->head == NULL);

    SCRadixReleaseRadixTree(tree);

    return result;
}

int SCRadixTestIPV6Insertion07(void)
{
    SCRadixTree *tree = NULL;
    struct sockaddr_in6 servaddr;
    int result = 1;

    tree = SCRadixCreateRadixTree(free, NULL);

    /* add the keys */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "2003:0BF1:5346:BDEA:7422:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "BD15:9791:5346:6223:AADB:8713:9882:2432",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "1111:A21B:6221:BDEA:BBBA::DBAA:9861",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "4444:0BF7:5346:BDEA:7422:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    /* Try to add the prefix that already exists in the tree */
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "5555:0BF1:ABCD:ADEA:7922:ABCD:9124:2375",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "DBCA:ABCD:ABCD:DBCA:1245:2342:1111:2212",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "2003:0BF1:5346:1251:7422:1112:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    /* test the existence of keys */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "2003:0BF1:5346:BDEA:7422:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "BD15:9791:5346:6223:AADB:8713:9882:2432",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "1111:A21B:6221:BDEA:BBBA::DBAA:9861",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "4444:0BF7:5346:BDEA:7422:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "DBCA:ABC2:ABCD:DBCA:1245:2342:1111:2212",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "2003:0BF5:5346:1251:7422:1112:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "5555:0BF1:ABCD:ADEA:7922:ABCD:9124:2375",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "DBCA:ABCD:ABCD:DBCA:1245:2342:1111:2212",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "2003:0BF1:5346:1251:7422:1112:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    SCRadixReleaseRadixTree(tree);

    return result;
}

int SCRadixTestIPV6Removal08(void)
{
    SCRadixTree *tree = NULL;
    struct sockaddr_in6 servaddr;
    int result = 1;

    tree = SCRadixCreateRadixTree(free, NULL);

    /* add the keys */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "2003:0BF1:5346:BDEA:7422:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "BD15:9791:5346:6223:AADB:8713:9882:2432",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "1111:A21B:6221:BDEA:BBBA::DBAA:9861",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "4444:0BF7:5346:BDEA:7422:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    /* Try to add the prefix that already exists in the tree */
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "5555:0BF1:ABCD:ADEA:7922:ABCD:9124:2375",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "DBCA:ABCD:ABCD:DBCA:1245:2342:1111:2212",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "2003:0BF1:5346:1251:7422:1112:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    /* test the existence of keys */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "2003:0BF1:5346:BDEA:7422:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "8888:0BF1:5346:BDEA:6422:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "2006:0BF1:5346:BDEA:7422:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "2003:0BF1:5346:BDEA:7422:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "BD15:9791:5346:6223:AADB:8713:9882:2432",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    /* test for existance */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "1111:A21B:6221:BDEA:BBBA::DBAA:9861",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "4444:0BF7:5346:BDEA:7422:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "5555:0BF1:ABCD:ADEA:7922:ABCD:9124:2375",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "DBCA:ABCD:ABCD:DBCA:1245:2342:1111:2212",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "2003:0BF1:5346:1251:7422:1112:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "2003:0BF1:5346:BDEA:7422:8713:DDDD:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) == NULL);

    /* remove keys */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "2003:0BF1:5346:BDEA:7422:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixRemoveKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "BD15:9791:5346:6223:AADB:8713:9882:2432",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixRemoveKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree);

    /* test for existance */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "2003:0BF1:5346:BDEA:7422:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "BD15:9791:5346:6223:AADB:8713:9882:2432",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "1111:A21B:6221:BDEA:BBBA::DBAA:9861",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "4444:0BF7:5346:BDEA:7422:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "5555:0BF1:ABCD:ADEA:7922:ABCD:9124:2375",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "DBCA:ABCD:ABCD:DBCA:1245:2342:1111:2212",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    /* remove keys */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "1111:A21B:6221:BDEA:BBBA::DBAA:9861",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixRemoveKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "4444:0BF7:5346:BDEA:7422:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixRemoveKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "5555:0BF1:ABCD:ADEA:7922:ABCD:9124:2375",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixRemoveKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "DBCA:ABCD:ABCD:DBCA:1245:2342:1111:2212",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixRemoveKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree);

    /* test for existance */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "2003:0BF1:5346:BDEA:7422:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "BD15:9791:5346:6223:AADB:8713:9882:2432",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "1111:A21B:6221:BDEA:BBBA::DBAA:9861",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "4444:0BF7:5346:BDEA:7422:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "5555:0BF1:ABCD:ADEA:7922:ABCD:9124:2375",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "DBCA:ABCD:ABCD:DBCA:1245:2342:1111:2212",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) == NULL);

    SCRadixReleaseRadixTree(tree);

    return result;
}

int SCRadixTestIPV4NetblockInsertion09(void)
{
    SCRadixTree *tree = NULL;
    struct sockaddr_in servaddr;
    int result = 1;

    tree = SCRadixCreateRadixTree(free, NULL);

    /* add the keys */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.1", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.2", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.167.1.3", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.167.1.4", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "220.168.1.2", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.5", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.18", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.0.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 16);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.128.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 24);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.192.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 18);

    if (inet_pton(AF_INET, "192.175.0.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 16);

    /* test for the existance of a key */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.6", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.170.1.6", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.128.145", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.64.6", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.191.6", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.224.6", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.174.224.6", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.175.224.6", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) != NULL);

    SCRadixReleaseRadixTree(tree);

    return result;
}

int SCRadixTestIPV4NetblockInsertion10(void)
{
    SCRadixTree *tree = NULL;
    SCRadixNode *node[2];
    struct sockaddr_in servaddr;
    int result = 1;

    tree = SCRadixCreateRadixTree(free, NULL);

    /* add the keys */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "253.192.0.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 16);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "253.192.235.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 24);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.167.0.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 16);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.167.1.4", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "220.168.0.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 16);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "253.224.1.5", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.0.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 16);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.128.0", &servaddr.sin_addr) <= 0)
        return 0;
    node[0] = SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL,
                                        24);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.128.45", &servaddr.sin_addr) <= 0)
        return 0;
    node[1] = SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.0.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 18);

    if (inet_pton(AF_INET, "192.175.0.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 16);

    /* test for the existance of a key */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.128.53", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) == node[0]);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.128.45", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) == node[1]);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.128.45", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) == node[1]);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.128.78", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) == node[0]);

    /* let us remove a netblock */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.128.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixRemoveKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, 24);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.128.78", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.127.78", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) == NULL);

    SCRadixReleaseRadixTree(tree);

    return result;
}

int SCRadixTestIPV4NetblockInsertion11(void)
{
    SCRadixTree *tree = NULL;
    SCRadixNode *node = NULL;
    struct sockaddr_in servaddr;
    int result = 1;

    tree = SCRadixCreateRadixTree(free, NULL);

    /* add the keys */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "253.192.0.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 16);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "253.192.235.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 24);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.167.0.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 16);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.167.1.4", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "220.168.0.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 16);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "253.224.1.5", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.0.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 16);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.128.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 24);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.128.45", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.0.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 18);

    if (inet_pton(AF_INET, "192.175.0.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 16);

    if (inet_pton(AF_INET, "0.0.0.0", &servaddr.sin_addr) <= 0)
        return 0;
    node = SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 0);

    /* test for the existance of a key */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.128.53", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.128.45", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.128.78", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.127.78", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) == node);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "1.1.1.1", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) == node);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.255.254.25", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) == node);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "169.255.254.25", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) == node);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "0.0.0.0", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) == node);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "253.224.1.5", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) != NULL &&
               SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) != node);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "245.63.62.121", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) != NULL &&
               SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) == node);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "253.224.1.6", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) != NULL &&
               SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) == node);

    /* remove node 0.0.0.0 */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "0.0.0.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixRemoveKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, 0);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "253.224.1.6", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.127.78", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "1.1.1.1", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.255.254.25", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "169.255.254.25", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "0.0.0.0", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) == NULL);

    SCRadixReleaseRadixTree(tree);

    return result;
}

int SCRadixTestIPV4NetblockInsertion12(void)
{
    SCRadixTree *tree = NULL;
    SCRadixNode *node[2];
    struct sockaddr_in servaddr;
    int result = 1;

    tree = SCRadixCreateRadixTree(free, NULL);

    /* add the keys */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "253.192.0.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 16);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "253.192.235.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 24);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.167.0.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 16);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.167.1.4", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "220.168.0.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 16);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "253.224.1.5", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.0.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 16);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.128.0", &servaddr.sin_addr) <= 0)
        return 0;
    node[0] = SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 24);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.128.45", &servaddr.sin_addr) <= 0)
        return 0;
    node[1] = SCRadixAddKeyIPV4((uint8_t *)&servaddr.sin_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.0.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 18);

    if (inet_pton(AF_INET, "225.175.21.228", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 32);

    /* test for the existance of a key */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.128.53", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) == node[0]);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.128.53", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.128.45", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) == node[1]);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.128.45", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) == node[1]);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.128.45", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) == node[1]);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.128.78", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) == node[0]);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.171.127.78", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "225.175.21.228", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "225.175.21.224", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "225.175.21.229", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "225.175.21.230", &servaddr.sin_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV4ExactMatch((uint8_t *)&servaddr.sin_addr, tree) == NULL);

    SCRadixReleaseRadixTree(tree);

    return result;
}

int SCRadixTestIPV6NetblockInsertion13(void)
{
    SCRadixTree *tree = NULL;
    struct sockaddr_in6 servaddr;
    int result = 1;

    tree = SCRadixCreateRadixTree(free, NULL);

    /* add the keys */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "2003:0BF1:5346:BDEA:7422:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "BD15:9791:5346:6223:AADB:8713:9882:2432",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "1111:A21B:6221:BDEA:BBBA::DBAA:9861",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "4444:0BF7:5346:BDEA:7422:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "5555:0BF1:ABCD:ADEA:7922:ABCD:9124:2375",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "DBCA:ABCD:ABCD:DB00:0000:0000:0000:0000",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6Netblock((uint8_t *)&servaddr.sin6_addr, tree, NULL, 56);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "DBCA:ABCD:ABCD:DBAA:1245:2342:1145:6241",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    /* test the existence of keys */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "2003:0BF1:5346:BDEA:7422:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "BD15:9791:5346:6223:AADB:8713:9882:2432",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "1111:A21B:6221:BDEA:BBBA::DBAA:9861",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "1111:A21B:6221:BDEA:BBBA::DBAA:9861",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6BestMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "4444:0BF7:5346:BDEA:7422:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "DBCA:ABC2:ABCD:DBCA:1245:2342:1111:2212",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "2003:0BF5:5346:1251:7422:1112:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "5555:0BF1:ABCD:ADEA:7922:ABCD:9124:2375",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "DBCA:ABCD:ABCD:DBCA:1245:2342:1111:2212",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6BestMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "DBCA:ABCD:ABCD:DBAA:1245:2342:1146:6241",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6BestMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "DBCA:ABCD:ABCD:DBAA:1245:2342:1356:1241",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6BestMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "DBCA:ABCD:ABCD:DAAA:1245:2342:1146:6241",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) == NULL);


    SCRadixReleaseRadixTree(tree);

    return result;
}

int SCRadixTestIPV6NetblockInsertion14(void)
{
    SCRadixTree *tree = NULL;
    SCRadixNode *node = NULL;
    struct sockaddr_in6 servaddr;
    int result = 1;

    tree = SCRadixCreateRadixTree(free, NULL);

    /* add the keys */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "2003:0BF1:5346:BDEA:7422:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "BD15:9791:5346:6223:AADB:8713:9882:2432",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "1111:A21B:6221:BDEA:BBBA::DBAA:9861",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "4444:0BF7:5346:BDEA:7422:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "5555:0BF1:ABCD:ADEA:7922:ABCD:9124:2375",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "DBCA:ABCD:ABCD:DB00:0000:0000:0000:0000",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6Netblock((uint8_t *)&servaddr.sin6_addr, tree, NULL, 56);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "DBCA:ABCD:ABCD:DBAA:1245:2342:1145:6241",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV6((uint8_t *)&servaddr.sin6_addr, tree, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "::", &servaddr.sin6_addr) <= 0)
        return 0;
    node = SCRadixAddKeyIPV6Netblock((uint8_t *)&servaddr.sin6_addr, tree, NULL,
                                     0);

    /* test the existence of keys */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "2004:0BF1:5346:BDEA:7422:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) == NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "2004:0BF1:5346:BDEA:7422:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6BestMatch((uint8_t *)&servaddr.sin6_addr, tree) == node);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "2004:0BF1:5346:B116:2362:8713:9124:2315",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6BestMatch((uint8_t *)&servaddr.sin6_addr, tree) == node);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "2004:0B23:3252:BDEA:7422:8713:9124:2341",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6BestMatch((uint8_t *)&servaddr.sin6_addr, tree) == node);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "DBCA:ABCD:ABCD:DBAA:1245:2342:1145:6241",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL &&
               SCRadixFindKeyIPV6ExactMatch((uint8_t *)&servaddr.sin6_addr, tree) != node);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "DBCA:ABCD:ABCD:DBAA:1245:2342:1145:6241",
                  &servaddr.sin6_addr) <= 0)
        return 0;
    result &= (SCRadixFindKeyIPV6BestMatch((uint8_t *)&servaddr.sin6_addr, tree) != NULL &&
               SCRadixFindKeyIPV6BestMatch((uint8_t *)&servaddr.sin6_addr, tree) != node);

    SCRadixReleaseRadixTree(tree);

    return result;
}

/**
 * \test Check that the best match search works for all the
 *       possible netblocks of a fixed address
 */
int SCRadixTestIPV4NetBlocksAndBestSearch15(void)
{
    SCRadixTree *tree = NULL;
    struct sockaddr_in servaddr;
    int result = 1;

    tree = SCRadixCreateRadixTree(free, NULL);

    uint32_t i = 0;

    uint32_t *user;

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.0.1", &servaddr.sin_addr) <= 0) {
        result = 0;
        goto end;
    }

    for (; i <= 32; i++) {
        user = SCMalloc(sizeof(uint32_t));
        if (user == NULL) {
            result = 0;
            goto end;
        }

        *user = i;

        SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, user, i);

        SCRadixNode *node = SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree);
        if (node == NULL) {
            printf("node == NULL: ");
            result = 0;
            goto end;
        }

        if (node->prefix->user_data_result == NULL) {
            printf("User data == NULL: ");
            result = 0;
            goto end;
        }

        if ( *( (uint32_t*)node->prefix->user_data_result) != i) {
            printf("User data == %"PRIu32"; i == %"PRIu32": ", *( (uint32_t*)node->prefix->user_data_result), i);
            result = 0;
            goto end;
        }
    }

end:
    SCRadixReleaseRadixTree(tree);

    return result;
}

/**
 * \test Check that the best match search works for all the
 *       possible netblocks of a fixed address
 */
int SCRadixTestIPV4NetBlocksAndBestSearch16(void)
{
    SCRadixTree *tree = NULL;
    struct sockaddr_in servaddr;
    int result = 1;

    tree = SCRadixCreateRadixTree(free, NULL);

    uint32_t i = 0;

    uint32_t *user;

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.1", &servaddr.sin_addr) <= 0) {
        result = 0;
        goto end;
    }

    for (; i <= 32; i++) {
        user = SCMalloc(sizeof(uint32_t));
        if (user == NULL) {
            result = 0;
            goto end;
        }

        *user = i;

        SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, user, i);

        SCRadixNode *node = SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree);
        if (node == NULL) {
            printf("node == NULL: ");
            result = 0;
            goto end;
        }

        if (node->prefix->user_data_result == NULL) {
            printf("User data == NULL: ");
            result = 0;
            goto end;
        }

        if ( *( (uint32_t*)node->prefix->user_data_result) != i) {
            printf("User data == %"PRIu32"; i == %"PRIu32": ", *( (uint32_t*)node->prefix->user_data_result), i);
            result = 0;
            goto end;
        }
    }

end:
    SCRadixReleaseRadixTree(tree);

    return result;
}

/**
 * \test Check that the best match search works for all the
 *       possible netblocks of a fixed address
 */
int SCRadixTestIPV4NetBlocksAndBestSearch17(void)
{
    SCRadixTree *tree = NULL;
    struct sockaddr_in servaddr;
    int result = 1;

    tree = SCRadixCreateRadixTree(free, NULL);

    uint32_t i = 0;

    uint32_t *user;

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "10.0.0.1", &servaddr.sin_addr) <= 0) {
        result = 0;
        goto end;
    }

    for (; i <= 32; i++) {
        user = SCMalloc(sizeof(uint32_t));
        if (user == NULL) {
            result = 0;
            goto end;
        }

        *user = i;

        SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, user, i);

        SCRadixNode *node = SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree);
        if (node == NULL) {
            printf("node == NULL: ");
            result = 0;
            goto end;
        }

        if (node->prefix->user_data_result == NULL) {
            printf("User data == NULL: ");
            result = 0;
            goto end;
        }

        if ( *( (uint32_t*)node->prefix->user_data_result) != i) {
            printf("User data == %"PRIu32"; i == %"PRIu32": ", *( (uint32_t*)node->prefix->user_data_result), i);
            result = 0;
            goto end;
        }
    }

end:
    SCRadixReleaseRadixTree(tree);

    return result;
}

/**
 * \test Check that the best match search works for all the
 *       possible netblocks of a fixed address
 */
int SCRadixTestIPV4NetBlocksAndBestSearch18(void)
{
    SCRadixTree *tree = NULL;
    struct sockaddr_in servaddr;
    int result = 1;

    tree = SCRadixCreateRadixTree(free, NULL);

    uint32_t i = 0;

    uint32_t *user;

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "172.26.0.1", &servaddr.sin_addr) <= 0) {
        result = 0;
        goto end;
    }

    for (; i <= 32; i++) {
        user = SCMalloc(sizeof(uint32_t));
        if (user == NULL) {
            result = 0;
            goto end;
        }

        *user = i;

        SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, user, i);

        SCRadixNode *node = SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree);
        if (node == NULL) {
            printf("node == NULL: ");
            result = 0;
            goto end;
        }

        if (node->prefix->user_data_result == NULL) {
            printf("User data == NULL: ");
            result = 0;
            goto end;
        }

        if ( *( (uint32_t*)node->prefix->user_data_result) != i) {
            printf("User data == %"PRIu32"; i == %"PRIu32": ", *( (uint32_t*)node->prefix->user_data_result), i);
            result = 0;
            goto end;
        }
    }

end:
    SCRadixReleaseRadixTree(tree);

    return result;
}

/**
 * \test Check special combinations of netblocks and addresses
 *       on best search checking the returned userdata
 */
int SCRadixTestIPV4NetBlocksAndBestSearch19(void)
{
    SCRadixTree *tree = NULL;
    struct sockaddr_in servaddr;
    int result = 1;

    tree = SCRadixCreateRadixTree(free, NULL);

    uint32_t *user;

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "0.0.0.0", &servaddr.sin_addr) <= 0) {
        result = 0;
        goto end;
    }

    user = SCMalloc(sizeof(uint32_t));
    if (user == NULL) {
        result = 0;
        goto end;
    }

    *user = 100;

    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, user, 0);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.1.15", &servaddr.sin_addr) <= 0) {
        result = 0;
        goto end;
    }

    SCRadixNode *node = SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree);
    if (node == NULL) {
        printf("node == NULL: ");
        result = 0;
        goto end;
    }

    if (node->prefix->user_data_result == NULL) {
        printf("User data == NULL: ");
        result = 0;
        goto end;
    }

    if ( *( (uint32_t*)node->prefix->user_data_result) != 100) {
        result = 0;
        goto end;
    }

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "177.0.0.0", &servaddr.sin_addr) <= 0) {
        result = 0;
        goto end;
    }

    user = SCMalloc(sizeof(uint32_t));
    if (user == NULL) {
        result = 0;
        goto end;
    }

    *user = 200;

    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, user, 8);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "177.168.1.15", &servaddr.sin_addr) <= 0) {
        result = 0;
        goto end;
    }

    node = SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree);
    if (node == NULL) {
        printf("node == NULL: ");
        result = 0;
        goto end;
    }

    if (node->prefix->user_data_result == NULL) {
        printf("User data == NULL: ");
        result = 0;
        goto end;
    }

    if ( *( (uint32_t*)node->prefix->user_data_result) != 200) {
        result = 0;
        goto end;
    }

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "178.168.1.15", &servaddr.sin_addr) <= 0) {
        result = 0;
        goto end;
    }

    node = SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree);
    if (node == NULL) {
        printf("node == NULL: ");
        result = 0;
        goto end;
    }

    if (node->prefix->user_data_result == NULL) {
        printf("User data == NULL: ");
        result = 0;
        goto end;
    }

    if ( *( (uint32_t*)node->prefix->user_data_result) != 100) {
        result = 0;
        goto end;
    }

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "177.168.0.0", &servaddr.sin_addr) <= 0) {
        result = 0;
        goto end;
    }

    user = SCMalloc(sizeof(uint32_t));
    if (user == NULL) {
        result = 0;
        goto end;
    }

    *user = 300;

    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, user, 12);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "177.168.1.15", &servaddr.sin_addr) <= 0) {
        result = 0;
        goto end;
    }

    node = SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree);
    if (node == NULL) {
        printf("node == NULL: ");
        result = 0;
        goto end;
    }

    if (node->prefix->user_data_result == NULL) {
        printf("User data == NULL: ");
        result = 0;
        goto end;
    }

    if ( *( (uint32_t*)node->prefix->user_data_result) != 300) {
        result = 0;
        goto end;
    }

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "177.167.1.15", &servaddr.sin_addr) <= 0) {
        result = 0;
        goto end;
    }

    node = SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree);
    if (node == NULL) {
        printf("node == NULL: ");
        result = 0;
        goto end;
    }

    if (node->prefix->user_data_result == NULL) {
        printf("User data == NULL: ");
        result = 0;
        goto end;
    }

    if ( *( (uint32_t*)node->prefix->user_data_result) != 300) {
        result = 0;
        goto end;
    }

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "177.178.1.15", &servaddr.sin_addr) <= 0) {
        result = 0;
        goto end;
    }

    node = SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree);
    if (node == NULL) {
        printf("node == NULL: ");
        result = 0;
        goto end;
    }

    if (node->prefix->user_data_result == NULL) {
        printf("User data == NULL: ");
        result = 0;
        goto end;
    }

    if ( *( (uint32_t*)node->prefix->user_data_result) != 200) {
        result = 0;
        goto end;
    }

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "197.178.1.15", &servaddr.sin_addr) <= 0) {
        result = 0;
        goto end;
    }

    node = SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree);
    if (node == NULL) {
        printf("node == NULL: ");
        result = 0;
        goto end;
    }

    if (node->prefix->user_data_result == NULL) {
        printf("User data == NULL: ");
        result = 0;
        goto end;
    }

    if ( *( (uint32_t*)node->prefix->user_data_result) != 100) {
        result = 0;
        goto end;
    }


end:
    SCRadixReleaseRadixTree(tree);

    return result;
}

/**
 * \test Check that the best match search works for all the
 *       possible netblocks of a fixed address
 */
int SCRadixTestIPV6NetBlocksAndBestSearch20(void)
{
    SCRadixTree *tree = NULL;
    struct sockaddr_in6 servaddr;
    int result = 1;

    tree = SCRadixCreateRadixTree(free, NULL);

    uint32_t i = 0;

    uint32_t *user;

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "ABAB:CDCD:ABAB:CDCD:1234:4321:1234:4321", &servaddr.sin6_addr) <= 0) {
        result = 0;
        goto end;
    }

    for (; i <= 128; i++) {
        user = SCMalloc(sizeof(uint32_t));
        if (user == NULL) {
            result = 0;
            goto end;
        }

        *user = i;

        SCRadixAddKeyIPV6Netblock((uint8_t *)&servaddr.sin6_addr, tree, user, i);

        SCRadixNode *node = SCRadixFindKeyIPV6BestMatch((uint8_t *)&servaddr.sin6_addr, tree);
        if (node == NULL) {
            printf("node == NULL: ");
            result = 0;
            goto end;
        }

        if (node->prefix->user_data_result == NULL) {
            printf("User data == NULL: ");
            result = 0;
            goto end;
        }

        if ( *( (uint32_t*)node->prefix->user_data_result) != i) {
            printf("User data == %"PRIu32"; i == %"PRIu32": ", *( (uint32_t*)node->prefix->user_data_result), i);
            result = 0;
            goto end;
        }
    }

end:
    SCRadixReleaseRadixTree(tree);

    return result;
}

/**
 * \test Check that the best match search works for all the
 *       possible netblocks of a fixed address
 */
int SCRadixTestIPV6NetBlocksAndBestSearch21(void)
{
    SCRadixTree *tree = NULL;
    struct sockaddr_in6 servaddr;
    int result = 1;

    tree = SCRadixCreateRadixTree(free, NULL);

    uint32_t i = 0;

    uint32_t *user;

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "ff00::1", &servaddr.sin6_addr) <= 0) {
        result = 0;
        goto end;
    }

    for (; i <= 128; i++) {
        user = SCMalloc(sizeof(uint32_t));
        if (user == NULL) {
            result = 0;
            goto end;
        }

        *user = i;

        SCRadixAddKeyIPV6Netblock((uint8_t *)&servaddr.sin6_addr, tree, user, i);

        SCRadixNode *node = SCRadixFindKeyIPV6BestMatch((uint8_t *)&servaddr.sin6_addr, tree);
        if (node == NULL) {
            printf("node == NULL: ");
            result = 0;
            goto end;
        }

        if (node->prefix->user_data_result == NULL) {
            printf("User data == NULL: ");
            result = 0;
            goto end;
        }

        if ( *( (uint32_t*)node->prefix->user_data_result) != i) {
            printf("User data == %"PRIu32"; i == %"PRIu32": ", *( (uint32_t*)node->prefix->user_data_result), i);
            result = 0;
            goto end;
        }
    }

end:
    SCRadixReleaseRadixTree(tree);

    return result;
}

/**
 * \test Check that the best match search works for all the
 *       possible netblocks of a fixed address
 */
int SCRadixTestIPV6NetBlocksAndBestSearch22(void)
{
    SCRadixTree *tree = NULL;
    struct sockaddr_in6 servaddr;
    int result = 1;

    tree = SCRadixCreateRadixTree(free, NULL);

    uint32_t i = 0;

    uint32_t *user;

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "ff00::192:168:1:1", &servaddr.sin6_addr) <= 0) {
        result = 0;
        goto end;
    }

    for (; i <= 128; i++) {
        user = SCMalloc(sizeof(uint32_t));
        if (user == NULL) {
            result = 0;
            goto end;
        }

        *user = i;

        SCRadixAddKeyIPV6Netblock((uint8_t *)&servaddr.sin6_addr, tree, user, i);

        SCRadixNode *node = SCRadixFindKeyIPV6BestMatch((uint8_t *)&servaddr.sin6_addr, tree);
        if (node == NULL) {
            printf("node == NULL: ");
            result = 0;
            goto end;
        }

        if (node->prefix->user_data_result == NULL) {
            printf("User data == NULL: ");
            result = 0;
            goto end;
        }

        if ( *( (uint32_t*)node->prefix->user_data_result) != i) {
            printf("User data == %"PRIu32"; i == %"PRIu32": ", *( (uint32_t*)node->prefix->user_data_result), i);
            result = 0;
            goto end;
        }
    }

end:
    SCRadixReleaseRadixTree(tree);

    return result;
}

/**
 * \test Check that the best match search works for all the
 *       possible netblocks of a fixed address
 */
int SCRadixTestIPV6NetBlocksAndBestSearch23(void)
{
    SCRadixTree *tree = NULL;
    struct sockaddr_in6 servaddr;
    int result = 1;

    tree = SCRadixCreateRadixTree(free, NULL);

    uint32_t i = 0;

    uint32_t *user;

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "FF00:ABCD:BCDA::ABCD", &servaddr.sin6_addr) <= 0) {
        result = 0;
        goto end;
    }

    for (; i <= 128; i++) {
        user = SCMalloc(sizeof(uint32_t));
        if (user == NULL) {
            result = 0;
            goto end;
        }

        *user = i;

        SCRadixAddKeyIPV6Netblock((uint8_t *)&servaddr.sin6_addr, tree, user, i);

        SCRadixNode *node = SCRadixFindKeyIPV6BestMatch((uint8_t *)&servaddr.sin6_addr, tree);
        if (node == NULL) {
            printf("node == NULL: ");
            result = 0;
            goto end;
        }

        if (node->prefix->user_data_result == NULL) {
            printf("User data == NULL: ");
            result = 0;
            goto end;
        }

        if ( *( (uint32_t*)node->prefix->user_data_result) != i) {
            printf("User data == %"PRIu32"; i == %"PRIu32": ", *( (uint32_t*)node->prefix->user_data_result), i);
            result = 0;
            goto end;
        }
    }

end:
    SCRadixReleaseRadixTree(tree);

    return result;
}

/**
 * \test Check special combinations of netblocks and addresses
 *       on best search checking the returned userdata
 */
int SCRadixTestIPV6NetBlocksAndBestSearch24(void)
{
    SCRadixTree *tree = NULL;
    struct sockaddr_in6 servaddr;
    int result = 1;

    tree = SCRadixCreateRadixTree(free, NULL);

    uint32_t *user;

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "::", &servaddr.sin6_addr) <= 0) {
        result = 0;
        goto end;
    }

    user = SCMalloc(sizeof(uint32_t));
    if (user == NULL) {
        result = 0;
        goto end;
    }

    *user = 100;

    SCRadixAddKeyIPV6Netblock((uint8_t *)&servaddr.sin6_addr, tree, user, 0);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "ABCD::1", &servaddr.sin6_addr) <= 0) {
        result = 0;
        goto end;
    }

    SCRadixNode *node = SCRadixFindKeyIPV6BestMatch((uint8_t *)&servaddr.sin6_addr, tree);
    if (node == NULL) {
        printf("node == NULL: ");
        result = 0;
        goto end;
    }

    if (node->prefix->user_data_result == NULL) {
        printf("User data == NULL: ");
        result = 0;
        goto end;
    }

    if ( *( (uint32_t*)node->prefix->user_data_result) != 100) {
        result = 0;
        goto end;
    }

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "ABCD::0", &servaddr.sin6_addr) <= 0) {
        result = 0;
        goto end;
    }

    user = SCMalloc(sizeof(uint32_t));
    if (user == NULL) {
        result = 0;
        goto end;
    }

    *user = 200;

    SCRadixAddKeyIPV6Netblock((uint8_t *)&servaddr.sin6_addr, tree, user, 8);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "ABCD::1", &servaddr.sin6_addr) <= 0) {
        result = 0;
        goto end;
    }

    node = SCRadixFindKeyIPV6BestMatch((uint8_t *)&servaddr.sin6_addr, tree);
    if (node == NULL) {
        printf("node == NULL: ");
        result = 0;
        goto end;
    }

    if (node->prefix->user_data_result == NULL) {
        printf("User data == NULL: ");
        result = 0;
        goto end;
    }

    if ( *( (uint32_t*)node->prefix->user_data_result) != 200) {
        printf("User data == %"PRIu32"; i != 200 ", *( (uint32_t*)node->prefix->user_data_result));
        result = 0;
        goto end;
    }

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "DCBA::1", &servaddr.sin6_addr) <= 0) {
        result = 0;
        goto end;
    }

    node = SCRadixFindKeyIPV6BestMatch((uint8_t *)&servaddr.sin6_addr, tree);
    if (node == NULL) {
        printf("node == NULL: ");
        result = 0;
        goto end;
    }

    if (node->prefix->user_data_result == NULL) {
        printf("User data == NULL: ");
        result = 0;
        goto end;
    }

    if ( *( (uint32_t*)node->prefix->user_data_result) != 100) {
        printf("User data == %"PRIu32"; != 100 ", *( (uint32_t*)node->prefix->user_data_result));
        result = 0;
        goto end;
    }

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "ABCD:ABCD::0", &servaddr.sin6_addr) <= 0) {
        result = 0;
        goto end;
    }

    user = SCMalloc(sizeof(uint32_t));
    if (user == NULL) {
        result = 0;
        goto end;
    }

    *user = 300;

    SCRadixAddKeyIPV6Netblock((uint8_t *)&servaddr.sin6_addr, tree, user, 12);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "ABCD:ABCD::1", &servaddr.sin6_addr) <= 0) {
        result = 0;
        goto end;
    }

    node = SCRadixFindKeyIPV6BestMatch((uint8_t *)&servaddr.sin6_addr, tree);
    if (node == NULL) {
        printf("node == NULL: ");
        result = 0;
        goto end;
    }

    if (node->prefix->user_data_result == NULL) {
        printf("User data == NULL: ");
        result = 0;
        goto end;
    }

    if ( *( (uint32_t*)node->prefix->user_data_result) != 300) {
        result = 0;
        goto end;
    }

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "ABCD:AAAA::1", &servaddr.sin6_addr) <= 0) {
        result = 0;
        goto end;
    }

    node = SCRadixFindKeyIPV6BestMatch((uint8_t *)&servaddr.sin6_addr, tree);
    if (node == NULL) {
        printf("node == NULL: ");
        result = 0;
        goto end;
    }

    if (node->prefix->user_data_result == NULL) {
        printf("User data == NULL: ");
        result = 0;
        goto end;
    }

    if ( *( (uint32_t*)node->prefix->user_data_result) != 300) {
        result = 0;
        goto end;
    }

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "ABAB::1", &servaddr.sin6_addr) <= 0) {
        result = 0;
        goto end;
    }

    node = SCRadixFindKeyIPV6BestMatch((uint8_t *)&servaddr.sin6_addr, tree);
    if (node == NULL) {
        printf("node == NULL: ");
        result = 0;
        goto end;
    }

    if (node->prefix->user_data_result == NULL) {
        printf("User data == NULL: ");
        result = 0;
        goto end;
    }

    if ( *( (uint32_t*)node->prefix->user_data_result) != 200) {
        result = 0;
        goto end;
    }

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET6, "CABD::1", &servaddr.sin6_addr) <= 0) {
        result = 0;
        goto end;
    }

    node = SCRadixFindKeyIPV6BestMatch((uint8_t *)&servaddr.sin6_addr, tree);
    if (node == NULL) {
        printf("node == NULL: ");
        result = 0;
        goto end;
    }

    if (node->prefix->user_data_result == NULL) {
        printf("User data == NULL: ");
        result = 0;
        goto end;
    }

    if ( *( (uint32_t*)node->prefix->user_data_result) != 100) {
        result = 0;
        goto end;
    }


end:
    SCRadixReleaseRadixTree(tree);

    return result;
}


/**
 * \test SCRadixTestIPV4NetblockInsertion15 insert a node searching on it.
 *       Should always return true but the purposse of the test is to monitor
 *       the memory usage to detect memleaks (there was one on searching)
 */
int SCRadixTestIPV4NetblockInsertion25(void)
{
    SCRadixTree *tree = NULL;
    struct sockaddr_in servaddr;
    int result = 1;

    tree = SCRadixCreateRadixTree(free, NULL);

    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.0.0", &servaddr.sin_addr) <= 0)
        return 0;
    SCRadixAddKeyIPV4Netblock((uint8_t *)&servaddr.sin_addr, tree, NULL, 16);

    /* test for the existance of a key */
    bzero(&servaddr, sizeof(servaddr));
    if (inet_pton(AF_INET, "192.168.128.53", &servaddr.sin_addr) <= 0)
        return 0;

    result &= (SCRadixFindKeyIPV4BestMatch((uint8_t *)&servaddr.sin_addr, tree) != NULL);

    SCRadixReleaseRadixTree(tree);

    return result;
}

/**
 * \test SC_RADIX_NODE_USERDATA macro
 */
static int SCRadixTestUserdataMacro01(void) {
    SCRadixNode node;
    int result = 0;

    memset(&node, 0x00, sizeof(node));

    void *ptr = SC_RADIX_NODE_USERDATA(&node, void);
    if (ptr != NULL) {
        printf("ptr %p, expected NULL: ", ptr);
        goto end;
    }

    result = 1;
end:
    return result;
}

/**
 * \test SC_RADIX_NODE_USERDATA macro
 */
static int SCRadixTestUserdataMacro02(void) {
    SCRadixNode node;
    SCRadixPrefix prefix;
    int result = 0;

    memset(&node, 0x00, sizeof(node));
    memset(&prefix, 0x00, sizeof(prefix));

    node.prefix = &prefix;

    void *ptr = SC_RADIX_NODE_USERDATA(&node, void);
    if (ptr != NULL) {
        printf("ptr %p, expected NULL: ", ptr);
        goto end;
    }

    result = 1;
end:
    return result;
}

/**
 * \test SC_RADIX_NODE_USERDATA macro
 */
static int SCRadixTestUserdataMacro03(void) {
    SCRadixNode node;
    SCRadixPrefix prefix;
    int result = 0;
    void *somep = &result;

    memset(&node, 0x00, sizeof(node));
    memset(&prefix, 0x00, sizeof(prefix));

    node.prefix = &prefix;
    prefix.user_data_result = somep;

    void *ptr = SC_RADIX_NODE_USERDATA(&node, void);
    if (ptr != somep) {
        printf("ptr %p, expected %p: ", ptr, somep);
        goto end;
    }

    result = 1;
end:
    return result;
}

#endif

void SCRadixRegisterTests(void)
{

#ifdef UNITTESTS
    //UtRegisterTest("SCRadixTestInsertion01", SCRadixTestInsertion01, 1);
    //UtRegisterTest("SCRadixTestInsertion02", SCRadixTestInsertion02, 1);
    UtRegisterTest("SCRadixTestIPV4Insertion03", SCRadixTestIPV4Insertion03, 1);
    UtRegisterTest("SCRadixTestIPV4Removal04", SCRadixTestIPV4Removal04, 1);
    //UtRegisterTest("SCRadixTestCharacterInsertion05",
    //               SCRadixTestCharacterInsertion05, 1);
    //UtRegisterTest("SCRadixTestCharacterRemoval06",
    //               SCRadixTestCharacterRemoval06, 1);
    UtRegisterTest("SCRadixTestIPV6Insertion07", SCRadixTestIPV6Insertion07, 1);
    UtRegisterTest("SCRadixTestIPV6Removal08", SCRadixTestIPV6Removal08, 1);
    UtRegisterTest("SCRadixTestIPV4NetblockInsertion09",
                   SCRadixTestIPV4NetblockInsertion09, 1);
    UtRegisterTest("SCRadixTestIPV4NetblockInsertion10",
                   SCRadixTestIPV4NetblockInsertion10, 1);
    UtRegisterTest("SCRadixTestIPV4NetblockInsertion11",
                   SCRadixTestIPV4NetblockInsertion11, 1);
    UtRegisterTest("SCRadixTestIPV4NetblockInsertion12",
                   SCRadixTestIPV4NetblockInsertion12, 1);
    UtRegisterTest("SCRadixTestIPV6NetblockInsertion13",
                   SCRadixTestIPV6NetblockInsertion13, 1);
    UtRegisterTest("SCRadixTestIPV6NetblockInsertion14",
                   SCRadixTestIPV6NetblockInsertion14, 1);
    UtRegisterTest("SCRadixTestIPV4NetBlocksAndBestSearch15",
                   SCRadixTestIPV4NetBlocksAndBestSearch15, 1);
    UtRegisterTest("SCRadixTestIPV4NetBlocksAndBestSearch16",
                   SCRadixTestIPV4NetBlocksAndBestSearch16, 1);
    UtRegisterTest("SCRadixTestIPV4NetBlocksAndBestSearch17",
                   SCRadixTestIPV4NetBlocksAndBestSearch17, 1);
    UtRegisterTest("SCRadixTestIPV4NetBlocksAndBestSearch18",
                   SCRadixTestIPV4NetBlocksAndBestSearch18, 1);
    UtRegisterTest("SCRadixTestIPV4NetBlocksAndBestSearch19",
                   SCRadixTestIPV4NetBlocksAndBestSearch19, 1);
    UtRegisterTest("SCRadixTestIPV6NetBlocksAndBestSearch20",
                   SCRadixTestIPV6NetBlocksAndBestSearch20, 1);
    UtRegisterTest("SCRadixTestIPV6NetBlocksAndBestSearch21",
                   SCRadixTestIPV6NetBlocksAndBestSearch21, 1);
    UtRegisterTest("SCRadixTestIPV6NetBlocksAndBestSearch22",
                   SCRadixTestIPV6NetBlocksAndBestSearch22, 1);
    UtRegisterTest("SCRadixTestIPV6NetBlocksAndBestSearch23",
                   SCRadixTestIPV6NetBlocksAndBestSearch23, 1);
    UtRegisterTest("SCRadixTestIPV6NetBlocksAndBestSearch24",
                   SCRadixTestIPV6NetBlocksAndBestSearch24, 1);
    UtRegisterTest("SCRadixTestIPV4NetblockInsertion25",
                   SCRadixTestIPV4NetblockInsertion25, 1);
    UtRegisterTest("SCRadixTestUserdataMacro01",
                   SCRadixTestUserdataMacro01, 1);
    UtRegisterTest("SCRadixTestUserdataMacro02",
                   SCRadixTestUserdataMacro02, 1);
    UtRegisterTest("SCRadixTestUserdataMacro03",
                   SCRadixTestUserdataMacro03, 1);
#endif

    return;
}
