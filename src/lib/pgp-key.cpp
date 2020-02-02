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

#include "pgp-key.h"
#include "utils.h"
#include <librekey/key_store_pgp.h>
#include <librekey/key_store_g10.h>
#include "crypto.h"
#include "crypto/s2k.h"
#include "fingerprint.h"

#include <rnp/rnp_sdk.h>
#include <librepgp/stream-packet.h>
#include <librepgp/stream-key.h>
#include <librepgp/stream-sig.h>
#include <librepgp/stream-armor.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include "defaults.h"

static bool
pgp_user_prefs_set_arr(uint8_t **arr, size_t *arrlen, const uint8_t *val, size_t len)
{
    uint8_t *newarr = (uint8_t *) malloc(len);

    if (len && !newarr) {
        return false;
    }

    free(*arr);
    memcpy(newarr, val, len);
    *arrlen = len;
    *arr = newarr;
    return true;
}

static bool
pgp_user_prefs_add_val(uint8_t **arr, size_t *arrlen, uint8_t val)
{
    /* do not add duplicate values */
    for (size_t i = 0; i < *arrlen; i++) {
        if ((*arr)[i] == val) {
            return true;
        }
    }

    uint8_t *newarr = (uint8_t *) realloc(*arr, *arrlen + 1);

    if (!newarr) {
        return false;
    }

    newarr[(*arrlen)++] = val;
    *arr = newarr;
    return true;
}

bool
pgp_user_prefs_set_symm_algs(pgp_user_prefs_t *prefs, const uint8_t *algs, size_t len)
{
    return pgp_user_prefs_set_arr(&prefs->symm_algs, &prefs->symm_alg_count, algs, len);
}

bool
pgp_user_prefs_set_hash_algs(pgp_user_prefs_t *prefs, const uint8_t *algs, size_t len)
{
    return pgp_user_prefs_set_arr(&prefs->hash_algs, &prefs->hash_alg_count, algs, len);
}

bool
pgp_user_prefs_set_z_algs(pgp_user_prefs_t *prefs, const uint8_t *algs, size_t len)
{
    return pgp_user_prefs_set_arr(&prefs->z_algs, &prefs->z_alg_count, algs, len);
}

bool
pgp_user_prefs_set_ks_prefs(pgp_user_prefs_t *prefs, const uint8_t *vals, size_t len)
{
    return pgp_user_prefs_set_arr(&prefs->ks_prefs, &prefs->ks_pref_count, vals, len);
}

bool
pgp_user_prefs_add_symm_alg(pgp_user_prefs_t *prefs, pgp_symm_alg_t alg)
{
    return pgp_user_prefs_add_val(&prefs->symm_algs, &prefs->symm_alg_count, alg);
}

bool
pgp_user_prefs_add_hash_alg(pgp_user_prefs_t *prefs, pgp_hash_alg_t alg)
{
    return pgp_user_prefs_add_val(&prefs->hash_algs, &prefs->hash_alg_count, alg);
}

bool
pgp_user_prefs_add_z_alg(pgp_user_prefs_t *prefs, pgp_compression_type_t alg)
{
    return pgp_user_prefs_add_val(&prefs->z_algs, &prefs->z_alg_count, alg);
}

bool
pgp_user_prefs_add_ks_pref(pgp_user_prefs_t *prefs, pgp_key_server_prefs_t val)
{
    return pgp_user_prefs_add_val(&prefs->ks_prefs, &prefs->ks_pref_count, val);
}

void
pgp_free_user_prefs(pgp_user_prefs_t *prefs)
{
    if (!prefs) {
        return;
    }
    free(prefs->symm_algs);
    free(prefs->hash_algs);
    free(prefs->z_algs);
    free(prefs->ks_prefs);
    free(prefs->key_server);
    memset(prefs, 0, sizeof(*prefs));
}

void
pgp_subsig_free(pgp_subsig_t *subsig)
{
    if (!subsig) {
        return;
    }
    pgp_free_user_prefs(&subsig->prefs);
    free_signature(&subsig->sig);
}

static void
revoke_free(pgp_revoke_t *revoke)
{
    if (!revoke) {
        return;
    }
    free(revoke->reason);
    revoke->reason = NULL;
}

/**
   \ingroup HighLevel_Keyring

   \brief Creates a new pgp_key_t struct

   \return A new pgp_key_t struct, initialised to zero.

   \note The returned pgp_key_t struct must be freed after use with pgp_key_free.
*/

pgp_key_t *
pgp_key_new(void)
{
    return (pgp_key_t *) calloc(1, sizeof(pgp_key_t));
}

static void
pgp_userid_free(pgp_userid_t *uid)
{
    free_userid_pkt(&uid->pkt);
    free(uid->str);
}

static void
pgp_rawpacket_free(pgp_rawpacket_t *packet)
{
    free(packet->raw);
    packet->raw = NULL;
}

bool
pgp_key_from_pkt(pgp_key_t *key, const pgp_key_pkt_t *pkt, const pgp_pkt_type_t tag)
{
    assert(!key->pkt.version);
    assert(is_key_pkt(tag));
    assert(pkt->material.alg);
    if (pgp_keyid(key->keyid, PGP_KEY_ID_SIZE, pkt) ||
        pgp_fingerprint(&key->fingerprint, pkt) ||
        !rnp_key_store_get_key_grip(&pkt->material, key->grip)) {
        return false;
    }
    /* this is correct since changes ownership */
    key->pkt = *pkt;
    key->pkt.tag = tag;
    return true;
}

void
pgp_key_free_data(pgp_key_t *key)
{
    unsigned n;

    if (key == NULL) {
        return;
    }

    for (n = 0; n < pgp_key_get_userid_count(key); ++n) {
        pgp_userid_free(pgp_key_get_userid(key, n));
    }
    list_destroy(&key->uids);

    for (n = 0; n < pgp_key_get_rawpacket_count(key); ++n) {
        pgp_rawpacket_free(pgp_key_get_rawpacket(key, n));
    }
    list_destroy(&key->packets);

    for (n = 0; n < pgp_key_get_subsig_count(key); n++) {
        pgp_subsig_free(pgp_key_get_subsig(key, n));
    }
    list_destroy(&key->subsigs);

    for (n = 0; n < pgp_key_get_revoke_count(key); n++) {
        revoke_free(pgp_key_get_revoke(key, n));
    }
    list_destroy(&key->revokes);
    revoke_free(&key->revocation);
    free(key->primary_grip);
    key->primary_grip = NULL;
    list_destroy(&key->subkey_grips);
    free_key_pkt(&key->pkt);
}

void
pgp_key_free(pgp_key_t *key)
{
    pgp_key_free_data(key);
    free(key);
}

/**
 * @brief Copy key's raw packets. If pubonly is true then dst->pkt must be populated
 */
