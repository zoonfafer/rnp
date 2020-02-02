/*
 * Copyright (c) 2017-2020 [Ribose Inc](https://www.ribose.com).
 * Copyright (c) 2009 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is originally derived from software contributed to
 * The NetBSD Foundation by Alistair Crooks (agc@netbsd.org), and
 * carried further by Ribose Inc (https://www.ribose.com).
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * Copyright (c) 2005-2008 Nominet UK (www.nic.uk)
 * All rights reserved.
 * Contributors: Ben Laurie, Rachel Willmer. The Contributors have asserted
 * their moral rights under the UK Copyright Design and Patents Act 1988 to
 * be recorded as the authors of this copyright work.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if defined(__NetBSD__)
__COPYRIGHT("@(#) Copyright (c) 2009 The NetBSD Foundation, Inc. All rights reserved.");
__RCSID("$NetBSD: keyring.c,v 1.50 2011/06/25 00:37:44 agc Exp $");
#endif

#include <stdlib.h>
#include <string.h>

#include <rnp/rnp_sdk.h>
#include <librepgp/stream-common.h>
#include <librepgp/stream-sig.h>
#include <librepgp/stream-packet.h>
#include <librepgp/stream-key.h>

#include "types.h"
#include "key_store_pgp.h"
#include "pgp-key.h"

static pgp_map_t ss_rr_code_map[] = {
  {0x00, "No reason specified"},
  {0x01, "Key is superseded"},
  {0x02, "Key material has been compromised"},
  {0x03, "Key is retired and no longer used"},
  {0x20, "User ID information is no longer valid"},
  {0x00, NULL}, /* this is the end-of-array marker */
};

static bool
create_key_from_pkt(pgp_key_t *key, pgp_key_pkt_t *pkt)
{
    pgp_key_pkt_t keypkt = {};

    memset(key, 0, sizeof(*key));

    if (!copy_key_pkt(&keypkt, pkt, false)) {
        RNP_LOG("failed to copy key packet");
        return false;
    }

    /* parse secret key if not encrypted */
    if (is_secret_key_pkt(keypkt.tag)) {
        bool cleartext = keypkt.sec_protection.s2k.usage == PGP_S2KU_NONE;
        if (cleartext && decrypt_secret_key(&keypkt, NULL)) {
            RNP_LOG("failed to setup key fields");
            free_key_pkt(&keypkt);
            return false;
        }
    }

    /* this call transfers ownership */
    if (!pgp_key_from_pkt(key, &keypkt, (pgp_pkt_type_t) pkt->tag)) {
        RNP_LOG("failed to setup key fields");
        free_key_pkt(&keypkt);
        return false;
    }

    /* add key rawpacket */
    if (!pgp_key_add_key_rawpacket(key, pkt)) {
        free_key_pkt(&keypkt);
        return false;
    }

    key->format = PGP_KEY_STORE_GPG;
    key->key_flags = pgp_pk_alg_capabilities(pgp_key_get_alg(key));
    return true;
}