static rnp_result_t
pgp_key_copy_raw_packets(pgp_key_t *dst, const pgp_key_t *src, bool pubonly)
{
    size_t start = 0;

    if (pubonly) {
        if (!pgp_key_add_key_rawpacket(dst, &dst->pkt)) {
            return RNP_ERROR_OUT_OF_MEMORY;
        }
        start = 1;
    }

    for (size_t i = start; i < pgp_key_get_rawpacket_count(src); i++) {
        pgp_rawpacket_t *pkt = pgp_key_get_rawpacket(src, i);
        if (!pgp_key_add_rawpacket(dst, pkt->raw, pkt->length, pkt->tag)) {
            return RNP_ERROR_OUT_OF_MEMORY;
        }
    }

    return RNP_SUCCESS;
}

static rnp_result_t
pgp_key_copy_g10(pgp_key_t *dst, const pgp_key_t *src, bool pubonly)
{
    rnp_result_t ret = RNP_ERROR_GENERIC;

    if (pubonly) {
        RNP_LOG("attempt to copy public part from g10 key");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    memset(dst, 0, sizeof(*dst));

    if (pgp_key_get_rawpacket_count(src) != 1) {
        RNP_LOG("wrong g10 key packets");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    if (!copy_key_pkt(&dst->pkt, &src->pkt, false)) {
        RNP_LOG("failed to copy key pkt");
        return RNP_ERROR_BAD_PARAMETERS;
    }

    if (pgp_key_copy_fields(dst, src)) {
        RNP_LOG("failed to copy key fields");
        goto done;
    }

    if (pgp_key_copy_raw_packets(dst, src, false)) {
        RNP_LOG("failed to copy raw packets");
        goto done;
    }

    dst->format = PGP_KEY_STORE_G10;
    ret = RNP_SUCCESS;
done:
    if (ret) {
        pgp_key_free_data(dst);
    }
    return ret;
}

rnp_result_t
pgp_key_copy(pgp_key_t *dst, const pgp_key_t *src, bool pubonly)
{
    rnp_result_t ret = RNP_ERROR_GENERIC;
    rnp_result_t tmpret;
    memset(dst, 0, sizeof(*dst));

    if (src->format == PGP_KEY_STORE_G10) {
        return pgp_key_copy_g10(dst, src, pubonly);
    }

    if (!copy_key_pkt(&dst->pkt, &src->pkt, pubonly)) {
        RNP_LOG("failed to copy key pkt");
        goto error;
    }

    if ((tmpret = pgp_key_copy_fields(dst, src))) {
        ret = tmpret;
        goto error;
    }

    if ((tmpret = pgp_key_copy_raw_packets(dst, src, pubonly))) {
        ret = tmpret;
        goto error;
    }

    return RNP_SUCCESS;
error:
    pgp_key_free_data(dst);
    return ret;
}

static rnp_result_t
pgp_userprefs_copy(pgp_user_prefs_t *dst, const pgp_user_prefs_t *src)
{
    rnp_result_t ret = RNP_ERROR_OUT_OF_MEMORY;

    memset(dst, 0, sizeof(*dst));
    if (src->symm_alg_count &&
        !pgp_user_prefs_set_symm_algs(dst, src->symm_algs, src->symm_alg_count)) {
        return ret;
    }

    if (src->hash_alg_count &&
        !pgp_user_prefs_set_hash_algs(dst, src->hash_algs, src->hash_alg_count)) {
        goto error;
    }

    if (src->z_alg_count && !pgp_user_prefs_set_z_algs(dst, src->z_algs, src->z_alg_count)) {
        goto error;
    }

    if (src->ks_pref_count &&
        !pgp_user_prefs_set_ks_prefs(dst, src->ks_prefs, src->ks_pref_count)) {
        goto error;
    }

    if (src->key_server) {
        size_t len = strlen((char *) src->key_server) + 1;
        dst->key_server = (uint8_t *) malloc(len);
        if (!dst->key_server) {
            goto error;
        }
        memcpy(dst->key_server, src->key_server, len);
    }

    return RNP_SUCCESS;
error:
    pgp_free_user_prefs(dst);
    return ret;
}

static rnp_result_t
pgp_subsig_copy(pgp_subsig_t *dst, const pgp_subsig_t *src)
{
    memcpy(dst, src, sizeof(*dst));
    /* signature packet */
    if (!copy_signature_packet(&dst->sig, &src->sig)) {
        memset(dst, 0, sizeof(*dst));
        return RNP_ERROR_GENERIC;
    }
    /* user prefs */
    if (pgp_userprefs_copy(&dst->prefs, &src->prefs)) {
        free_signature(&dst->sig);
        memset(dst, 0, sizeof(*dst));
        return RNP_ERROR_GENERIC;
    }

    return RNP_SUCCESS;
}

static rnp_result_t
pgp_revoke_copy(pgp_revoke_t *dst, const pgp_revoke_t *src)
{
    memcpy(dst, src, sizeof(*dst));
    if (src->reason) {
        size_t len = strlen(src->reason) + 1;
        dst->reason = (char *) malloc(len);
        if (!dst->reason) {
            return RNP_ERROR_OUT_OF_MEMORY;
        }
        memcpy(dst->reason, src->reason, len);
    }
    return RNP_SUCCESS;
}

static rnp_result_t
pgp_userid_copy(pgp_userid_t *dst, const pgp_userid_t *src)
{
    memset(dst, 0, sizeof(*dst));
    if (src->str) {
        dst->str = strdup(src->str);
        if (!dst->str) {
            return RNP_ERROR_OUT_OF_MEMORY;
        }
    }
    if (!copy_userid_pkt(&dst->pkt, &src->pkt)) {
        free(dst->str);
        dst->str = NULL;
        return RNP_ERROR_GENERIC;
    }
    return RNP_SUCCESS;
}

rnp_result_t
pgp_key_copy_fields(pgp_key_t *dst, const pgp_key_t *src)
{
    rnp_result_t ret = RNP_ERROR_OUT_OF_MEMORY;
    rnp_result_t tmpret;

    /* uids */
    for (size_t i = 0; i < pgp_key_get_userid_count(src); i++) {
        pgp_userid_t *uid = pgp_key_add_userid(dst);
        if (!uid) {
            goto error;
        }
        if ((tmpret = pgp_userid_copy(uid, pgp_key_get_userid(src, i)))) {
            ret = tmpret;
            goto error;
        }
    }

    /* signatures */
    for (size_t i = 0; i < pgp_key_get_subsig_count(src); i++) {
        pgp_subsig_t *subsig = pgp_key_add_subsig(dst);
        if (!subsig) {
            goto error;
        }
        if ((tmpret = pgp_subsig_copy(subsig, pgp_key_get_subsig(src, i)))) {
            ret = tmpret;
            goto error;
        }
    }

    /* revocations */
    for (size_t i = 0; i < pgp_key_get_revoke_count(src); i++) {
        pgp_revoke_t *revoke = pgp_key_add_revoke(dst);
        if (!revoke) {
            goto error;
        }
        if ((tmpret = pgp_revoke_copy(revoke, pgp_key_get_revoke(src, i)))) {
            ret = tmpret;
            goto error;
        }
    }

    /* subkey grips */
    for (list_item *grip = list_front(src->subkey_grips); grip; grip = list_next(grip)) {
        if (!list_append(&dst->subkey_grips, grip, PGP_KEY_GRIP_SIZE)) {
            goto error;
        }
    }

    /* primary grip */
    if (src->primary_grip && !pgp_key_set_primary_grip(dst, pgp_key_get_primary_grip(src))) {
        goto error;
    }

    /* expiration */
    dst->expiration = src->expiration;

    /* key_flags */
    dst->key_flags = src->key_flags;

    /* key id / fingerprint / grip */
    memcpy(dst->keyid, src->keyid, sizeof(dst->keyid));
    memcpy(&dst->fingerprint, &src->fingerprint, sizeof(dst->fingerprint));
    memcpy(&dst->grip, &src->grip, sizeof(dst->grip));

    /* primary uid */
    dst->uid0 = src->uid0;
    dst->uid0_set = src->uid0_set;

    /* revocation */
    dst->revoked = src->revoked;
    tmpret = pgp_revoke_copy(&dst->revocation, &src->revocation);
    if (tmpret) {
        goto error;
    }

    /* key store format */
    dst->format = src->format;

    /* key validity */
    dst->valid = src->valid;
    dst->validated = src->validated;

    return RNP_SUCCESS;
error:
    pgp_key_free_data(dst);
    return ret;
}

/**
 \ingroup HighLevel_KeyGeneral

 \brief Returns the public key in the given key.
 \param key

  \return Pointer to public key

  \note This is not a copy, do not free it after use.
*/

const pgp_key_pkt_t *
pgp_key_get_pkt(const pgp_key_t *key)
{
    return &key->pkt;
}

const pgp_key_material_t *
pgp_key_get_material(const pgp_key_t *key)
{
    return &key->pkt.material;
}

pgp_pubkey_alg_t
pgp_key_get_alg(const pgp_key_t *key)
{
    return key->pkt.alg;
}

size_t
pgp_key_get_dsa_qbits(const pgp_key_t *key)
{
    if (pgp_key_get_alg(key) != PGP_PKA_DSA) {
        return 0;
    }

    return dsa_qbits(&pgp_key_get_material(key)->dsa);
}

size_t
pgp_key_get_bits(const pgp_key_t *key)
{
    return key_bitlength(pgp_key_get_material(key));
}

pgp_curve_t
pgp_key_get_curve(const pgp_key_t *key)
{
    switch (pgp_key_get_alg(key)) {
    case PGP_PKA_ECDH:
    case PGP_PKA_ECDSA:
    case PGP_PKA_EDDSA:
    case PGP_PKA_SM2:
        return pgp_key_get_material(key)->ec.curve;
    default:
        return PGP_CURVE_UNKNOWN;
    }
}

pgp_version_t
pgp_key_get_version(const pgp_key_t *key)
{
    return key->pkt.version;
}

pgp_pkt_type_t
pgp_key_get_type(const pgp_key_t *key)
{
    return key->pkt.tag;
}

bool
pgp_key_is_public(const pgp_key_t *key)
{
    return is_public_key_pkt(key->pkt.tag);
}

bool
pgp_key_is_secret(const pgp_key_t *key)
{
    return is_secret_key_pkt(key->pkt.tag);
}

bool
pgp_key_is_encrypted(const pgp_key_t *key)
{
    if (!pgp_key_is_secret(key)) {
        return false;
    }

    const pgp_key_pkt_t *pkt = pgp_key_get_pkt(key);
    return !pkt->material.secret;
}

uint8_t
pgp_key_get_flags(const pgp_key_t *key)
{
    return key->key_flags;
}

bool
pgp_key_can_sign(const pgp_key_t *key)
{
    return pgp_key_get_flags(key) & PGP_KF_SIGN;
}

bool
pgp_key_can_certify(const pgp_key_t *key)
{
    return pgp_key_get_flags(key) & PGP_KF_CERTIFY;
}

bool
pgp_key_can_encrypt(const pgp_key_t *key)
{
    return pgp_key_get_flags(key) & PGP_KF_ENCRYPT;
}

bool
pgp_key_is_primary_key(const pgp_key_t *key)
{
    return is_primary_key_pkt(key->pkt.tag);
}

bool
pgp_key_is_subkey(const pgp_key_t *key)
{
    return is_subkey_pkt(key->pkt.tag);
}

uint32_t
pgp_key_get_expiration(const pgp_key_t *key)
{
    return (key->pkt.version >= 4) ? key->expiration : key->pkt.v3_days * 86400;
}

uint32_t
pgp_key_get_creation(const pgp_key_t *key)
{
    return key->pkt.creation_time;
}

pgp_key_pkt_t *
pgp_decrypt_seckey_pgp(const uint8_t *      data,
                       size_t               data_len,
                       const pgp_key_pkt_t *pubkey,
                       const char *         password)
{
    pgp_source_t   src = {0};
    pgp_key_pkt_t *res = (pgp_key_pkt_t *) calloc(1, sizeof(*res));
    if (!res) {
        return NULL;
    }

    if (init_mem_src(&src, data, data_len, false)) {
        free(res);
        return NULL;
    }

    if (stream_parse_key(&src, res)) {
        goto error;
    }

    if (decrypt_secret_key(res, password)) {
        goto error;
    }

    src_close(&src);
    return res;
error:
    src_close(&src);
    free_key_pkt(res);
    free(res);
    return NULL;
}

/* Note that this function essentially serves two purposes.
 * - In the case of a protected key, it requests a password and
 *   uses it to decrypt the key and fill in key->key.seckey.
 * - In the case of an unprotected key, it simply re-loads
 *   key->key.seckey by parsing the key data in packets[0].
 */
pgp_key_pkt_t *
pgp_decrypt_seckey(const pgp_key_t *              key,
                   const pgp_password_provider_t *provider,
                   const pgp_password_ctx_t *     ctx)
{
    pgp_key_pkt_t *               decrypted_seckey = NULL;
    typedef struct pgp_key_pkt_t *pgp_seckey_decrypt_t(
      const uint8_t *data, size_t data_len, const pgp_key_pkt_t *pubkey, const char *password);
    pgp_seckey_decrypt_t *decryptor = NULL;
    pgp_rawpacket_t *     packet = NULL;
    char                  password[MAX_PASSWORD_LENGTH] = {0};

    // sanity checks
    if (!key || !pgp_key_is_secret(key) || !provider) {
        RNP_LOG("invalid args");
        goto done;
    }
    switch (key->format) {
    case PGP_KEY_STORE_GPG:
    case PGP_KEY_STORE_KBX:
        decryptor = pgp_decrypt_seckey_pgp;
        break;
    case PGP_KEY_STORE_G10:
        decryptor = g10_decrypt_seckey;
        break;
    default:
        RNP_LOG("unexpected format: %d", key->format);
        goto done;
        break;
    }
    if (!decryptor) {
        RNP_LOG("missing decrypt callback");
        goto done;
    }

    if (pgp_key_is_protected(key)) {
        // ask the provider for a password
        if (!pgp_request_password(provider, ctx, password, sizeof(password))) {
            goto done;
        }
    }
    // attempt to decrypt with the provided password
    packet = pgp_key_get_rawpacket(key, 0);
    decrypted_seckey = decryptor(packet->raw, packet->length, pgp_key_get_pkt(key), password);

done:
    pgp_forget(password, sizeof(password));
    return decrypted_seckey;
}

const uint8_t *
pgp_key_get_keyid(const pgp_key_t *key)
{
    return key->keyid;
}

const pgp_fingerprint_t *
pgp_key_get_fp(const pgp_key_t *key)
{
    return &key->fingerprint;
}

const uint8_t *
pgp_key_get_grip(const pgp_key_t *key)
{
    return key->grip;
}

const uint8_t *
pgp_key_get_primary_grip(const pgp_key_t *key)
{
    return key->primary_grip;
}

bool
pgp_key_set_primary_grip(pgp_key_t *key, const uint8_t *grip)
{
    key->primary_grip = (uint8_t *) malloc(PGP_KEY_GRIP_SIZE);
    if (!key->primary_grip) {
        RNP_LOG("alloc failed");
        return false;
    }
    memcpy(key->primary_grip, grip, PGP_KEY_GRIP_SIZE);
    return true;
}

bool
pgp_key_link_subkey_grip(pgp_key_t *key, pgp_key_t *subkey)
{
    if (!pgp_key_set_primary_grip(subkey, pgp_key_get_grip(key))) {
        RNP_LOG("failed to set primary grip");
        return false;
    }
    if (!pgp_key_add_subkey_grip(key, pgp_key_get_grip(subkey))) {
        RNP_LOG("failed to add subkey grip");
        return false;
    }
    return true;
}

/**
\ingroup Core_Keys
\brief How many User IDs in this key?
\param key Key to check
\return Num of user ids
*/
size_t
pgp_key_get_userid_count(const pgp_key_t *key)
{
    return list_length(key->uids);
}

pgp_userid_t *
pgp_key_get_userid(const pgp_key_t *key, size_t idx)
{
    return (pgp_userid_t *) list_at(key->uids, idx);
}

pgp_revoke_t *
pgp_key_get_userid_revoke(const pgp_key_t *key, size_t uid)
{
    for (size_t i = 0; i < pgp_key_get_revoke_count(key); i++) {
        pgp_revoke_t *revoke = pgp_key_get_revoke(key, i);
        if (revoke->uid == uid) {
            return revoke;
        }
    }
    return NULL;
}

bool
pgp_key_has_userid(const pgp_key_t *key, const char *uid)
{
    for (list_item *li = list_front(key->uids); li; li = list_next(li)) {
        pgp_userid_t *userid = (pgp_userid_t *) li;
        if (!strcmp(uid, userid->str)) {
            return true;
        }
    }

    return false;
}

pgp_userid_t *
pgp_key_add_userid(pgp_key_t *key)
{
    return (pgp_userid_t *) list_append(&key->uids, NULL, sizeof(pgp_userid_t));
}

pgp_revoke_t *
pgp_key_add_revoke(pgp_key_t *key)
{
    return (pgp_revoke_t *) list_append(&key->revokes, NULL, sizeof(pgp_revoke_t));
}

size_t
pgp_key_get_revoke_count(const pgp_key_t *key)
{
    return list_length(key->revokes);
}

pgp_revoke_t *
pgp_key_get_revoke(const pgp_key_t *key, size_t idx)
{
    return (pgp_revoke_t *) list_at(key->revokes, idx);
}

pgp_subsig_t *
pgp_key_add_subsig(pgp_key_t *key)
{
    return (pgp_subsig_t *) list_append(&key->subsigs, NULL, sizeof(pgp_subsig_t));
}

size_t
pgp_key_get_subsig_count(const pgp_key_t *key)
{
    return list_length(key->subsigs);
}

pgp_subsig_t *
pgp_key_get_subsig(const pgp_key_t *key, size_t idx)
{
    return (pgp_subsig_t *) list_at(key->subsigs, idx);
}

pgp_rawpacket_t *
pgp_key_add_rawpacket(pgp_key_t *key, void *data, size_t len, pgp_pkt_type_t tag)
{
    pgp_rawpacket_t *packet;
    if (!(packet = (pgp_rawpacket_t *) list_append(&key->packets, NULL, sizeof(*packet)))) {
        return NULL;
    }
    if (data) {
        if (!(packet->raw = (uint8_t *) malloc(len))) {
            list_remove((list_item *) packet);
            return NULL;
        }
        memcpy(packet->raw, data, len);
    }
    packet->length = len;
    packet->tag = tag;
    return packet;
}

pgp_rawpacket_t *
pgp_key_add_stream_rawpacket(pgp_key_t *key, pgp_pkt_type_t tag, pgp_dest_t *memdst)
{
    pgp_rawpacket_t *res =
      pgp_key_add_rawpacket(key, mem_dest_get_memory(memdst), memdst->writeb, tag);
    if (!res) {
        RNP_LOG("Failed to add packet");
    }
    dst_close(memdst, true);
    return res;
}

pgp_rawpacket_t *
pgp_key_add_key_rawpacket(pgp_key_t *key, pgp_key_pkt_t *pkt)
{
    pgp_dest_t dst = {};

    if (init_mem_dest(&dst, NULL, 0)) {
        return NULL;
    }
    if (!stream_write_key(pkt, &dst)) {
        dst_close(&dst, true);
        return NULL;
    }
    return pgp_key_add_stream_rawpacket(key, (pgp_pkt_type_t) pkt->tag, &dst);
}

pgp_rawpacket_t *
pgp_key_add_sig_rawpacket(pgp_key_t *key, const pgp_signature_t *pkt)
{
    pgp_dest_t dst = {};

    if (init_mem_dest(&dst, NULL, 0)) {
        return NULL;
    }
    if (!stream_write_signature(pkt, &dst)) {
        dst_close(&dst, true);
        return NULL;
    }
    return pgp_key_add_stream_rawpacket(key, PGP_PKT_SIGNATURE, &dst);
}

pgp_rawpacket_t *
pgp_key_add_uid_rawpacket(pgp_key_t *key, const pgp_userid_pkt_t *pkt)
{
    pgp_dest_t dst = {};

    if (init_mem_dest(&dst, NULL, 0)) {
        return NULL;
    }
    if (!stream_write_userid(pkt, &dst)) {
        dst_close(&dst, true);
        return NULL;
    }
    return pgp_key_add_stream_rawpacket(key, (pgp_pkt_type_t) pkt->tag, &dst);
}

size_t
pgp_key_get_rawpacket_count(const pgp_key_t *key)
{
    return list_length(key->packets);
}

pgp_rawpacket_t *
pgp_key_get_rawpacket(const pgp_key_t *key, size_t idx)
{
    return (pgp_rawpacket_t *) list_at(key->packets, idx);
}

size_t
pgp_key_get_subkey_count(const pgp_key_t *key)
{
    return list_length(key->subkey_grips);
}

bool
pgp_key_add_subkey_grip(pgp_key_t *key, const uint8_t *grip)
{
    for (list_item *li = list_front(key->subkey_grips); li; li = list_next(li)) {
        if (!memcmp(grip, (uint8_t *) li, PGP_KEY_GRIP_SIZE)) {
            return true;
        }
    }

    return list_append(&key->subkey_grips, grip, PGP_KEY_GRIP_SIZE);
}

const uint8_t *
pgp_key_get_subkey_grip(const pgp_key_t *key, size_t idx)
{
    return (uint8_t *) list_at(key->subkey_grips, idx);
}

pgp_key_t *
pgp_key_get_subkey(const pgp_key_t *key, const rnp_key_store_t *store, size_t idx)
{
    uint8_t *grip = (uint8_t *) list_at(key->subkey_grips, idx);
    if (!grip) {
        return NULL;
    }
    return rnp_key_store_get_key_by_grip(store, grip);
}

pgp_key_flags_t
pgp_pk_alg_capabilities(pgp_pubkey_alg_t alg)
{
    switch (alg) {
    case PGP_PKA_RSA:
        return pgp_key_flags_t(PGP_KF_SIGN | PGP_KF_CERTIFY | PGP_KF_AUTH | PGP_KF_ENCRYPT);

    case PGP_PKA_RSA_SIGN_ONLY:
        // deprecated, but still usable
        return PGP_KF_SIGN;

    case PGP_PKA_RSA_ENCRYPT_ONLY:
        // deprecated, but still usable
        return PGP_KF_ENCRYPT;

    case PGP_PKA_ELGAMAL_ENCRYPT_OR_SIGN: /* deprecated */
        // These are no longer permitted per the RFC
        return PGP_KF_NONE;

    case PGP_PKA_DSA:
    case PGP_PKA_ECDSA:
    case PGP_PKA_EDDSA:
        return pgp_key_flags_t(PGP_KF_SIGN | PGP_KF_CERTIFY | PGP_KF_AUTH);

    case PGP_PKA_SM2:
        return pgp_key_flags_t(PGP_KF_SIGN | PGP_KF_CERTIFY | PGP_KF_AUTH | PGP_KF_ENCRYPT);

    case PGP_PKA_ECDH:
    case PGP_PKA_ELGAMAL:
        return PGP_KF_ENCRYPT;

    default:
        RNP_LOG("unknown pk alg: %d\n", alg);
        return PGP_KF_NONE;
    }
}

bool
pgp_key_is_locked(const pgp_key_t *key)
{
    if (!pgp_key_is_secret(key)) {
        RNP_LOG("key is not a secret key");
        return false;
    }
    return pgp_key_is_encrypted(key);
}

bool
pgp_key_unlock(pgp_key_t *key, const pgp_password_provider_t *provider)
{
    pgp_key_pkt_t *decrypted_seckey = NULL;

    // sanity checks
    if (!key || !provider) {
        return false;
    }
    if (!pgp_key_is_secret(key)) {
        RNP_LOG("key is not a secret key");
        return false;
    }

    // see if it's already unlocked
    if (!pgp_key_is_locked(key)) {
        return true;
    }

    pgp_password_ctx_t ctx = {.op = PGP_OP_UNLOCK, .key = key};
    decrypted_seckey = pgp_decrypt_seckey(key, provider, &ctx);

    if (decrypted_seckey) {
        // this shouldn't really be necessary, but just in case
        forget_secret_key_fields(&key->pkt.material);
        // copy the decrypted mpis into the pgp_key_t
        key->pkt.material = decrypted_seckey->material;
        key->pkt.material.secret = true;

        free_key_pkt(decrypted_seckey);
        // free the actual structure
        free(decrypted_seckey);
        return true;
    }
    return false;
}

bool
pgp_key_lock(pgp_key_t *key)
{
    // sanity checks
    if (!key || !pgp_key_is_secret(key)) {
        RNP_LOG("invalid args");
        return false;
    }

    // see if it's already locked
    if (pgp_key_is_locked(key)) {
        return true;
    }

    forget_secret_key_fields(&key->pkt.material);
    return true;
}

static bool
pgp_write_seckey(pgp_dest_t *   dst,
                 pgp_pkt_type_t tag,
                 pgp_key_pkt_t *seckey,
                 const char *   password)
{
    bool           res = false;
    pgp_pkt_type_t oldtag = seckey->tag;

    seckey->tag = tag;
    if (encrypt_secret_key(seckey, password, NULL)) {
        goto done;
    }
    res = stream_write_key(seckey, dst);
done:
    seckey->tag = oldtag;
    return res;
}

static bool
write_key_to_rawpacket(pgp_key_pkt_t *        seckey,
                       pgp_rawpacket_t *      packet,
                       pgp_pkt_type_t         type,
                       pgp_key_store_format_t format,
                       const char *           password)
{
    pgp_dest_t memdst = {};
    bool       ret = false;

    if (init_mem_dest(&memdst, NULL, 0)) {
        goto done;
    }

    // encrypt+write the key in the appropriate format
    switch (format) {
    case PGP_KEY_STORE_GPG:
    case PGP_KEY_STORE_KBX:
        if (!pgp_write_seckey(&memdst, type, seckey, password)) {
            RNP_LOG("failed to write seckey");
            goto done;
        }
        break;
    case PGP_KEY_STORE_G10:
        if (!g10_write_seckey(&memdst, seckey, password)) {
            RNP_LOG("failed to write g10 seckey");
            goto done;
        }
        break;
    default:
        RNP_LOG("invalid format");
        goto done;
        break;
    }
    // free the existing data if needed
    pgp_rawpacket_free(packet);
    // take ownership of this memory
    packet->raw = (uint8_t *) mem_dest_own_memory(&memdst);
    packet->length = memdst.writeb;
    ret = true;
done:
    dst_close(&memdst, true);
    return ret;
}

bool
rnp_key_add_protection(pgp_key_t *                    key,
                       pgp_key_store_format_t         format,
                       rnp_key_protection_params_t *  protection,
                       const pgp_password_provider_t *password_provider)
{
    char password[MAX_PASSWORD_LENGTH] = {0};

    if (!key || !password_provider) {
        return false;
    }

    pgp_password_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.op = PGP_OP_PROTECT;
    ctx.key = key;

    // ask the provider for a password
    if (!pgp_request_password(password_provider, &ctx, password, sizeof(password))) {
        return false;
    }

    bool ret = pgp_key_protect(key, &key->pkt, format, protection, password);
    pgp_forget(password, sizeof(password));
    return ret;
}

bool
pgp_key_protect(pgp_key_t *                  key,
                pgp_key_pkt_t *              decrypted_seckey,
                pgp_key_store_format_t       format,
                rnp_key_protection_params_t *protection,
                const char *                 new_password)
{
    bool                        ret = false;
    rnp_key_protection_params_t default_protection = {.symm_alg = DEFAULT_PGP_SYMM_ALG,
                                                      .cipher_mode = DEFAULT_PGP_CIPHER_MODE,
                                                      .iterations = 0,
                                                      .hash_alg = DEFAULT_PGP_HASH_ALG};
    pgp_key_pkt_t *             seckey = NULL;

    // sanity check
    if (!key || !decrypted_seckey || !new_password) {
        RNP_LOG("NULL args");
        goto done;
    }
    if (!pgp_key_is_secret(key)) {
        RNP_LOG("Warning: this is not a secret key");
        goto done;
    }
    if (!decrypted_seckey->material.secret) {
        RNP_LOG("Decrypted seckey must be provided");
        goto done;
    }

    seckey = &key->pkt;
    // force these, as it's the only method we support
    seckey->sec_protection.s2k.usage = PGP_S2KU_ENCRYPTED_AND_HASHED;
    seckey->sec_protection.s2k.specifier = PGP_S2KS_ITERATED_AND_SALTED;

    if (!protection) {
        protection = &default_protection;
    }

    if (!protection->symm_alg) {
        protection->symm_alg = default_protection.symm_alg;
    }
    if (!protection->cipher_mode) {
        protection->cipher_mode = default_protection.cipher_mode;
    }
    if (!protection->hash_alg) {
        protection->hash_alg = default_protection.hash_alg;
    }
    if (!protection->iterations) {
        protection->iterations =
          pgp_s2k_compute_iters(protection->hash_alg, DEFAULT_S2K_MSEC, DEFAULT_S2K_TUNE_MSEC);
    }

    seckey->sec_protection.symm_alg = protection->symm_alg;
    seckey->sec_protection.cipher_mode = protection->cipher_mode;
    seckey->sec_protection.s2k.iterations = pgp_s2k_round_iterations(protection->iterations);
    seckey->sec_protection.s2k.hash_alg = protection->hash_alg;

    // write the protected key to packets[0]
    if (!write_key_to_rawpacket(decrypted_seckey,
                                pgp_key_get_rawpacket(key, 0),
                                pgp_key_get_type(key),
                                format,
                                new_password)) {
        goto done;
    }
    key->format = format;
    ret = true;

done:
    return ret;
}

bool
pgp_key_unprotect(pgp_key_t *key, const pgp_password_provider_t *password_provider)
{
    bool           ret = false;
    pgp_key_pkt_t *seckey = NULL;
    pgp_key_pkt_t *decrypted_seckey = NULL;

    // sanity check
    if (!pgp_key_is_secret(key)) {
        RNP_LOG("Warning: this is not a secret key");
        goto done;
    }
    // already unprotected
    if (!pgp_key_is_protected(key)) {
        ret = true;
        goto done;
    }

    seckey = &key->pkt;

    if (pgp_key_is_encrypted(key)) {
        pgp_password_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.op = PGP_OP_UNPROTECT;
        ctx.key = key;

        decrypted_seckey = pgp_decrypt_seckey(key, password_provider, &ctx);
        if (!decrypted_seckey) {
            goto done;
        }
        seckey = decrypted_seckey;
    }
    seckey->sec_protection.s2k.usage = PGP_S2KU_NONE;
    if (!write_key_to_rawpacket(
          seckey, pgp_key_get_rawpacket(key, 0), pgp_key_get_type(key), key->format, NULL)) {
        goto done;
    }
    if (decrypted_seckey) {
        free_key_pkt(&key->pkt);
        copy_key_pkt(&key->pkt, decrypted_seckey, false);
        /* current logic is that unprotected key should be additionally unlocked */
        forget_secret_key_fields(&key->pkt.material);
    }
    ret = true;

done:
    free_key_pkt(decrypted_seckey);
    free(decrypted_seckey);
    return ret;
}

bool
pgp_key_is_protected(const pgp_key_t *key)
{
    // sanity check
    if (!pgp_key_is_secret(key)) {
        RNP_LOG("Warning: this is not a secret key");
    }
    return key->pkt.sec_protection.s2k.usage != PGP_S2KU_NONE;
}

bool
pgp_key_add_userid_certified(pgp_key_t *              key,
                             const pgp_key_pkt_t *    seckey,
                             pgp_hash_alg_t           hash_alg,
                             rnp_selfsig_cert_info_t *cert)
{
    bool                      ret = false;
    pgp_transferable_userid_t uid = {};

    // sanity checks
    if (!key || !seckey || !cert || !cert->userid[0]) {
        RNP_LOG("wrong parameters");
        goto done;
    }
    // userids are only valid for primary keys, not subkeys
    if (!pgp_key_is_primary_key(key)) {
        RNP_LOG("cannot add a userid to a subkey");
        goto done;
    }
    // see if the key already has this userid
    if (pgp_key_has_userid(key, (const char *) cert->userid)) {
        RNP_LOG("key already has this userid");
        goto done;
    }
    // this isn't really valid for this format
    if (key->format == PGP_KEY_STORE_G10) {
        RNP_LOG("Unsupported key store type");
        goto done;
    }
    // We only support modifying v4 and newer keys
    if (key->pkt.version < PGP_V4) {
        RNP_LOG("adding a userid to V2/V3 key is not supported");
        goto done;
    }
    // TODO: changing the primary userid is not currently supported
    if (key->uid0_set && cert->primary) {
        RNP_LOG("changing the primary userid is not supported");
        goto done;
    }

    /* Fill the transferable userid */
    uid.uid.tag = PGP_PKT_USER_ID;
    uid.uid.uid_len = strlen((char *) cert->userid);
    if (!(uid.uid.uid = (uint8_t *) malloc(uid.uid.uid_len))) {
        RNP_LOG("allocation failed");
        goto done;
    }
    /* uid.uid.uid looks really weird */
    memcpy(uid.uid.uid, (char *) cert->userid, uid.uid.uid_len);

    if (!transferable_userid_certify(seckey, &uid, seckey, hash_alg, cert)) {
        RNP_LOG("failed to add userid certification");
        goto done;
    }

    ret = rnp_key_add_transferable_userid(key, &uid);
done:
    transferable_userid_destroy(&uid);
    return ret;
}

bool
pgp_key_write_packets(const pgp_key_t *key, pgp_dest_t *dst)
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
        if (dst->werr) {
            return false;
        }
    }
    return true;
}

static bool
packet_matches(pgp_pkt_type_t tag, bool secret)
{
    switch (tag) {
    case PGP_PKT_SIGNATURE:
    case PGP_PKT_USER_ID:
    case PGP_PKT_USER_ATTR:
        return true;
    case PGP_PKT_PUBLIC_KEY:
    case PGP_PKT_PUBLIC_SUBKEY:
        return !secret;
    case PGP_PKT_SECRET_KEY:
    case PGP_PKT_SECRET_SUBKEY:
        return secret;
    default:
        return false;
    }
}

static bool
write_xfer_packets(pgp_dest_t *           dst,
                   const pgp_key_t *      key,
                   const rnp_key_store_t *keyring,
                   bool                   secret)
{
    for (size_t i = 0; i < pgp_key_get_rawpacket_count(key); i++) {
        pgp_rawpacket_t *pkt = pgp_key_get_rawpacket(key, i);

        if (!packet_matches(pkt->tag, secret)) {
            RNP_LOG("skipping packet with tag: %d", pkt->tag);
            continue;
        }
        dst_write(dst, pkt->raw, (size_t) pkt->length);
    }

    if (!keyring) {
        return !dst->werr;
    }

    // Export subkeys
    for (list_item *grip = list_front(key->subkey_grips); grip; grip = list_next(grip)) {
        const pgp_key_t *subkey = rnp_key_store_get_key_by_grip(keyring, (uint8_t *) grip);
        if (!write_xfer_packets(dst, subkey, NULL, secret)) {
            RNP_LOG("Error occured when exporting a subkey");
            return false;
        }
    }

    return !dst->werr;
}