static bool
rnp_key_add_signature(pgp_key_t *key, pgp_signature_t *sig)
{
    pgp_subsig_t *subsig = NULL;
    uint8_t *     algs = NULL;
    size_t        count = 0;

    if (!(subsig = pgp_key_add_subsig(key))) {
        RNP_LOG("Failed to add subsig");
        return false;
    }

    /* add signature rawpacket */
    if (!pgp_key_add_sig_rawpacket(key, sig)) {
        return false;
    }

    subsig->uid = pgp_key_get_userid_count(key) - 1;
    if (!copy_signature_packet(&subsig->sig, sig)) {
        return false;
    }

    if (signature_has_key_expiration(&subsig->sig)) {
        key->expiration = signature_get_key_expiration(&subsig->sig);
    }
    if (signature_has_trust(&subsig->sig)) {
        signature_get_trust(&subsig->sig, &subsig->trustlevel, &subsig->trustamount);
    }
    if (signature_get_primary_uid(&subsig->sig)) {
        key->uid0 = pgp_key_get_userid_count(key) - 1;
        key->uid0_set = 1;
    }

    if (signature_get_preferred_symm_algs(&subsig->sig, &algs, &count) &&
        !pgp_user_prefs_set_symm_algs(&subsig->prefs, algs, count)) {
        RNP_LOG("failed to alloc symm algs");
        return false;
    }
    if (signature_get_preferred_hash_algs(&subsig->sig, &algs, &count) &&
        !pgp_user_prefs_set_hash_algs(&subsig->prefs, algs, count)) {
        RNP_LOG("failed to alloc hash algs");
        return false;
    }
    if (signature_get_preferred_z_algs(&subsig->sig, &algs, &count) &&
        !pgp_user_prefs_set_z_algs(&subsig->prefs, algs, count)) {
        RNP_LOG("failed to alloc z algs");
        return false;
    }
    if (signature_has_key_flags(&subsig->sig)) {
        subsig->key_flags = signature_get_key_flags(&subsig->sig);
        key->key_flags = subsig->key_flags;
    }
    if (signature_has_key_server_prefs(&subsig->sig)) {
        uint8_t ks_pref = signature_get_key_server_prefs(&subsig->sig);
        if (!pgp_user_prefs_set_ks_prefs(&subsig->prefs, &ks_pref, 1)) {
            RNP_LOG("failed to alloc ks prefs");
            return false;
        }
    }
    if (signature_has_key_server(&subsig->sig)) {
        subsig->prefs.key_server = (uint8_t *) signature_get_key_server(&subsig->sig);
    }
    if (signature_has_revocation_reason(&subsig->sig)) {
        /* not sure whether this logic is correct - we should check signature type? */
        pgp_revoke_t *revocation = NULL;
        if (!pgp_key_get_userid_count(key)) {
            /* revoke whole key */
            key->revoked = 1;
            revocation = &key->revocation;
        } else {
            /* revoke the user id */
            if (!(revocation = pgp_key_add_revoke(key))) {
                RNP_LOG("failed to add revoke");
                return false;
            }
            revocation->uid = pgp_key_get_userid_count(key) - 1;
        }
        signature_get_revocation_reason(&subsig->sig, &revocation->code, &revocation->reason);
        if (!strlen(revocation->reason)) {
            free(revocation->reason);
            revocation->reason = strdup(pgp_str_from_map(revocation->code, ss_rr_code_map));
        }
    }

    return true;
}

static bool
rnp_key_add_signatures(pgp_key_t *key, list signatures)
{
    for (list_item *sig = list_front(signatures); sig; sig = list_next(sig)) {
        if (!rnp_key_add_signature(key, (pgp_signature_t *) sig)) {
            return false;
        }
    }
    return true;
}

bool
rnp_key_store_add_transferable_subkey(rnp_key_store_t *          keyring,
                                      pgp_transferable_subkey_t *tskey,
                                      pgp_key_t *                pkey)
{
    pgp_key_t skey = {};

    /* create subkey */
    if (!rnp_key_from_transferable_subkey(&skey, tskey, pkey)) {
        RNP_LOG("failed to create subkey");
        return false;
    }

    /* add it to the storage */
    if (!rnp_key_store_add_key(keyring, &skey)) {
        RNP_LOG("Failed to add subkey to key store.");
        goto error;
    }

    return true;
error:
    pgp_key_free_data(&skey);
    return false;
}