bool
pgp_key_write_xfer(pgp_dest_t *dst, const pgp_key_t *key, const rnp_key_store_t *keyring)
{
    if (!pgp_key_get_rawpacket_count(key)) {
        return false;
    }
    return write_xfer_packets(dst, key, keyring, pgp_key_is_secret(key));
}

pgp_key_t *
find_suitable_key(pgp_op_t            op,
                  pgp_key_t *         key,
                  pgp_key_provider_t *key_provider,
                  uint8_t             desired_usage)
{
    assert(desired_usage);
    if (!key) {
        return NULL;
    }
    if (pgp_key_get_flags(key) & desired_usage) {
        return key;
    }
    list_item *           subkey_grip = list_front(key->subkey_grips);
    pgp_key_request_ctx_t ctx{.op = op, .secret = pgp_key_is_secret(key)};
    ctx.search.type = PGP_KEY_SEARCH_GRIP;

    while (subkey_grip) {
        memcpy(ctx.search.by.grip, subkey_grip, PGP_KEY_GRIP_SIZE);
        pgp_key_t *subkey = pgp_request_key(key_provider, &ctx);
        if (subkey && (pgp_key_get_flags(subkey) & desired_usage)) {
            return subkey;
        }
        subkey_grip = list_next(subkey_grip);
    }
    return NULL;
}

static const pgp_signature_t *
get_subkey_binding(const pgp_key_t *subkey)
{
    // find the subkey binding signature
    for (size_t i = 0; i < pgp_key_get_subsig_count(subkey); i++) {
        const pgp_signature_t *sig = &pgp_key_get_subsig(subkey, i)->sig;
        if (sig->type == PGP_SIG_SUBKEY) {
            return sig;
        }
    }
    return NULL;
}

static pgp_key_t *
find_signer(const pgp_signature_t *   sig,
            const rnp_key_store_t *   store,
            const pgp_key_provider_t *key_provider,
            bool                      secret)
{
    pgp_key_search_t search;
    pgp_key_t *      key = NULL;

    // prefer using the issuer fingerprint when available
    if (signature_has_keyfp(sig)) {
        search.type = PGP_KEY_SEARCH_FINGERPRINT;
        signature_get_keyfp(sig, &search.by.fingerprint);
        // search the store, if provided
        if (store && (key = rnp_key_store_search(store, &search, NULL)) &&
            pgp_key_is_secret(key) == secret) {
            return key;
        }

        pgp_key_request_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.op = PGP_OP_MERGE_INFO;
        ctx.secret = secret;
        ctx.search = search;

        // try the key provider
        if ((key = pgp_request_key(key_provider, &ctx))) {
            return key;
        }
    }
    if (signature_get_keyid(sig, search.by.keyid)) {
        search.type = PGP_KEY_SEARCH_KEYID;
        // search the store, if provided
        if (store && (key = rnp_key_store_search(store, &search, NULL)) &&
            pgp_key_is_secret(key) == secret) {
            return key;
        }

        pgp_key_request_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.op = PGP_OP_MERGE_INFO;
        ctx.secret = secret;
        ctx.search = search;

        if ((key = pgp_request_key(key_provider, &ctx))) {
            return key;
        }
    }
    return NULL;
}