bool
rnp_key_add_transferable_userid(pgp_key_t *key, pgp_transferable_userid_t *uid)
{
    if (!pgp_key_add_uid_rawpacket(key, &uid->uid)) {
        return false;
    }

    pgp_userid_t *userid = pgp_key_add_userid(key);
    if (!userid) {
        RNP_LOG("Failed to add userid");
        return false;
    }
    if (uid->uid.tag == PGP_PKT_USER_ID) {
        userid->str = (char *) calloc(1, uid->uid.uid_len + 1);
        if (!userid->str) {
            RNP_LOG("uid alloc failed");
            return false;
        }
        memcpy(userid->str, uid->uid.uid, uid->uid.uid_len);
        userid->str[uid->uid.uid_len] = 0;
    } else {
        userid->str = strdup("(photo)");
        if (!userid->str) {
            RNP_LOG("uattr alloc failed");
            return false;
        }
    }

    if (!copy_userid_pkt(&userid->pkt, &uid->uid)) {
        RNP_LOG("failed to copy user id pkt");
        return false;
    }

    if (!rnp_key_add_signatures(key, uid->signatures)) {
        return false;
    }

    return true;
}

bool
rnp_key_store_add_transferable_key(rnp_key_store_t *keyring, pgp_transferable_key_t *tkey)
{
    pgp_key_t  key = {};
    pgp_key_t *addkey = NULL;

    /* create key from transferable key */
    if (!rnp_key_from_transferable_key(&key, tkey)) {
        RNP_LOG("failed to create key");
        return false;
    }

    /* add key to the storage before subkeys */
    if (!(addkey = rnp_key_store_add_key(keyring, &key))) {
        RNP_LOG("Failed to add key to key store.");
        goto error;
    }

    /* add subkeys */
    for (list_item *skey = list_front(tkey->subkeys); skey; skey = list_next(skey)) {
        pgp_transferable_subkey_t *subkey = (pgp_transferable_subkey_t *) skey;
        if (!rnp_key_store_add_transferable_subkey(keyring, subkey, addkey)) {
            goto error;
        }
    }

    return true;
error:
    if (addkey) {
        /* during key addition all fields are copied so will be cleaned below */
        rnp_key_store_remove_key(keyring, addkey);
        pgp_key_free_data(addkey);
    } else {
        pgp_key_free_data(&key);
    }
    return false;
}

bool
rnp_key_from_transferable_key(pgp_key_t *key, pgp_transferable_key_t *tkey)
{
    memset(key, 0, sizeof(*key));
    /* create key */
    if (!create_key_from_pkt(key, &tkey->key)) {
        return false;
    }

    /* add direct-key signatures */
    if (!rnp_key_add_signatures(key, tkey->signatures)) {
        goto error;
    }

    /* add userids and their signatures */
    for (list_item *uid = list_front(tkey->userids); uid; uid = list_next(uid)) {
        pgp_transferable_userid_t *tuid = (pgp_transferable_userid_t *) uid;
        if (!rnp_key_add_transferable_userid(key, tuid)) {
            goto error;
        }
    }

    return true;
error:
    pgp_key_free_data(key);
    return false;
}

bool
rnp_key_from_transferable_subkey(pgp_key_t *                subkey,
                                 pgp_transferable_subkey_t *tskey,
                                 pgp_key_t *                primary)
{
    memset(subkey, 0, sizeof(*subkey));

    /* create key */
    if (!create_key_from_pkt(subkey, &tskey->subkey)) {
        return false;
    }

    /* add subkey binding signatures */
    if (!rnp_key_add_signatures(subkey, tskey->signatures)) {
        RNP_LOG("failed to add subkey signatures");
        goto error;
    }

    /* setup key grips if primary is available */
    if (primary && !pgp_key_link_subkey_grip(primary, subkey)) {
        goto error;
    }
    return true;
error:
    pgp_key_free_data(subkey);
    return false;
}