/* Some background related to this function:
 * Given that
 * - It doesn't really make sense to support loading a subkey for which no primary is
 *   available, because:
 *   - We can't verify the binding signature without the primary.
 *   - The primary holds the userids.
 *   - The way we currently write keyrings out, orphan keys would be omitted.
 * - The way we maintain a link between primary and sub is via:
 *   - primary_grip in the subkey
 *   - subkey_grips in the primary
 *
 * We clearly need the primary to be available when loading a subkey.
 * Rather than requiring it to be loaded first, we just use the key provider.
 */
pgp_key_t *
pgp_get_primary_key_for(const pgp_key_t *         subkey,
                        const rnp_key_store_t *   store,
                        const pgp_key_provider_t *key_provider)
{
    const pgp_signature_t *binding_sig = NULL;

    // find the subkey binding signature
    binding_sig = get_subkey_binding(subkey);
    if (!binding_sig) {
        RNP_LOG("Missing subkey binding signature for key.");
        return NULL;
    }
    if (!signature_has_keyfp(binding_sig) && !signature_has_keyid(binding_sig)) {
        RNP_LOG("No issuer information in subkey binding signature.");
        return NULL;
    }
    return find_signer(binding_sig, store, key_provider, pgp_key_is_secret(subkey));
}

pgp_hash_alg_t
pgp_hash_adjust_alg_to_key(pgp_hash_alg_t hash, const pgp_key_pkt_t *pubkey)
{
    if ((pubkey->alg != PGP_PKA_DSA) && (pubkey->alg != PGP_PKA_ECDSA)) {
        return hash;
    }

    pgp_hash_alg_t hash_min;
    if (pubkey->alg == PGP_PKA_ECDSA) {
        hash_min = ecdsa_get_min_hash(pubkey->material.ec.curve);
    } else {
        hash_min = dsa_get_min_hash(mpi_bits(&pubkey->material.dsa.q));
    }

    if (pgp_digest_length(hash) < pgp_digest_length(hash_min)) {
        return hash_min;
    }
    return hash;
}

static bool
pgp_sig_is_certification(const pgp_subsig_t *sig)
{
    pgp_sig_type_t type = signature_get_type(&sig->sig);
    return (type == PGP_CERT_CASUAL) || (type == PGP_CERT_GENERIC) ||
           (type == PGP_CERT_PERSONA) || (type == PGP_CERT_POSITIVE);
}

static bool
pgp_sig_is_self_signature(const pgp_key_t *key, const pgp_subsig_t *sig)
{
    if (!pgp_key_is_primary_key(key) || !pgp_sig_is_certification(sig)) {
        return false;
    }

    /* if we have fingerprint let's check it */
    if (signature_has_keyfp(&sig->sig)) {
        pgp_fingerprint_t sigfp = {};
        if (signature_get_keyfp(&sig->sig, &sigfp)) {
            return fingerprint_equal(pgp_key_get_fp(key), &sigfp);
        }
    }
    if (!signature_has_keyid(&sig->sig)) {
        return false;
    }
    uint8_t sigid[PGP_KEY_ID_SIZE] = {0};
    if (!signature_get_keyid(&sig->sig, sigid)) {
        return false;
    }
    return !memcmp(pgp_key_get_keyid(key), sigid, PGP_KEY_ID_SIZE);
}

static bool
pgp_sig_is_key_revocation(const pgp_key_t *key, const pgp_subsig_t *sig)
{
    return pgp_key_is_primary_key(key) && (signature_get_type(&sig->sig) == PGP_SIG_REV_KEY);
}