rnp_result_t
rnp_key_store_pgp_read_from_src(rnp_key_store_t *keyring, pgp_source_t *src)
{
    pgp_key_sequence_t        keys = {};
    pgp_transferable_subkey_t tskey = {};
    rnp_result_t              ret = RNP_ERROR_GENERIC;

    /* check whether we have transferable subkey in source */
    if (is_subkey_pkt(stream_pkt_type(src))) {
        if ((ret = process_pgp_subkey(src, &tskey))) {
            return ret;
        }
        ret = rnp_key_store_add_transferable_subkey(keyring, &tskey, NULL) ?
                RNP_SUCCESS :
                RNP_ERROR_BAD_STATE;
        transferable_subkey_destroy(&tskey);
        return ret;
    }

    /* process armored or raw transferable key packets sequence(s) */
    if ((ret = process_pgp_keys(src, &keys))) {
        return ret;
    }

    for (list_item *key = list_front(keys.keys); key; key = list_next(key)) {
        if (!rnp_key_store_add_transferable_key(keyring, (pgp_transferable_key_t *) key)) {
            ret = RNP_ERROR_BAD_STATE;
            goto done;
        }
    }

    ret = RNP_SUCCESS;
done:
    key_sequence_destroy(&keys);
    return ret;
}

bool
rnp_key_write_packets_stream(const pgp_key_t *key, pgp_dest_t *dst)
{
    if (!pgp_key_get_rawpacket_count(key)) {
        return false;
    }
    for (size_t i = 0; i < pgp_key_get_rawpacket_count(key); i++) {
        pgp_rawpacket_t *pkt = pgp_key_get_rawpacket(key, i);
        if (!pkt->raw || !pkt->length) {
            return false;
        }
        dst_write(dst, pkt->raw, pkt->length);
    }
    return !dst->werr;
}

bool
rnp_key_to_src(const pgp_key_t *key, pgp_source_t *src)
{
    pgp_dest_t dst = {};
    bool       res;

    if (init_mem_dest(&dst, NULL, 0)) {
        return false;
    }

    res = rnp_key_write_packets_stream(key, &dst) &&
          !init_mem_src(src, mem_dest_own_memory(&dst), dst.writeb, true);
    dst_close(&dst, true);
    return res;
}

static bool
do_write(rnp_key_store_t *key_store, pgp_dest_t *dst, bool secret)
{
    for (list_item *key_item = list_front(rnp_key_store_get_keys(key_store)); key_item;
         key_item = list_next(key_item)) {
        pgp_key_t *key = (pgp_key_t *) key_item;
        if (pgp_key_is_secret(key) != secret) {
            continue;
        }
        // skip subkeys, they are written below (orphans are ignored)
        if (!pgp_key_is_primary_key(key)) {
            continue;
        }

        if (key->format != PGP_KEY_STORE_GPG) {
            RNP_LOG("incorrect format (conversions not supported): %d", key->format);
            return false;
        }
        if (!rnp_key_write_packets_stream(key, dst)) {
            return false;
        }
        for (list_item *subkey_grip = list_front(key->subkey_grips); subkey_grip;
             subkey_grip = list_next(subkey_grip)) {
            pgp_key_search_t search = {};
            search.type = PGP_KEY_SEARCH_GRIP;
            memcpy(search.by.grip, (uint8_t *) subkey_grip, PGP_KEY_GRIP_SIZE);
            pgp_key_t *subkey = NULL;
            for (list_item *subkey_item = list_front(rnp_key_store_get_keys(key_store));
                 subkey_item;
                 subkey_item = list_next(subkey_item)) {
                pgp_key_t *candidate = (pgp_key_t *) subkey_item;
                if (pgp_key_is_secret(candidate) != secret) {
                    continue;
                }
                if (rnp_key_matches_search(candidate, &search)) {
                    subkey = candidate;
                    break;
                }
            }
            if (!subkey) {
                RNP_LOG("Missing subkey");
                continue;
            }
            if (!rnp_key_write_packets_stream(subkey, dst)) {
                return false;
            }
        }
    }
    return true;
}

bool
rnp_key_store_pgp_write_to_dst(rnp_key_store_t *key_store, pgp_dest_t *dst)
{
    // two separate passes (public keys, then secret keys)
    return do_write(key_store, dst, false) && do_write(key_store, dst, true);
}