static bool
pgp_sig_is_subkey_binding(const pgp_key_t *key, const pgp_subsig_t *sig)
{
    return pgp_key_is_subkey(key) && (signature_get_type(&sig->sig) == PGP_SIG_SUBKEY);
}

static bool
pgp_sig_is_subkey_revocation(const pgp_key_t *key, const pgp_subsig_t *sig)
{
    return pgp_key_is_subkey(key) && (signature_get_type(&sig->sig) == PGP_SIG_REV_SUBKEY);
}

static rnp_result_t
pgp_key_validate_primary(pgp_key_t *key)
{
    /* consider public key as valid on this level if it has at least one non-expired
     * self-signature (or it is secret), and is not revoked */
    key->valid = false;
    bool has_cert = false;
    for (size_t i = 0; i < pgp_key_get_subsig_count(key); i++) {
        pgp_subsig_t *sig = pgp_key_get_subsig(key, i);

        if (pgp_sig_is_self_signature(key, sig) && !has_cert) {
            pgp_signature_info_t sinfo = {};
            sinfo.sig = &sig->sig;
            sinfo.signer = key;
            sinfo.signer_valid = true;
            pgp_userid_t *uid = pgp_key_get_userid(key, sig->uid);
            if (uid) {
                signature_check_certification(&sinfo, pgp_key_get_pkt(key), &uid->pkt);
                has_cert = sinfo.valid && !sinfo.expired;
            }
            continue;
        }

        if (pgp_sig_is_key_revocation(key, sig)) {
            pgp_signature_info_t sinfo = {};
            sinfo.sig = &sig->sig;
            sinfo.signer = key;
            sinfo.signer_valid = true;
            signature_check_direct(&sinfo, pgp_key_get_pkt(key));
            /* revocation signature cannot expire */
            if (sinfo.valid) {
                return RNP_SUCCESS;
            }
        }
    }

    key->valid = has_cert || pgp_key_is_secret(key);
    return RNP_SUCCESS;
}

static rnp_result_t
pgp_key_validate_subkey(pgp_key_t *subkey, pgp_key_t *key)
{
    /* consider subkey as valid on this level if it has valid primary key, has at least one
     * non-expired binding signature (or is secret), and is not revoked. */
    subkey->valid = false;
    if (!key->valid) {
        return RNP_SUCCESS;
    }

    bool has_binding = false;
    for (size_t i = 0; i < pgp_key_get_subsig_count(subkey); i++) {
        pgp_subsig_t *sig = pgp_key_get_subsig(subkey, i);

        if (pgp_sig_is_subkey_binding(subkey, sig) && !has_binding) {
            pgp_signature_info_t sinfo = {};
            sinfo.sig = &sig->sig;
            sinfo.signer = key;
            sinfo.signer_valid = true;
            signature_check_binding(&sinfo, pgp_key_get_pkt(key), pgp_key_get_pkt(subkey));
            has_binding = sinfo.valid && !sinfo.expired;
            continue;
        }

        if (pgp_sig_is_subkey_revocation(subkey, sig)) {
            pgp_signature_info_t sinfo = {};
            sinfo.sig = &sig->sig;
            sinfo.signer = key;
            sinfo.signer_valid = true;
            signature_check_subkey_revocation(
              &sinfo, pgp_key_get_pkt(key), pgp_key_get_pkt(subkey));
            /* revocation signature cannot expire */
            if (sinfo.valid) {
                return RNP_SUCCESS;
            }
        }
    }

    subkey->valid = has_binding || (pgp_key_is_secret(subkey) && pgp_key_is_secret(key));
    return RNP_SUCCESS;
}

rnp_result_t
pgp_key_validate(pgp_key_t *key, rnp_key_store_t *keyring)
{
    rnp_result_t res = RNP_ERROR_GENERIC;
    pgp_key_t *  primary = NULL;

    key->valid = false;
    if (!pgp_key_is_subkey(key)) {
        res = pgp_key_validate_primary(key);
        goto done;
    }

    primary = rnp_key_store_get_key_by_grip(keyring, pgp_key_get_primary_grip(key));
    if (!primary) {
        return RNP_ERROR_BAD_PARAMETERS;
    }
    res = pgp_key_validate_subkey(key, primary);
done:
    if (!res) {
        key->validated = true;
    }
    return res;
}
