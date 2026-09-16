/* ============================================================================
 * Background sync engine — serial pthread work queue that walks profile,
 * friends, chats, messages, and media on the sign → request → decrypt → write
 * pipeline.
 * ==========================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include "enil_sync.h"
#include "enil_cocoa_progress.h"
#include "enil_line.h"
#include "enil_session.h"
#include "enil_talkserv.h"
#include "enil_api_json.h"
#include "enil_worker.h"
#include "enil_db.h"
#include "enil_obs.h"
#include "enil_cocoa_image.h"
#include "enil_http.h"
#include "enil_html.h"
#include "enil_message_format.h"
#include "enil_b64.h"
#include "enil_cocoa_log.h"
#include "cJSON.h"

#define MESSAGES_PER_CHAT 50
#define STICKER_CDN_BATCH_SIZE 10
#define LOG(fn, msg) enil_log("Sync." fn, "%s", msg)

/* Decode a chunk and interpret its bytes as a big-endian integer (parseKeyId). */
static long long parse_key_id(const char *chunk_b64) {
  unsigned char buf[32];
  int len, i;
  long long out = 0;
  if (!chunk_b64) return 0;
  len = enil_b64_decode(chunk_b64, buf);
  if (len <= 0) return 0;
  for (i = 0; i < len && i < (int)sizeof(buf); i++)
    out = out * 256 + buf[i];
  return out;
}

/* Location payloads (contentType 15) are intentionally unsupported by this
 * client (PLAN.md, "Message types to not support").  They can still carry E2EE
 * chunks, but attempting to decrypt an old one may require a peer key LINE no
 * longer serves.  Treat them as terminal non-text rows instead of feeding them
 * into the retryable decrypt queue. */
static int is_unsupported_location_message(cJSON *msg) {
  return msg &&
    enil_json_coerce_int64(cJSON_GetObjectItem(msg, "contentType")) == 15;
}

/* Build ciphertext by decoding and concatenating chunks in order, then re-encoding.
 * v1: chunks[0]+[1]+[2]    v2: chunks[0]+[2]+[1] */
static char *build_ciphertext(cJSON *chunks, int version) {
  const char *c0, *c1, *c2;
  unsigned char *buf;
  size_t buf_sz;
  int n0, n1, n2;
  char *result;

  if (!cJSON_IsArray(chunks) || cJSON_GetArraySize(chunks) < 3) return NULL;
  c0 = cJSON_GetArrayItem(chunks, 0)->valuestring;
  c1 = cJSON_GetArrayItem(chunks, 1)->valuestring;
  c2 = cJSON_GetArrayItem(chunks, 2)->valuestring;
  if (!c0 || !c1 || !c2) return NULL;

  buf_sz = (strlen(c0) + strlen(c1) + strlen(c2)) * 3 / 4 + 4;
  buf = (unsigned char *)malloc(buf_sz);
  if (!buf) return NULL;

  n0 = enil_b64_decode(c0, buf);
  if (version == 1) {
    n1 = enil_b64_decode(c1, buf + n0);
    n2 = enil_b64_decode(c2, buf + n0 + n1);
  } else {
    n2 = enil_b64_decode(c2, buf + n0);
    n1 = enil_b64_decode(c1, buf + n0 + n2);
  }

  if (n0 < 0 || n1 < 0 || n2 < 0) { free(buf); return NULL; }
  result = enil_b64_encode(buf, (size_t)(n0 + n1 + n2));
  free(buf);
  return result;
}

/* --- Session helpers --- */

/* Loads session.json into *out and exposes workerRestoreState + e2eeKeys.
 * The returned cJSON* are owned by *out — caller must enil_session_free(out).
 * Returns 0 on success. */
static int load_session(const char *session_path, session_t *out,
                        cJSON **restore_out, cJSON **keys_out)
{
  *restore_out = NULL; *keys_out = NULL;
  if (!enil_session_load(session_path, out)) return -1;
  *restore_out = out->workerRestoreState;
  *keys_out    = out->e2eeKeys;
  return 0;
}

/* Writes an updated workerRestoreState back into session.json. */
static void save_restore_state(const char *session_path,
                                session_t *session,
                                cJSON *new_state)
{
  cJSON *root;
  if (session->workerRestoreState) cJSON_Delete(session->workerRestoreState);
  session->workerRestoreState = cJSON_Duplicate(new_state, 1);
  root = enil_session_to_json(session);
  if (!root) return;
  enil_session_write(session_path, root);
  cJSON_Delete(root);
}

/* Find exportedKey for a given keyId in the e2eeKeys array. Returns NULL if missing. */
static const char *find_private_key(cJSON *e2ee_keys, long long key_id) {
  int i, n;
  if (!cJSON_IsArray(e2ee_keys)) return NULL;
  n = cJSON_GetArraySize(e2ee_keys);
  for (i = 0; i < n; i++) {
    cJSON *entry  = cJSON_GetArrayItem(e2ee_keys, i);
    cJSON *kid    = cJSON_GetObjectItem(entry, "keyId");
    cJSON *expkey = cJSON_GetObjectItem(entry, "exportedKey");
    long long id  = cJSON_IsNumber(kid) ? (long long)kid->valuedouble : 0;
    if (id == key_id && cJSON_IsString(expkey) && expkey->valuestring)
      return expkey->valuestring;
  }
  return NULL;
}

static int save_sticon_meta_file(const char *session_path, const char *package_id,
                                 const char *meta_body) {
  char account_dir[1024];
  char purchases_dir[1280];
  char package_dir[1536];
  char meta_path[1600];
  const char *last_slash;
  FILE *fp;
  size_t n;

  if (!session_path || !package_id || !meta_body) return -1;
  last_slash = strrchr(session_path, '/');
  if (!last_slash || (size_t)(last_slash - session_path) >= sizeof(account_dir)) return -1;
  memcpy(account_dir, session_path, (size_t)(last_slash - session_path));
  account_dir[last_slash - session_path] = '\0';
  snprintf(purchases_dir, sizeof(purchases_dir), "%s/purchases", account_dir);
  snprintf(package_dir, sizeof(package_dir), "%s/%s", purchases_dir, package_id);
  snprintf(meta_path, sizeof(meta_path), "%s/meta.json", package_dir);
  mkdir(purchases_dir, 0755);
  mkdir(package_dir, 0755);
  fp = fopen(meta_path, "wb");
  if (!fp) return -1;
  n = strlen(meta_body);
  if (fwrite(meta_body, 1, n, fp) != n) {
    fclose(fp);
    return -1;
  }
  fclose(fp);
  return 0;
}

/* --- E2EE decrypt --- */

/* --- E2EE public-key cache (scoped to one decrypt phase) ---
 * TalkService_getE2EEPublicKey(mid, key_id) returns a *static* identity key:
 * the same (mid, key_id) always maps to the same key data (pure ECDH — see
 * CLAUDE.md "Re-decryption is safe and idempotent"). Within a decrypt phase
 * the same keys (our own, plus a handful of frequent senders) would otherwise
 * be re-fetched once per message — the dominant network cost of a large sync.
 * This memoises (mid, key_id) -> keyData for the phase so each pair hits the
 * wire at most once. It returns a fresh strdup on every call, so callers keep
 * their existing free() ownership unchanged. A NULL cache always fetches —
 * single-message paths get no benefit and skip the bookkeeping. */
typedef struct {
  char     **mids;
  long long *key_ids;
  char     **keys;
  int        count;
  int        cap;
} e2ee_pubkey_cache_t;

static char *pubkey_cache_get(e2ee_pubkey_cache_t *c, const char *mid,
                              long long key_id, const char *access_token) {
  char *fetched;
  int   i;
  if (c && mid) {
    for (i = 0; i < c->count; i++)
      if (c->key_ids[i] == key_id && strcmp(c->mids[i], mid) == 0)
        return c->keys[i] ? strdup(c->keys[i]) : NULL;
  }
  fetched = TalkService_getE2EEPublicKey(mid, key_id, access_token);
  if (!c || !mid || !fetched) return fetched;
  if (c->count == c->cap) {
    int        newcap = c->cap ? c->cap * 2 : 8;
    char     **m, **v;
    long long *k;
    /* Grow each array in turn, committing back on success so a later failure
     * never strands the original block. On OOM we simply skip caching. */
    m = (char **)realloc(c->mids, (size_t)newcap * sizeof(*m));
    if (!m) return fetched;
    c->mids = m;
    k = (long long *)realloc(c->key_ids, (size_t)newcap * sizeof(*k));
    if (!k) return fetched;
    c->key_ids = k;
    v = (char **)realloc(c->keys, (size_t)newcap * sizeof(*v));
    if (!v) return fetched;
    c->keys = v;
    c->cap = newcap;
  }
  c->mids[c->count] = strdup(mid);
  c->keys[c->count] = strdup(fetched);
  if (c->mids[c->count] && c->keys[c->count]) {
    c->key_ids[c->count] = key_id;
    c->count++;
  } else {
    free(c->mids[c->count]);
    free(c->keys[c->count]);
  }
  return fetched;
}

static void pubkey_cache_free(e2ee_pubkey_cache_t *c) {
  int i;
  if (!c) return;
  for (i = 0; i < c->count; i++) { free(c->mids[i]); free(c->keys[i]); }
  free(c->mids);
  free(c->key_ids);
  free(c->keys);
  memset(c, 0, sizeof(*c));
}

/* --- E2EE group-shared-key cache (scoped to one decrypt phase) ---
 * getE2EEGroupSharedKey(groupMid, groupKeyId) is static for a given pair, just
 * like the public keys, and is otherwise re-fetched once per group message.
 * Unlike a public key it parses into a struct that owns heap fields (three
 * strings + the allowedTypes/raw cJSON), so this memo keeps a deep copy as its
 * master and hands back a fresh deep copy on every hit — leaving the caller's
 * existing talk_e2ee_group_shared_key_free() teardown unchanged. NULL cache
 * always fetches. */
typedef struct {
  char                         **group_mids;
  long long                     *group_key_ids;
  talk_e2ee_group_shared_key_t  *keys;
  int                            count;
  int                            cap;
} group_key_cache_t;

/* Deep-copy src into dst, symmetric with talk_e2ee_group_shared_key_free.
 * Returns 0 on success; on OOM dst is freed+zeroed and -1 returned. */
static int group_key_copy(talk_e2ee_group_shared_key_t *dst,
                          const talk_e2ee_group_shared_key_t *src) {
  memset(dst, 0, sizeof(*dst));
  dst->keyVersion    = src->keyVersion;
  dst->groupKeyId    = src->groupKeyId;
  dst->creatorKeyId  = src->creatorKeyId;
  dst->receiverKeyId = src->receiverKeyId;
  dst->specVersion   = src->specVersion;
  if (src->creator && !(dst->creator = strdup(src->creator))) goto fail;
  if (src->receiver && !(dst->receiver = strdup(src->receiver))) goto fail;
  if (src->encryptedSharedKey &&
      !(dst->encryptedSharedKey = strdup(src->encryptedSharedKey))) goto fail;
  if (src->allowedTypes &&
      !(dst->allowedTypes = cJSON_Duplicate(src->allowedTypes, 1))) goto fail;
  if (src->raw && !(dst->raw = cJSON_Duplicate(src->raw, 1))) goto fail;
  return 0;
fail:
  talk_e2ee_group_shared_key_free(dst);
  return -1;
}

/* Fill *out with the (group_mid, group_key_id) shared key. Returns 0 on
 * success. On a hit, deep-copies the cached master into out; on a miss,
 * fetches, caches a deep copy, and leaves the fetched key in out. The caller
 * owns out either way and frees it as before. */
static int group_key_cache_get(group_key_cache_t *c, const char *access_token,
                               const char *group_mid, long long group_key_id,
                               talk_e2ee_group_shared_key_t *out) {
  int i;
  if (c && group_mid) {
    for (i = 0; i < c->count; i++)
      if (c->group_key_ids[i] == group_key_id &&
          strcmp(c->group_mids[i], group_mid) == 0)
        return group_key_copy(out, &c->keys[i]);
  }
  if (talk_get_e2ee_group_shared_key(access_token, group_mid, group_key_id, out) != 0)
    return -1;
  if (!c || !group_mid) return 0;
  if (c->count == c->cap) {
    int        newcap = c->cap ? c->cap * 2 : 8;
    char     **gm;
    long long *gk;
    talk_e2ee_group_shared_key_t *ks;
    gm = (char **)realloc(c->group_mids, (size_t)newcap * sizeof(*gm));
    if (!gm) return 0;
    c->group_mids = gm;
    gk = (long long *)realloc(c->group_key_ids, (size_t)newcap * sizeof(*gk));
    if (!gk) return 0;
    c->group_key_ids = gk;
    ks = (talk_e2ee_group_shared_key_t *)realloc(c->keys, (size_t)newcap * sizeof(*ks));
    if (!ks) return 0;
    c->keys = ks;
    c->cap = newcap;
  }
  if (group_key_copy(&c->keys[c->count], out) != 0) return 0; /* skip caching on OOM */
  c->group_mids[c->count] = strdup(group_mid);
  if (c->group_mids[c->count]) {
    c->group_key_ids[c->count] = group_key_id;
    c->count++;
  } else {
    talk_e2ee_group_shared_key_free(&c->keys[c->count]);
  }
  return 0;
}

static void group_key_cache_free(group_key_cache_t *c) {
  int i;
  if (!c) return;
  for (i = 0; i < c->count; i++) {
    free(c->group_mids[i]);
    talk_e2ee_group_shared_key_free(&c->keys[i]);
  }
  free(c->group_mids);
  free(c->group_key_ids);
  free(c->keys);
  memset(c, 0, sizeof(*c));
}

/* Decrypt a 1:1 user message. Updates *restore_state_ptr on success.
 * Returns malloc'd plaintext or NULL. If the payload contains keyMaterial
 * (image/media message), *key_material_out is set (caller must free). */
static char *decrypt_user_message(cJSON *msg, const char *my_mid,
                                  const char *access_token,
                                  cJSON *e2ee_keys,
                                  cJSON **restore_state_ptr,
                                  char **key_material_out,
                                  char **replace_json_out,
                                  e2ee_pubkey_cache_t *pubkey_cache)
{
  cJSON *chunks     = cJSON_GetObjectItem(msg, "chunks");
  cJSON *meta       = cJSON_GetObjectItem(msg, "contentMetadata");
  cJSON *from_item  = cJSON_GetObjectItem(msg, "from");
  cJSON *to_item    = cJSON_GetObjectItem(msg, "to");
  const char *from_mid, *to_mid, *peer_mid;
  long long sender_key_id, receiver_key_id, private_key_id, peer_key_id;
  const char *private_key;
  char *peer_public_key, *ciphertext;
  int version, from_me;
  cJSON *payload_obj, *response, *text_item, *new_state;
  char *result = NULL;

  if (!cJSON_IsArray(chunks) || cJSON_GetArraySize(chunks) < 5) return NULL;
  if (!cJSON_IsObject(meta)) return NULL;
  from_mid = cJSON_IsString(from_item) ? from_item->valuestring : NULL;
  to_mid   = cJSON_IsString(to_item)   ? to_item->valuestring   : NULL;
  if (!from_mid || !to_mid) return NULL;

  version        = (int)enil_json_coerce_int64(cJSON_GetObjectItem(meta, "e2eeVersion"));
  if (version != 1 && version != 2) return NULL;

  sender_key_id   = parse_key_id(cJSON_GetArrayItem(chunks, 3)->valuestring);
  receiver_key_id = parse_key_id(cJSON_GetArrayItem(chunks, 4)->valuestring);
  from_me         = my_mid && strcmp(from_mid, my_mid) == 0;
  private_key_id  = from_me ? sender_key_id   : receiver_key_id;
  peer_key_id     = from_me ? receiver_key_id : sender_key_id;
  peer_mid        = from_me ? to_mid          : from_mid;

  private_key = find_private_key(e2ee_keys, private_key_id);
  if (!private_key) return NULL;

  peer_public_key = pubkey_cache_get(pubkey_cache, peer_mid, peer_key_id, access_token);
  if (!peer_public_key) return NULL;

  ciphertext = build_ciphertext(chunks, version);
  if (!ciphertext) { free(peer_public_key); return NULL; }

  payload_obj = cJSON_CreateObject();
  cJSON_AddStringToObject(payload_obj, "privateKey",    private_key);
  cJSON_AddNumberToObject(payload_obj, "privateKeyId",  (double)private_key_id);
  cJSON_AddStringToObject(payload_obj, "peerPublicKey", peer_public_key);
  cJSON_AddStringToObject(payload_obj, "peerMid",       peer_mid);
  cJSON_AddNumberToObject(payload_obj, "peerKeyId",     (double)peer_key_id);
  if (version == 2) {
    cJSON_AddStringToObject(payload_obj, "to",   to_mid);
    cJSON_AddStringToObject(payload_obj, "from", from_mid);
    cJSON_AddNumberToObject(payload_obj, "senderKeyId",   (double)sender_key_id);
    cJSON_AddNumberToObject(payload_obj, "receiverKeyId", (double)receiver_key_id);
    cJSON *ctype = cJSON_GetObjectItem(msg, "contentType");
    if (ctype) cJSON_AddNumberToObject(payload_obj, "contentType", ctype->valuedouble);
  }
  cJSON_AddStringToObject(payload_obj, "ciphertext", ciphertext);
  if (*restore_state_ptr)
    cJSON_AddItemToObject(payload_obj, "workerRestoreState",
                          cJSON_Duplicate(*restore_state_ptr, 1));

  response = enil_worker_decrypt(
    version == 1 ? "/e2ee/decrypt-user-v1" : "/e2ee/decrypt-user-v2",
    payload_obj);
  cJSON_Delete(payload_obj);
  free(peer_public_key);
  free(ciphertext);

  if (!response) return NULL;

  new_state = cJSON_GetObjectItem(response, "workerRestoreState");
  if (new_state) *restore_state_ptr = cJSON_Duplicate(new_state, 1);

  {
    cJSON *payload = cJSON_GetObjectItem(response, "payload");
    text_item = cJSON_GetObjectItem(payload, "text");
    if (cJSON_IsString(text_item) && text_item->valuestring)
      result = strdup(text_item->valuestring);
    if (key_material_out) {
      cJSON *km = cJSON_GetObjectItem(payload, "keyMaterial");
      if (cJSON_IsString(km) && km->valuestring && km->valuestring[0])
        *key_material_out = strdup(km->valuestring);
    }
    if (replace_json_out) {
      cJSON *replace = cJSON_GetObjectItem(payload, "REPLACE");
      if (replace) *replace_json_out = cJSON_PrintUnformatted(replace);
    }
  }

  cJSON_Delete(response);
  return result;
}

/* Decrypt a group message. Updates *restore_state_ptr on success.
 * Returns malloc'd plaintext or NULL. If the payload contains keyMaterial
 * (image/media message), *key_material_out is set (caller must free). */
static char *decrypt_group_message(cJSON *msg, const char *access_token,
                                   cJSON *e2ee_keys,
                                   cJSON **restore_state_ptr,
                                   char **key_material_out,
                                   char **replace_json_out,
                                   e2ee_pubkey_cache_t *pubkey_cache,
                                   group_key_cache_t *group_key_cache)
{
  cJSON *chunks       = cJSON_GetObjectItem(msg, "chunks");
  cJSON *meta         = cJSON_GetObjectItem(msg, "contentMetadata");
  cJSON *from_item    = cJSON_GetObjectItem(msg, "from");
  cJSON *to_item      = cJSON_GetObjectItem(msg, "to");
  const char *from_mid, *group_mid;
  long long sender_key_id, group_key_id, receiver_key_id, creator_key_id;
  const char *my_private_key;
  char *creator_pub_key, *sender_pub_key, *ciphertext;
  int version;
  talk_e2ee_group_shared_key_t group_key;
  int have_group_key = 0;
  cJSON *payload_obj, *response, *text_item, *new_state;
  char *result = NULL;

  if (!cJSON_IsArray(chunks) || cJSON_GetArraySize(chunks) < 5) return NULL;
  if (!cJSON_IsObject(meta)) return NULL;
  from_mid  = cJSON_IsString(from_item) ? from_item->valuestring : NULL;
  group_mid = cJSON_IsString(to_item)   ? to_item->valuestring   : NULL;
  if (!from_mid || !group_mid) return NULL;

  version = (int)enil_json_coerce_int64(cJSON_GetObjectItem(meta, "e2eeVersion"));
  if (version != 1 && version != 2) return NULL;

  sender_key_id = parse_key_id(cJSON_GetArrayItem(chunks, 3)->valuestring);
  group_key_id  = parse_key_id(cJSON_GetArrayItem(chunks, 4)->valuestring);

  if (group_key_cache_get(group_key_cache, access_token, group_mid, group_key_id, &group_key) != 0)
    return NULL;
  have_group_key = 1;

  receiver_key_id = group_key.receiverKeyId;
  creator_key_id  = group_key.creatorKeyId;

  my_private_key = find_private_key(e2ee_keys, receiver_key_id);
  if (!my_private_key) { talk_e2ee_group_shared_key_free(&group_key); return NULL; }

  if (!group_key.creator) { talk_e2ee_group_shared_key_free(&group_key); return NULL; }

  creator_pub_key = pubkey_cache_get(pubkey_cache, group_key.creator, creator_key_id, access_token);
  sender_pub_key  = pubkey_cache_get(pubkey_cache, from_mid,          sender_key_id,  access_token);
  ciphertext      = build_ciphertext(chunks, version);

  if (!creator_pub_key || !sender_pub_key || !ciphertext) {
    free(creator_pub_key); free(sender_pub_key); free(ciphertext);
    talk_e2ee_group_shared_key_free(&group_key);
    return NULL;
  }

  payload_obj = cJSON_CreateObject();
  cJSON_AddStringToObject(payload_obj, "myPrivateKey",    my_private_key);
  cJSON_AddNumberToObject(payload_obj, "myPrivateKeyId",  (double)receiver_key_id);
  cJSON_AddStringToObject(payload_obj, "groupMid",        group_mid);
  cJSON_AddItemToObject  (payload_obj, "groupKey",        cJSON_Duplicate(group_key.raw, 1));
  cJSON_AddStringToObject(payload_obj, "senderMid",       from_mid);
  cJSON_AddNumberToObject(payload_obj, "senderKeyId",     (double)sender_key_id);
  cJSON_AddNumberToObject(payload_obj, "groupKeyId",      (double)group_key_id);
  cJSON_AddStringToObject(payload_obj, "creatorPublicKey",creator_pub_key);
  cJSON_AddStringToObject(payload_obj, "senderPublicKey", sender_pub_key);
  cJSON_AddStringToObject(payload_obj, "ciphertext",      ciphertext);
  if (version == 2) {
    cJSON_AddStringToObject(payload_obj, "to",   group_mid);
    cJSON_AddStringToObject(payload_obj, "from", from_mid);
    cJSON *ctype = cJSON_GetObjectItem(msg, "contentType");
    if (ctype) cJSON_AddNumberToObject(payload_obj, "contentType", ctype->valuedouble);
  }
  if (*restore_state_ptr)
    cJSON_AddItemToObject(payload_obj, "workerRestoreState",
                          cJSON_Duplicate(*restore_state_ptr, 1));

  response = enil_worker_decrypt(
    version == 1 ? "/e2ee/decrypt-group-v1" : "/e2ee/decrypt-group-v2",
    payload_obj);
  cJSON_Delete(payload_obj);
  if (have_group_key) talk_e2ee_group_shared_key_free(&group_key);
  free(creator_pub_key); free(sender_pub_key); free(ciphertext);

  if (!response) return NULL;

  new_state = cJSON_GetObjectItem(response, "workerRestoreState");
  if (new_state) *restore_state_ptr = cJSON_Duplicate(new_state, 1);

  {
    cJSON *payload = cJSON_GetObjectItem(response, "payload");
    text_item = cJSON_GetObjectItem(payload, "text");
    if (cJSON_IsString(text_item) && text_item->valuestring)
      result = strdup(text_item->valuestring);
    if (key_material_out) {
      cJSON *km = cJSON_GetObjectItem(payload, "keyMaterial");
      if (cJSON_IsString(km) && km->valuestring && km->valuestring[0])
        *key_material_out = strdup(km->valuestring);
    }
    if (replace_json_out) {
      cJSON *replace = cJSON_GetObjectItem(payload, "REPLACE");
      if (replace) *replace_json_out = cJSON_PrintUnformatted(replace);
    }
  }

  cJSON_Delete(response);
  return result;
}

/* --- Progress helper --- */

/* Forwards the canonical 7-phase progress through the bridge. The phase
 * label drives the queue-coalesce key — every same-phase post updates the
 * displayed item in place, so the bar advances smoothly. Determinate
 * (unit_total > 0) drives the percentage bar; indeterminate phases
 * (unit_total == 0) render as a barber-pole spinner.
 *
 * report_ex() lets specific call sites override the key (e.g. transient
 * SSE op events that should NOT coalesce with the main sync's per-phase
 * row) and the indeterminate flag. */
static void report_ex(int phase, int unit_count, int unit_total,
                      const char *msg, int error,
                      const char *key, int indeterminate)
{
  enil_progress_post(phase, ENIL_SYNC_PHASES, unit_count, unit_total,
                     msg, error, key, indeterminate);
}

static const char *phase_key(int phase)
{
  switch (phase) {
    case ENIL_SYNC_PHASE_CONNECTING:          return "sync.connecting";
    case ENIL_SYNC_PHASE_ACCOUNT:             return "sync.account";
    case ENIL_SYNC_PHASE_MESSAGES:            return "sync.messages";
    case ENIL_SYNC_PHASE_DECRYPTING:          return "sync.decrypting";
    case ENIL_SYNC_PHASE_PREPARING_DOWNLOADS: return "sync.preparing";
    case ENIL_SYNC_PHASE_DOWNLOADING:         return "sync.downloading";
    case ENIL_SYNC_PHASE_DONE:                return "sync.done";
  }
  return "sync.unknown";
}

static void report(int phase, int unit_count, int unit_total,
                   const char *msg, int error)
{
  /* Determinate when unit_total > 0; otherwise the bar shows a spinner.
   * All in-phase posts share the same key so the queue coalesces them
   * (no 3s dwell holds back the bar). */
  report_ex(phase, unit_count, unit_total, msg, error,
            phase_key(phase), unit_total > 0 ? 0 : 1);
}

/* Maps LINE stickerResourceType (int or string) to the CDN option tag. */
static const char *sticker_option_for_item(cJSON *item) {
  if (cJSON_IsString(item)) {
    const char *s = item->valuestring;
    if (strcmp(s, "ANIMATION") == 0)       return "A";
    if (strcmp(s, "ANIMATION_SOUND") == 0) return "AS";
    if (strcmp(s, "POPUP") == 0)           return "P";
    if (strcmp(s, "POPUP_SOUND") == 0)     return "PS";
    if (strcmp(s, "NAME_TEXT") == 0 ||
        strcmp(s, "PER_STICKER_TEXT") == 0) return "T";
    return "0";
  }
  switch ((int)item->valuedouble) {
    case 2: return "A";
    case 4: return "AS";
    case 5: return "P";
    case 6: return "PS";
    case 7: case 8: return "T";
    default: return "0";
  }
}

static void png_dimensions(const char *path, int *w, int *h) {
  unsigned char buf[24];
  FILE *f = fopen(path, "rb");
  *w = *h = 0;
  if (!f) return;
  if (fread(buf, 1, 24, f) == 24)  {
    *w = (buf[16]<<24)|(buf[17]<<16)|(buf[18]<<8)|buf[19];
    *h = (buf[20]<<24)|(buf[21]<<16)|(buf[22]<<8)|buf[23];
  }
  fclose(f);
}

typedef struct {
  const char *sticker_id;
  const char *package_id;
  const char *shop;
  const char *option;
  const char *hash;
  const char *sticon_type;
  char pkg_dir[1280];
  char image_rel[512];
  char thumb_rel[512];
  int image_width;
  int image_height;
  int thumb_width;
  int thumb_height;
  int downloaded;
  int error;
  int started;
  enil_identity_t identity;
} ENILStickerDownloadJob;

static int download_if_missing(const char *url, const char *dest_path,
                               int optional) {
  struct stat st;
  if (!url || !url[0] || !dest_path || !dest_path[0]) return -1;
  if (stat(dest_path, &st) == 0) return 0;
  if (enil_curl_download_file(url, dest_path) == 0) return 1;
  if (!optional) ENIL_LOG("Sync.downloads", "failed: %s", url);
  return optional ? 0 : -1;
}

static void download_avatars(sqlite3 *db, const char *session_path) {
  char account_dir[1024], avatars_dir[1088], url[512], dest[1200];
  talk_profile_t profile;
  const char *ps, *mid;
  int i;

  strncpy(account_dir, session_path, sizeof(account_dir) - 1);
  account_dir[sizeof(account_dir) - 1] = '\0';
  {
    char *slash = strrchr(account_dir, '/');
    if (!slash) {
      ENIL_LOG("Sync.download_avatars", "bad session_path: %s", session_path);
      return;
    }
    *slash = '\0';
  }
  snprintf(avatars_dir, sizeof(avatars_dir), "%s/avatars", account_dir);
  mkdir(avatars_dir, 0755);

  if (enil_db_talk_profile_get(db, &profile) == 0) {
    mid = profile.mid;
    ps  = profile.picture_path;
    if (mid && mid[0] && ps && ps[0]) {
      snprintf(url,  sizeof(url),  "https://profile.line-scdn.net/%s", ps);
      snprintf(dest, sizeof(dest), "%s/%s.jpg", avatars_dir, mid);
      (void)download_if_missing(url, dest, 1);
    }
    talk_profile_free(&profile);
  }

  {
    talk_contact_t *contacts_arr = NULL;
    int contacts_n = 0;
    enil_db_talk_contact_get_all(db, &contacts_arr, &contacts_n);
    for (i = 0; i < contacts_n; i++) {
      mid = contacts_arr[i].mid;
      ps  = contacts_arr[i].picture_path;
      if (!mid || !mid[0] || !ps || !ps[0]) { talk_contact_free(&contacts_arr[i]); continue; }
      snprintf(url,  sizeof(url),  "https://profile.line-scdn.net/%s", ps);
      snprintf(dest, sizeof(dest), "%s/%s.jpg", avatars_dir, mid);
      (void)download_if_missing(url, dest, 1);
      talk_contact_free(&contacts_arr[i]);
    }
    free(contacts_arr);
  }

  /* Group/room pictures: chats_v2.picturePath -> avatars/<chatMid>.jpg, keyed by
   * chatMid so the chat list resolves a group's own photo instead of a member's.
   * Only c/r chats live in chats_v2, so every row is a group/room; a chat with no
   * custom picture has an empty picturePath and is skipped (the UI then shows its
   * placeholder, never a random member's avatar). */
  {
    sqlite3_stmt *stmt = NULL;
    static const char *sql =
      "SELECT chatMid, picturePath FROM chats_v2"
      " WHERE picturePath IS NOT NULL AND picturePath != ''";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
      while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *cmid = (const char *)sqlite3_column_text(stmt, 0);
        const char *cpic = (const char *)sqlite3_column_text(stmt, 1);
        if (!cmid || !cmid[0] || !cpic || !cpic[0]) continue;
        snprintf(url,  sizeof(url),  "https://profile.line-scdn.net/%s", cpic);
        snprintf(dest, sizeof(dest), "%s/%s.jpg", avatars_dir, cmid);
        (void)download_if_missing(url, dest, 1);
      }
      sqlite3_finalize(stmt);
    }
  }
}

static int run_stickershop_download(ENILStickerDownloadJob *job) {
  char base_url[512], thumb_url[640], thumb_dest[1600];
  int thumb_result, image_result;
  int is_anim, is_popup;

  if (job->hash && job->hash[0])
    snprintf(base_url, sizeof(base_url),
             "https://stickershop.line-scdn.net/stickershop/v2/sticker/%s/%s/android",
             job->sticker_id, job->hash);
  else
    snprintf(base_url, sizeof(base_url),
             "https://stickershop.line-scdn.net/stickershop/v1/sticker/%s/android",
             job->sticker_id);

  snprintf(thumb_url,  sizeof(thumb_url),  "%s/sticker.png", base_url);
  snprintf(thumb_dest, sizeof(thumb_dest), "%s/%s.png", job->pkg_dir, job->sticker_id);
  snprintf(job->thumb_rel, sizeof(job->thumb_rel), "purchases/%s/%s.png",
           job->package_id, job->sticker_id);
  thumb_result = download_if_missing(thumb_url, thumb_dest, 0);
  if (thumb_result < 0) return -1;
  job->downloaded += thumb_result;
  png_dimensions(thumb_dest, &job->thumb_width, &job->thumb_height);

  is_anim  = job->option && (strcmp(job->option, "A") == 0 || strcmp(job->option, "AS") == 0);
  is_popup = job->option && (strcmp(job->option, "P") == 0 || strcmp(job->option, "PS") == 0);
  if (!is_anim && !is_popup) {
    snprintf(job->image_rel, sizeof(job->image_rel), "%s", job->thumb_rel);
    job->image_width  = job->thumb_width;
    job->image_height = job->thumb_height;
    return 0;
  }

  {
    const char *suffix = is_anim ? "sticker_animation.png" : "sticker_popup.png";
    const char *tag = is_anim ? "_anim" : "_popup";
    char image_url[640], image_dest[1600];
    snprintf(image_url, sizeof(image_url), "%s/%s", base_url, suffix);
    snprintf(image_dest, sizeof(image_dest), "%s/%s%s.png", job->pkg_dir,
             job->sticker_id, tag);
    snprintf(job->image_rel, sizeof(job->image_rel), "purchases/%s/%s%s.png",
             job->package_id, job->sticker_id, tag);
    image_result = download_if_missing(image_url, image_dest, 0);
    if (image_result < 0) return -1;
    job->downloaded += image_result;
    png_dimensions(image_dest, &job->image_width, &job->image_height);
  }
  return 0;
}

static int run_sticonshop_download(ENILStickerDownloadJob *job) {
  char thumb_url[640], thumb_dest[1600], anim_url[640], anim_dest[1600];
  int thumb_result, anim_result;
  int is_animated_package;
  struct stat st_anim;

  snprintf(thumb_url, sizeof(thumb_url),
           "https://stickershop.line-scdn.net/sticonshop/v1/sticon/%s/android/%s.png",
           job->package_id, job->sticker_id);
  snprintf(thumb_dest, sizeof(thumb_dest), "%s/%s.png", job->pkg_dir, job->sticker_id);
  snprintf(job->thumb_rel, sizeof(job->thumb_rel), "purchases/%s/%s.png",
           job->package_id, job->sticker_id);
  thumb_result = download_if_missing(thumb_url, thumb_dest, 0);
  if (thumb_result < 0) return -1;
  job->downloaded += thumb_result;
  png_dimensions(thumb_dest, &job->thumb_width, &job->thumb_height);

  snprintf(anim_url, sizeof(anim_url),
           "https://stickershop.line-scdn.net/sticonshop/v1/sticon/%s/android/%s_animation.png",
           job->package_id, job->sticker_id);
  snprintf(anim_dest, sizeof(anim_dest), "%s/%s_anim.png", job->pkg_dir, job->sticker_id);
  is_animated_package = job->sticon_type && strcmp(job->sticon_type, "ANIMATION") == 0;
  anim_result = is_animated_package ? download_if_missing(anim_url, anim_dest, 1) : 0;
  job->downloaded += anim_result > 0 ? anim_result : 0;

  if (stat(anim_dest, &st_anim) == 0) {
    snprintf(job->image_rel, sizeof(job->image_rel), "purchases/%s/%s_anim.png",
             job->package_id, job->sticker_id);
    png_dimensions(anim_dest, &job->image_width, &job->image_height);
    return 0;
  }

  snprintf(job->image_rel, sizeof(job->image_rel), "%s", job->thumb_rel);
  job->image_width  = job->thumb_width;
  job->image_height = job->thumb_height;
  return 0;
}

static void *sticker_download_thread(void *arg) {
  ENILStickerDownloadJob *job = (ENILStickerDownloadJob *)arg;
  if (!job || !job->sticker_id || !job->package_id || !job->shop) return NULL;
  if (!enil_identity_bind(&job->identity)) { job->error = 1; return NULL; }
  mkdir(job->pkg_dir, 0755);
  if (strcmp(job->shop, "stickershop") == 0)
    job->error = run_stickershop_download(job) != 0;
  else
    job->error = run_sticonshop_download(job) != 0;
  return NULL;
}

static void set_account_dir(const char *session_path, char *buf, size_t buf_size) {
  const char *last_slash;
  size_t len;
  if (!buf || buf_size == 0) return;
  buf[0] = '\0';
  if (!session_path) return;
  last_slash = strrchr(session_path, '/');
  if (!last_slash || (size_t)(last_slash - session_path) >= buf_size) {
    return;
  }
  len = (size_t)(last_slash - session_path);
  memcpy(buf, session_path, len);
  buf[len] = '\0';
}

static int init_sticker_job(ENILStickerDownloadJob *job, cJSON *entry,
                            const char *stk_dir) {
  memset(job, 0, sizeof(*job));
  if (!enil_identity_current()) return 0;
  job->identity = *enil_identity_current();
  job->sticker_id  = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "sticker_id"));
  job->package_id  = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "package_id"));
  job->shop        = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "shop"));
  job->option      = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "option"));
  job->hash        = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "hash"));
  job->sticon_type = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "sticon_resource_type"));
  if (!job->sticker_id || !job->package_id || !job->shop || !stk_dir) return 0;
  snprintf(job->pkg_dir, sizeof(job->pkg_dir), "%s/%s", stk_dir, job->package_id);
  job->started = 1;
  return 1;
}

static void apply_sticker_job(sqlite3 *db, ENILStickerDownloadJob *job,
                              int *done, int *errors) {
  int is_sticon;
  if (!job || !job->started) return;
  *done += job->downloaded;
  if (job->error || !job->image_rel[0] || !job->thumb_rel[0]) {
    (*errors)++;
    /* Name the specific failure so a sticker/sticon that silently never
     * downloads is diagnosable (it stays pending and is retried next sync). */
    ENIL_LOG("Sync.downloads", "%s/%s (%s) download failed: %s",
             job->package_id ? job->package_id : "?",
             job->sticker_id ? job->sticker_id : "?",
             job->shop       ? job->shop       : "?",
             job->error          ? "fetch error" :
             (!job->image_rel[0] ? "no image"    : "no thumbnail"));
    return;
  }
  is_sticon = job->shop && strcmp(job->shop, "sticonshop") == 0;
  if (is_sticon) {
    enil_db_set_sticon_image_path(db, job->sticker_id, job->package_id,
                                   job->image_rel, job->image_width, job->image_height);
    enil_db_set_sticon_thumb_path(db, job->sticker_id, job->package_id,
                                   job->thumb_rel, job->thumb_width, job->thumb_height);
  } else {
    enil_db_set_sticker_image_path(db, job->sticker_id, job->package_id,
                                    job->image_rel, job->image_width, job->image_height);
    enil_db_set_sticker_thumb_path(db, job->sticker_id, job->package_id,
                                    job->thumb_rel, job->thumb_width, job->thumb_height);
  }
}

/* Wraps `enil_db_get_*_needing_download` so the existing batched download loop
 * can drive both shops through one code path. */
static cJSON *items_needing_download(sqlite3 *db, int purchased, int sticon) {
  return sticon ? enil_db_get_sticons_needing_download(db, purchased)
                : enil_db_get_stickers_needing_download(db, purchased);
}

/* Download every item in `pending` (a cJSON array of {sticker_id, package_id,
 * shop, option, hash, [sticon_resource_type]} entries) and write the resulting
 * image/thumb paths back. Takes ownership of `pending` (frees it). src/shop are
 * for logging only. Shared by the full-catalog sweep and the single-message
 * resolver. */
static int download_sticker_list(sqlite3 *db, const char *session_path,
                                 cJSON *pending, const char *src,
                                 const char *shop, int *counter,
                                 int phase_total, int emit_progress) {
  int    total   = pending ? cJSON_GetArraySize(pending) : 0;
  int    done    = 0, errors = 0, i, batch_num = 0;
  char   stk_dir[1024];
  char   account_dir[1024];

  if (total == 0) { cJSON_Delete(pending); return 0; }
  ENIL_LOG("Sync.downloads", "%s %s: %d items pending", src, shop, total);

  set_account_dir(session_path, account_dir, sizeof(account_dir));
  if (!account_dir[0]) {
    cJSON_Delete(pending);
    return -1;
  }
  snprintf(stk_dir, sizeof(stk_dir), "%s/purchases", account_dir);
  mkdir(stk_dir, 0755);

  sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);

  for (i = 0; i < total; i++) {
    ENILStickerDownloadJob jobs[STICKER_CDN_BATCH_SIZE];
    pthread_t threads[STICKER_CDN_BATCH_SIZE];
    int thread_started[STICKER_CDN_BATCH_SIZE];
    int batch_count = 0, j;

    while (i < total && batch_count < STICKER_CDN_BATCH_SIZE) {
      cJSON *entry = cJSON_GetArrayItem(pending, i);
      if (init_sticker_job(&jobs[batch_count], entry, stk_dir)) {
        int rc = pthread_create(&threads[batch_count], NULL,
                                sticker_download_thread, &jobs[batch_count]);
        thread_started[batch_count] = rc == 0;
        if (rc != 0) sticker_download_thread(&jobs[batch_count]);
        batch_count++;
      }
      i++;
    }
    i--;

    /* One line per batch as its downloads kick off (threads already spawned
     * above; this batch joins below). i is the last index of this batch. */
    ENIL_LOG("Sync.downloads", "%s %s: batch %d - %d items (%d/%d)",
             src, shop, ++batch_num, batch_count, i + 1, total);

    for (j = 0; j < batch_count; j++)
      if (thread_started[j]) pthread_join(threads[j], NULL);

    for (j = 0; j < batch_count; j++) {
      apply_sticker_job(db, &jobs[j], &done, &errors);
      if (counter) (*counter)++;
      if (emit_progress && counter)
        report(ENIL_SYNC_PHASE_DOWNLOADING, *counter, phase_total,
               "Downloading...", 0);
    }
  }
  sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  cJSON_Delete(pending);
  ENIL_LOG("Sync.downloads",
           "%s %s: %d files downloaded for %d items, %d errors",
           src, shop, done, total, errors);
  /* Per-sticker errors are non-fatal; the rows stay pending for next sync. */
  return 0;
}

/* Full-catalog path: discover work by scanning the whole table, then download
 * it. Used by full sync (sync_phase_downloading). */
static int run_sticker_downloads(sqlite3 *db, const char *session_path,
                                 int purchased, int sticon,
                                 int *counter, int phase_total,
                                 int emit_progress) {
  cJSON      *pending = items_needing_download(db, purchased, sticon);
  const char *shop    = sticon    ? "sticon" : "sticker";
  const char *src     = purchased ? "owned"  : "discovered";
  return download_sticker_list(db, session_path, pending, src, shop,
                               counter, phase_total, emit_progress);
}

/* --- Sticonshop meta.json fetch.
 *     Two modes:
 *       expand=1: full expansion. INSERTs one sticons_v2 row per entry in
 *                 meta.orders[] with alt_text from meta.altTexts. Used for
 *                 owned packs that drive the sticker picker.
 *       expand=0: alt_text enrichment only. UPDATEs alt_text on the sticons_v2
 *                 rows that already exist (e.g. discovered via REPLACE blocks
 *                 in incoming messages). Does NOT create new rows — keeps
 *                 unowned packs sparse so we don't pre-download images for
 *                 sticons the user may never see.
 *     The meta.json file itself is cached on disk regardless of mode.
 *     Returns 0 on success, -1 on fetch failure. */
static int sync_sticonshop_meta(sqlite3 *db, const char *session_path,
                                const char *pkg_id, int expand) {
  char  meta_url[256];
  char *meta_body;
  cJSON *meta, *orders, *alt_map;
  int k, nk;

  if (!pkg_id || !pkg_id[0]) return -1;
  snprintf(meta_url, sizeof(meta_url),
           "https://stickershop.line-scdn.net/sticonshop/v1/product/%s/android/meta.json",
           pkg_id);
  meta_body = enil_curl_get(meta_url);
  if (!meta_body) {
    ENIL_LOG("Sync.sticon_meta", "%s: meta.json fetch failed", pkg_id);
    return -1;
  }
  meta = cJSON_Parse(meta_body);
  if (!cJSON_IsObject(meta)) {
    ENIL_LOG("Sync.sticon_meta", "%s: meta.json parse failed", pkg_id);
    cJSON_Delete(meta);
    free(meta_body);
    return -1;
  }
  save_sticon_meta_file(session_path, pkg_id, meta_body);

  /* meta.json carries sticonResourceType="ANIMATION" for animated packs (the
   * field is absent on static packs). Back-fill the int form on the package
   * row so the download phase knows whether to also fetch <id>_animation.png
   * for sticons in this pack. */
  {
    const char *rt = cJSON_GetStringValue(
                       cJSON_GetObjectItem(meta, "sticonResourceType"));
    if (rt && strcmp(rt, "ANIMATION") == 0)
      enil_db_sticon_package_set_resource_type(db, pkg_id, 2);
    else if (rt && strcmp(rt, "STATIC") == 0)
      enil_db_sticon_package_set_resource_type(db, pkg_id, 1);
  }

  alt_map = cJSON_GetObjectItem(meta, "altTexts");
  orders  = cJSON_GetObjectItem(meta, "orders");
  nk = cJSON_IsArray(orders) ? cJSON_GetArraySize(orders) : 0;
  for (k = 0; k < nk; k++) {
    cJSON      *id_item   = cJSON_GetArrayItem(orders, k);
    const char *sticon_id = cJSON_IsString(id_item) ? id_item->valuestring : NULL;
    if (sticon_id && sticon_id[0]) {
      const char *alt_text = cJSON_GetStringValue(cJSON_GetObjectItem(alt_map, sticon_id));
      if (expand)
        enil_db_sticon_upsert(db, sticon_id, pkg_id, alt_text);
      else if (alt_text && alt_text[0])
        enil_db_set_sticon_alt_text(db, sticon_id, pkg_id, alt_text);
    }
  }
  ENIL_LOG("Sync.sticon_meta", "%s: %d sticons (%s)", pkg_id, nk, expand ? "expand" : "enrich");
  cJSON_Delete(meta);
  free(meta_body);
  return 0;
}

/* --- Inline message scan: discover sticker/sticon packages from raw message JSON.
 *     Called per chat as part of PHASE_MESSAGES. */
static void scan_messages_for_stickers(sqlite3 *db, cJSON *msgs) {
  int j, nm = cJSON_GetArraySize(msgs);
  for (j = 0; j < nm; j++) {
    cJSON *m    = cJSON_GetArrayItem(msgs, j);
    cJSON *ct   = cJSON_GetObjectItem(m, "contentType");
    cJSON *meta = cJSON_GetObjectItem(m, "contentMetadata");
    if (!cJSON_IsObject(meta)) continue;
    if (cJSON_IsNumber(ct) && (int)ct->valuedouble == 7) {
      const char *pkg_id = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "STKPKGID"));
      const char *stk_id = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "STKID"));
      const char *opt    = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "STKOPT"));
      const char *hash   = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "STKHASH"));
      if (pkg_id && pkg_id[0] && stk_id && stk_id[0]) {
        enil_db_sticker_package_discover(db, pkg_id);
        enil_db_sticker_upsert(db, stk_id, pkg_id, opt, hash);
      }
    } else if (cJSON_IsNumber(ct) && (int)ct->valuedouble == 0) {
      cJSON *own_item = cJSON_GetObjectItem(meta, "STICON_OWNERSHIP");
      if (cJSON_IsString(own_item) && own_item->valuestring) {
        cJSON *ownership = cJSON_Parse(own_item->valuestring);
        if (cJSON_IsArray(ownership)) {
          int k, nk = cJSON_GetArraySize(ownership);
          for (k = 0; k < nk; k++) {
            cJSON *item = cJSON_GetArrayItem(ownership, k);
            if (cJSON_IsString(item) && item->valuestring && item->valuestring[0])
              enil_db_sticon_package_discover(db, item->valuestring);
          }
        }
        cJSON_Delete(ownership);
      }
      cJSON *rep_item = cJSON_GetObjectItem(meta, "REPLACE");
      if (cJSON_IsString(rep_item) && rep_item->valuestring) {
        cJSON *replace    = cJSON_Parse(rep_item->valuestring);
        cJSON *sticon_obj = replace ? cJSON_GetObjectItem(replace, "sticon") : NULL;
        cJSON *resources  = sticon_obj ? cJSON_GetObjectItem(sticon_obj, "resources") : NULL;
        if (cJSON_IsArray(resources)) {
          int k, nk = cJSON_GetArraySize(resources);
          for (k = 0; k < nk; k++) {
            cJSON *res = cJSON_GetArrayItem(resources, k);
            const char *prod_id = cJSON_GetStringValue(cJSON_GetObjectItem(res, "productId"));
            const char *sti_id  = cJSON_GetStringValue(cJSON_GetObjectItem(res, "sticonId"));
            if (prod_id && prod_id[0] && sti_id && sti_id[0]) {
              enil_db_sticon_package_discover(db, prod_id);
              enil_db_sticon_upsert(db, sti_id, prod_id, NULL);
            }
          }
        }
        cJSON_Delete(replace);
      }
    }
  }
}

static int upsert_chat_for_message(sqlite3 *db, cJSON *msg,
                                   const char *chat_id, const char *my_mid) {
  const char *from_mid = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "from"));
  const char *created  = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "createdTime"));
  const char *msg_id   = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "id"));
  int incoming;
  if (!db || !msg || !chat_id) return SQLITE_ERROR;
  incoming = (my_mid && from_mid && strcmp(from_mid, my_mid) != 0) ? 1 : 0;
  return enil_db_message_box_upsert_live_message(
    db, chat_id, 0, msg_id, created ? strtoll(created, NULL, 10) : 0,
    incoming, msg_id);
}

static int decrypt_one_message(sqlite3 *db, cJSON *msg,
                               const char *my_mid,
                               const char *access_token,
                               const char *session_path) {
  session_t session;
  cJSON *restore_state = NULL, *e2ee_keys = NULL;
  cJSON *chunks, *meta, *to_type_item, *old_restore;
  memset(&session, 0, sizeof(session));
  const char *message_id = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "id"));
  char *plaintext = NULL, *replace_json = NULL, *key_material = NULL;
  int restore_changed = 0;
  int to_type;

  if (!db || !msg || !message_id || !access_token || !session_path) return SQLITE_ERROR;
  if (is_unsupported_location_message(msg)) {
    ENIL_LOG("Sync.decrypting",
             "%s: unsupported location; leaving undecrypted", message_id);
    return enil_db_set_message_text(db, message_id, NULL,
                                    ENIL_DECRYPT_NOT_TEXT);
  }
  chunks = cJSON_GetObjectItem(msg, "chunks");
  meta = cJSON_GetObjectItem(msg, "contentMetadata");
  to_type_item = cJSON_GetObjectItem(msg, "toType");
  if (!cJSON_IsArray(chunks) || cJSON_GetArraySize(chunks) < 5 ||
      !cJSON_IsObject(meta) || !cJSON_GetObjectItem(meta, "e2eeVersion")) {
    enil_db_set_message_text(db, message_id, NULL, ENIL_DECRYPT_NOT_TEXT);
    return SQLITE_OK;
  }
  if (load_session(session_path, &session, &restore_state, &e2ee_keys) != 0)
    return SQLITE_ERROR;
  old_restore = restore_state;
  to_type = cJSON_IsNumber(to_type_item) ? (int)to_type_item->valuedouble : 0;
  if (to_type == 0)
    plaintext = decrypt_user_message(msg, my_mid, access_token,
                                     e2ee_keys, &restore_state,
                                     &key_material, &replace_json, NULL);
  else
    plaintext = decrypt_group_message(msg, access_token,
                                      e2ee_keys, &restore_state,
                                      &key_material, &replace_json, NULL, NULL);
  if (restore_state != old_restore) restore_changed = 1;
  if (plaintext) {
    char *updated_raw = NULL;
    if (replace_json && cJSON_IsObject(meta)) {
      cJSON_DeleteItemFromObject(meta, "REPLACE");
      cJSON_AddStringToObject(meta, "REPLACE", replace_json);
      updated_raw = cJSON_PrintUnformatted(msg);
    }
    if (updated_raw) {
      enil_db_set_message_plaintext(db, message_id, plaintext, updated_raw,
                                    replace_json, ENIL_DECRYPT_SUCCESS);
      free(updated_raw);
    } else {
      enil_db_set_message_text(db, message_id, plaintext, ENIL_DECRYPT_SUCCESS);
    }
  } else if (key_material) {
    enil_db_set_message_enc_km(db, message_id, key_material);
  } else {
    enil_db_set_message_text(db, message_id, NULL, ENIL_DECRYPT_FAILED);
  }
  if (restore_changed && restore_state)
    save_restore_state(session_path, &session, restore_state);
  free(plaintext);
  free(replace_json);
  free(key_material);
  enil_session_free(&session);
  return SQLITE_OK;
}

/* Add pkg_id to a cJSON string-array set, skipping empties and duplicates. */
static void add_pkg_id(cJSON *set, const char *id) {
  int i, n;
  if (!id || !id[0]) return;
  n = cJSON_GetArraySize(set);
  for (i = 0; i < n; i++) {
    cJSON *it = cJSON_GetArrayItem(set, i);
    if (cJSON_IsString(it) && strcmp(it->valuestring, id) == 0) return;
  }
  cJSON_AddItemToArray(set, cJSON_CreateString(id));
}

/* Collect the sticon package ids one message references, from the plaintext
 * STICON_OWNERSHIP list and the (post-decrypt) REPLACE resources. */
static void collect_message_sticon_pkgs(cJSON *meta, cJSON *set) {
  cJSON *own = cJSON_GetObjectItem(meta, "STICON_OWNERSHIP");
  cJSON *rep = cJSON_GetObjectItem(meta, "REPLACE");
  if (cJSON_IsString(own) && own->valuestring) {
    cJSON *arr = cJSON_Parse(own->valuestring);
    int i, n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
    for (i = 0; i < n; i++) {
      cJSON *it = cJSON_GetArrayItem(arr, i);
      if (cJSON_IsString(it)) add_pkg_id(set, it->valuestring);
    }
    cJSON_Delete(arr);
  }
  if (cJSON_IsString(rep) && rep->valuestring) {
    cJSON *r   = cJSON_Parse(rep->valuestring);
    cJSON *so  = r ? cJSON_GetObjectItem(r, "sticon") : NULL;
    cJSON *res = so ? cJSON_GetObjectItem(so, "resources") : NULL;
    int i, n = cJSON_IsArray(res) ? cJSON_GetArraySize(res) : 0;
    for (i = 0; i < n; i++) {
      cJSON *e = cJSON_GetArrayItem(res, i);
      add_pkg_id(set, cJSON_GetStringValue(cJSON_GetObjectItem(e, "productId")));
    }
    cJSON_Delete(r);
  }
}

/* Download the PNGs for THIS message's sticons that aren't on disk yet, so
 * resolve_resource (which has no CDN fallback) renders them. Reads the (pkg,
 * sticon_id) pairs from message_sticons, which message_upsert + the post-decrypt
 * text write have already populated. Animation variants are left to full sync. */
static void download_message_sticon_images(sqlite3 *db, const char *session_path,
                                           const char *message_id) {
  cJSON *rows, *pending;
  int i, n;
  if (!message_id || !message_id[0]) return;
  rows = enil_db_get_message_sticons(db, message_id);
  n = rows ? cJSON_GetArraySize(rows) : 0;
  if (n == 0) { cJSON_Delete(rows); return; }
  pending = cJSON_CreateArray();
  for (i = 0; i < n; i++) {
    cJSON      *row = cJSON_GetArrayItem(rows, i);
    const char *pkg = cJSON_GetStringValue(cJSON_GetObjectItem(row, "package_id"));
    const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(row, "sticon_id"));
    const char *img = cJSON_GetStringValue(cJSON_GetObjectItem(row, "image_path"));
    cJSON      *entry;
    if (img && img[0]) continue;                 /* already on disk */
    if (!pkg || !pkg[0] || !sid || !sid[0]) continue;
    entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "sticker_id", sid);
    cJSON_AddStringToObject(entry, "package_id", pkg);
    cJSON_AddStringToObject(entry, "shop", "sticonshop");
    cJSON_AddItemToArray(pending, entry);
  }
  cJSON_Delete(rows);
  download_sticker_list(db, session_path, pending, "live", "sticon", NULL, 0, 0);
}

/* SSE path: for the sticons THIS message references, fetch meta.json (alt text /
 * owned-pack expansion) for packs that still need it, then download the sticon
 * images — both targeted, no whole-catalog scan. */
static void sync_one_message_sticons(sqlite3 *db, const char *session_path,
                                     cJSON *msg) {
  cJSON *meta = cJSON_GetObjectItem(msg, "contentMetadata");
  const char *message_id = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "id"));
  cJSON *set;
  int i, n;
  if (!cJSON_IsObject(meta)) return;
  set = cJSON_CreateArray();
  if (!set) return;
  collect_message_sticon_pkgs(meta, set);
  n = cJSON_GetArraySize(set);
  for (i = 0; i < n; i++) {
    const char *pkg = cJSON_GetArrayItem(set, i)->valuestring;
    int st = enil_db_sticon_package_meta_state(db, pkg);
    if (st == 1) sync_sticonshop_meta(db, session_path, pkg, 1);
    else if (st == 2) sync_sticonshop_meta(db, session_path, pkg, 0);
  }
  cJSON_Delete(set);
  download_message_sticon_images(db, session_path, message_id);
}

/* SSE path: download just THIS message's sticker if it isn't already on disk. */
static void sync_one_message_sticker(sqlite3 *db, const char *session_path,
                                     cJSON *msg) {
  cJSON *meta = cJSON_GetObjectItem(msg, "contentMetadata");
  const char *pkg_id, *stk_id, *opt, *hash;
  cJSON *pending, *entry;
  char buf[512];
  int  w, h;
  if (!cJSON_IsObject(meta)) return;
  pkg_id = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "STKPKGID"));
  stk_id = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "STKID"));
  opt    = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "STKOPT"));
  hash   = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "STKHASH"));
  if (!pkg_id || !pkg_id[0] || !stk_id || !stk_id[0]) return;
  if (enil_db_get_sticker_image_info_for_package(db, pkg_id, stk_id,
                                                 buf, sizeof(buf), &w, &h))
    return; /* already on disk */
  pending = cJSON_CreateArray();
  entry   = cJSON_CreateObject();
  if (!pending || !entry) { cJSON_Delete(pending); cJSON_Delete(entry); return; }
  cJSON_AddStringToObject(entry, "sticker_id", stk_id);
  cJSON_AddStringToObject(entry, "package_id", pkg_id);
  cJSON_AddStringToObject(entry, "shop", "stickershop");
  cJSON_AddStringToObject(entry, "option", opt ? opt : "");
  cJSON_AddStringToObject(entry, "hash", hash ? hash : "");
  cJSON_AddItemToArray(pending, entry);
  download_sticker_list(db, session_path, pending, "live", "sticker", NULL, 0, 0);
}

/* Download one media entry (from enil_db_get_message*_media). No-op if the row
 * already has a media_path. */
static void download_media_entry(sqlite3 *db, const char *session_path,
                                 const char *account_dir, cJSON *entry) {
  const char *message_id = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "message_id"));
  const char *chat_id    = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "chat_id"));
  const char *media_path = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "media_path"));
  const char *enc_km, *sid, *oid, *obs_pop, *e2ee_version;
  char media_dir[1280], chat_dir[1536], dest[1600], rel_path[512];
  char thumb_dest[1600], thumb_rel[512];
  ENILThumbInfo tinfo;
  size_t plain_size;
  if (!message_id || !chat_id) return;
  if (media_path && media_path[0]) return;
  enc_km = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "enc_km"));
  sid = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "sid"));
  oid = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "oid"));
  obs_pop = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "obs_pop"));
  e2ee_version = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "e2ee_version"));
  plain_size = (size_t)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "plain_size"));
  snprintf(media_dir, sizeof(media_dir), "%s/media", account_dir);
  snprintf(chat_dir, sizeof(chat_dir), "%s/%s", media_dir, chat_id);
  snprintf(dest, sizeof(dest), "%s/%s.jpg", chat_dir, message_id);
  snprintf(rel_path, sizeof(rel_path), "media/%s/%s.jpg", chat_id, message_id);
  mkdir(media_dir, 0755);
  mkdir(chat_dir, 0755);
  if (enil_obs_download_message(session_path, message_id, sid, oid,
                                obs_pop, e2ee_version, enc_km,
                                plain_size, dest) != 0)
    return;
  memset(&tinfo, 0, sizeof(tinfo));
  snprintf(thumb_dest, sizeof(thumb_dest), "%s/%s_t.jpg", chat_dir, message_id);
  snprintf(thumb_rel, sizeof(thumb_rel), "media/%s/%s_t.jpg", chat_id, message_id);
  if (enil_thumb_generate(dest, thumb_dest, &tinfo) != 0)
    thumb_rel[0] = '\0';
  enil_db_set_media_info(db, message_id, rel_path,
                         tinfo.orig_width, tinfo.orig_height,
                         thumb_rel[0] ? thumb_rel : NULL,
                         tinfo.thumb_width, tinfo.thumb_height);
}

/* SSE path: download just THIS message's image if it isn't already on disk. */
static void sync_one_message_media(sqlite3 *db, const char *session_path,
                                   const char *message_id) {
  cJSON *entry;
  const char *media_path;
  char account_dir[1024];
  if (!message_id || !message_id[0]) return;
  entry = enil_db_get_message_media_info(db, message_id);
  if (!entry) return;
  media_path = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "media_path"));
  if (media_path && media_path[0]) { cJSON_Delete(entry); return; }
  set_account_dir(session_path, account_dir, sizeof(account_dir));
  if (account_dir[0]) {
    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
    download_media_entry(db, session_path, account_dir, entry);
    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  }
  cJSON_Delete(entry);
}

static int message_has_plaintext(sqlite3 *db, const char *message_id) {
  static const char *sql =
    "SELECT 1 FROM messages_v2 WHERE id = ? AND text IS NOT NULL AND text != ''";
  sqlite3_stmt *stmt;
  int found = 0;
  if (!db || !message_id) return 0;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
  sqlite3_bind_text(stmt, 1, message_id, -1, SQLITE_TRANSIENT);
  found = (sqlite3_step(stmt) == SQLITE_ROW);
  sqlite3_finalize(stmt);
  return found;
}

/* Force-refresh the avatar JPEG on disk for a given mid. Used by op 49 when
 * a peer's profile changes, since the cached file is now stale. */
static void refresh_avatar_for_mid(const char *session_path,
                                   const char *mid,
                                   const char *picture_path) {
  char account_dir[1024], avatars_dir[1088], url[512], dest[1200];
  char *slash;
  if (!session_path || !mid || !mid[0] || !picture_path || !picture_path[0])
    return;
  strncpy(account_dir, session_path, sizeof(account_dir) - 1);
  account_dir[sizeof(account_dir) - 1] = '\0';
  slash = strrchr(account_dir, '/');
  if (!slash) return;
  *slash = '\0';
  snprintf(avatars_dir, sizeof(avatars_dir), "%s/avatars", account_dir);
  mkdir(avatars_dir, 0755);
  snprintf(url,  sizeof(url),  "https://profile.line-scdn.net/%s", picture_path);
  snprintf(dest, sizeof(dest), "%s/%s.jpg", avatars_dir, mid);
  ENIL_LOG("Sync.refresh_avatar", "mid=%s picturePath=%s", mid, picture_path);
  enil_curl_download_file(url, dest);  /* overwrites on success */
}

/* Handle op 49 NOTIFIED_UPDATE_PROFILE: refetch one contact via
 * getContactsV2([mid]) and upsert. Force-refreshes the avatar on disk
 * since the cached file no longer matches the new picturePath. */
static int apply_op_contact_update(sqlite3    *db,
                                   const char *access_token,
                                   const char *session_path,
                                   const char *mid) {
  talk_contact_t contact;
  int rc;
  if (!mid || !mid[0]) return SQLITE_ERROR;
  memset(&contact, 0, sizeof(contact));
  if (talk_get_contact_by_mid(access_token, mid, &contact) != 0) {
    ENIL_LOG("Sync.op_49", "getContactsV2 failed for %s", mid);
    return SQLITE_ERROR;
  }
  rc = enil_db_talk_contact_upsert(db, &contact);
  if (rc == SQLITE_OK)
    refresh_avatar_for_mid(session_path, mid, contact.picture_path);
  talk_contact_free(&contact);
  return rc;
}

/* Handle op 121 / 122 NOTIFIED_UPDATE_CHAT: refetch one chat via
 * getChats([mid]) and upsert. isInvited stays 0 — if a full sync is needed
 * to recover invite state, the user will press Sync. */
static int apply_op_chat_update(sqlite3    *db,
                                const char *access_token,
                                const char *session_path,
                                const char *chat_mid) {
  const char  *mids[1];
  talk_chat_t *chats_arr = NULL;
  int          chats_n = 0;
  int          rc = SQLITE_ERROR;
  if (!chat_mid || !chat_mid[0]) return SQLITE_ERROR;
  mids[0] = chat_mid;
  if (talk_get_chats(access_token, mids, 1, &chats_arr, &chats_n) != 0) {
    ENIL_LOG("Sync.op_121", "getChats failed for %s", chat_mid);
    return SQLITE_ERROR;
  }
  if (chats_n > 0) {
    chats_arr[0].isInvited = 0;
    rc = enil_db_chat_upsert(db, &chats_arr[0]);
    /* op 121/122 fires precisely when chat metadata (including the photo)
     * changed, so force-refresh avatars/<chatMid>.jpg to replace a stale copy.
     * No-op when the group has no custom picture (empty picturePath). */
    if (rc == SQLITE_OK)
      refresh_avatar_for_mid(session_path, chats_arr[0].chatMid,
                             chats_arr[0].picturePath);
  }
  {
    int i;
    for (i = 0; i < chats_n; i++) talk_chat_free(&chats_arr[i]);
  }
  free(chats_arr);
  return rc;
}

/* An SSE message can be the first thing we ever see for a chat (e.g. a group
 * re-created after we left, or a brand-new 1:1). upsert_chat_for_message only
 * writes a message_boxes_v2 row, so the chat list would show the raw mid.
 * Pull the display metadata once, only when it's actually missing:
 *   group/room → getChats         (chats_v2.chatName)
 *   1:1        → getContactsV2     (contacts_v2.displayName, for the peer)
 * Best-effort: a failure just leaves the mid showing until the next Sync. */
static void backfill_chat_metadata(sqlite3 *db, const char *access_token,
                                   const char *session_path,
                                   const char *chat_id) {
  if (!chat_id || !chat_id[0]) return;
  if (enil_mid_is_one_to_one(chat_id)) {
    if (!enil_db_contact_exists(db, chat_id))
      apply_op_contact_update(db, access_token, session_path, chat_id);
    return;
  }
  if (!enil_db_chat_exists(db, chat_id))
    apply_op_chat_update(db, access_token, session_path, chat_id);
}

/* Handle op 139 SEND_REACTION (our own echo; param3 absent → reactor is
 * my_mid) and 140 NOTIFIED_SEND_REACTION (peer reacted; param3 = reactor).
 * Both carry param2 = {chatMid, curr?:{predefinedReactionType:N |
 * paidReactionType:{productId,emojiId,...}}}; curr absent → reaction removed. */
static int apply_op_reaction(sqlite3 *db, const talk_operation_t *op,
                             const char *my_mid) {
  cJSON *p2 = (op->param2 && op->param2[0]) ? cJSON_Parse(op->param2) : NULL;
  cJSON *curr = p2 ? cJSON_GetObjectItem(p2, "curr") : NULL;
  cJSON *prt  = cJSON_IsObject(curr)
    ? cJSON_GetObjectItem(curr, "paidReactionType") : NULL;
  cJSON *pre  = cJSON_IsObject(curr)
    ? cJSON_GetObjectItem(curr, "predefinedReactionType") : NULL;
  const char *product_id = NULL, *emoji_id = NULL;
  const char *reactor;
  int predefined_type = 0, is_remove;
  int rc;
  if (cJSON_IsObject(prt)) {
    cJSON *pid = cJSON_GetObjectItem(prt, "productId");
    cJSON *eid = cJSON_GetObjectItem(prt, "emojiId");
    if (cJSON_IsString(pid)) product_id = pid->valuestring;
    if (cJSON_IsString(eid)) emoji_id   = eid->valuestring;
  }
  if (pre && cJSON_IsNumber(pre)) predefined_type = (int)pre->valuedouble;
  is_remove = !cJSON_IsObject(curr);
  reactor = (op->param3 && op->param3[0]) ? op->param3 : my_mid;
  ENIL_LOG("Sync.op_reaction", "type=%d target=%s reactor=%s "
          "predefined=%d productId=%s remove=%d", op->type,
          op->param1 ? op->param1 : "(null)",
          reactor ? reactor : "(null)",
          predefined_type,
          product_id ? product_id : "(null)",
          is_remove);
  rc = (op->param1 && op->param1[0] && reactor && reactor[0])
    ? enil_db_message_reactions_update(db, op->param1, reactor,
                                       is_remove, predefined_type,
                                       product_id, emoji_id,
                                       op->createdTime)
    : SQLITE_ERROR;
  if (p2) cJSON_Delete(p2);
  return rc;
}

/* ============================================================================
 * Dispatch a single SSE event — decrypts, applies the op to sqlite, and
 * advances localRev. Returns 1 if the event was handled.
 * ==========================================================================*/
int enil_sync_process_sse_event(sqlite3    *db,
                                const char *access_token,
                                const char *my_mid,
                                const char *session_path,
                                const char *event_type,
                                const char *event_data,
                                int        *out_handled) {
  cJSON *root, *msg, *arr;
  talk_operation_t op;
  const char *chat_id;
  int rc;
  if (out_handled) *out_handled = 0;
  if (!db || !access_token || !session_path || !event_type || !event_data)
    return SQLITE_ERROR;
  if (strcmp(event_type, "message") != 0) return SQLITE_OK;
  root = cJSON_Parse(event_data);
  if (!root) return SQLITE_ERROR;
  if (talk_operation_parse(root, my_mid, &op) != 0) {
    cJSON_Delete(root);
    return SQLITE_ERROR;
  }
  ENIL_LOG("Sync.process_sse_event", "op rev=%lld type=%d (%s) msg=%s", op.revision, op.type, enil_op_type_name(op.type),
          op.message && op.message->id ? op.message->id : "(none)");
  /* Explicit dispatch: every case below is an op we ACT on. Anything not
     listed falls to `default` and is logged as unhandled (no-op, localRev
     still advances). Op-type names resolve via enil_op_type_name(). */
  switch (op.type) {
    case ENIL_OP_NOTIFIED_READ_MESSAGE:
      if (out_handled) *out_handled = 1;
      /* param1 is the chat mid (1:1 peer mid, or group/room mid),
         param2 is the reader mid, createdTime is the read-up-to ms.
         Route by chat-mid prefix: u/U is a 1:1 conversation and uses the
         single peerLastRead* columns; c/C/r/R is a group/room and writes
         a per-member row in message_box_readers_v2 instead. */
      if (op.param1 && op.param1[0]) {
        char p = op.param1[0];
        if (p == 'u' || p == 'U') {
          rc = enil_db_message_box_set_peer_read(db, op.param1, op.param2,
                                                  op.createdTime);
        } else if (op.param2 && op.param2[0]) {
          rc = enil_db_message_box_set_group_reader(db, op.param1, op.param2,
                                                     op.createdTime);
        } else {
          rc = SQLITE_ERROR;
        }
      } else {
        rc = SQLITE_ERROR;
      }
      talk_operation_free(&op);
      cJSON_Delete(root);
      return rc;

    case ENIL_OP_SEND_CHAT_CHECKED:
      if (out_handled) *out_handled = 1;
      /* Multi-device seen sync: this account marked the chat as seen on
         another client (typically the paired phone). param1=chat_mid,
         param2=last-seen message_id. Zero our local unread count. */
      rc = (op.param1 && op.param1[0])
           ? enil_db_message_box_mark_seen(db, op.param1, op.param2)
           : SQLITE_ERROR;
      talk_operation_free(&op);
      cJSON_Delete(root);
      return rc;

    case ENIL_OP_DESTROY_MESSAGE:
    case ENIL_OP_NOTIFIED_DESTROY_MESSAGE:
      if (out_handled) *out_handled = 1;
      /* param1=chat_mid, param2=message_id */
      rc = (op.param2 && op.param2[0])
           ? enil_db_message_mark_deleted(db, op.param2)
           : SQLITE_ERROR;
      talk_operation_free(&op);
      cJSON_Delete(root);
      return rc;

    case ENIL_OP_UPDATE_CONTACT:
      if (out_handled) *out_handled = 1;
      /* param1=mid of the contact whose profile changed */
      rc = apply_op_contact_update(db, access_token, session_path, op.param1);
      talk_operation_free(&op);
      cJSON_Delete(root);
      return rc;

    case ENIL_OP_UPDATE_CHAT:
    case ENIL_OP_NOTIFIED_UPDATE_CHAT:
      if (out_handled) *out_handled = 1;
      /* param1=chat mid (group/room) whose metadata changed */
      rc = apply_op_chat_update(db, access_token, session_path, op.param1);
      talk_operation_free(&op);
      cJSON_Delete(root);
      return rc;

    case ENIL_OP_SEND_REACTION:
    case ENIL_OP_NOTIFIED_SEND_REACTION:
      if (out_handled) *out_handled = 1;
      rc = apply_op_reaction(db, &op, my_mid);
      talk_operation_free(&op);
      cJSON_Delete(root);
      return rc;

    case ENIL_OP_SEND_MESSAGE:
    case ENIL_OP_RECEIVE_MESSAGE:
      /* Carries an embedded message object — handled by the path below. */
      if (out_handled) *out_handled = 1;
      break;

    default:
      ENIL_LOG("Sync.process_sse_event", "unhandled op type=%d (%s)", op.type, enil_op_type_name(op.type));
      talk_operation_free(&op);
      cJSON_Delete(root);
      return SQLITE_OK;
  }

  if (!op.message || !op.message->chat_id || !op.message->chat_id[0]) {
    talk_operation_free(&op);
    cJSON_Delete(root);
    return SQLITE_ERROR;
  }
  msg     = cJSON_GetObjectItem(root, "message");
  chat_id = op.message->chat_id;
  upsert_chat_for_message(db, msg, chat_id, my_mid);
  backfill_chat_metadata(db, access_token, session_path, chat_id);
  arr = cJSON_CreateArray();
  if (!arr) {
    talk_operation_free(&op);
    cJSON_Delete(root);
    return SQLITE_NOMEM;
  }
  cJSON_AddItemToArray(arr, cJSON_Duplicate(msg, 1));
  rc = enil_db_message_upsert(db, op.message);
  if (rc == SQLITE_OK) {
    cJSON *updated_arr;
    scan_messages_for_stickers(db, arr);
    if (!message_has_plaintext(db, op.message->id))
      decrypt_one_message(db, msg, my_mid, access_token, session_path);
    updated_arr = cJSON_CreateArray();
    if (updated_arr) {
      cJSON *updated_msg = cJSON_Duplicate(msg, 1);
      if (updated_msg) {
        cJSON_AddItemToArray(updated_arr, updated_msg);
        scan_messages_for_stickers(db, updated_arr);
      }
      cJSON_Delete(updated_arr);
    }
    /* Resolve ONLY the assets THIS message references — no table scans. A plain
     * text message has none and renders immediately; an image/sticker/sticon
     * message fetches just its own asset. Full sync (sync_phase_downloading)
     * still owns whole-catalog reconciliation and the retry net for any asset
     * that failed here. contentType: 0=text(+sticons), 1=image, 7=sticker. */
    {
      cJSON *cmeta = cJSON_GetObjectItem(msg, "contentMetadata");
      int    ctype = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(msg, "contentType"));
      int    has_sticon = cJSON_IsObject(cmeta) &&
        (cJSON_GetObjectItem(cmeta, "STICON_OWNERSHIP") != NULL ||
         cJSON_GetObjectItem(cmeta, "REPLACE") != NULL);
      if (has_sticon) sync_one_message_sticons(db, session_path, msg);
      if (ctype == 1) sync_one_message_media(db, session_path, op.message->id);
      if (ctype == 7) sync_one_message_sticker(db, session_path, msg);
    }
  }
  cJSON_Delete(arr);
  talk_operation_free(&op);
  cJSON_Delete(root);
  return rc;
}

/* ============================================================================
 * Parse the talkException code from a raw SSE event payload, or 0 if absent.
 * ==========================================================================*/
int enil_sync_sse_talk_exception_code(const char *event_data)
{
  cJSON *root, *code;
  int value;

  if (!event_data || !event_data[0]) return 0;
  root = cJSON_Parse(event_data);
  if (!root) return 0;
  code = cJSON_GetObjectItem(root, "code");
  value = cJSON_IsNumber(code) ? (int)code->valuedouble : 0;
  cJSON_Delete(root);
  return value;
}

/* ============================================================================
 * Extract a quick summary (chat id, message id, type) from an SSE event
 * payload without touching the database. Returns 1 on success.
 * ==========================================================================*/
int enil_sync_sse_message_info(const char *event_data,
                               ENILSSEMessageInfo *info)
{
  cJSON *root;
  talk_operation_t op;

  if (!event_data || !info) return 0;
  memset(info, 0, sizeof(*info));
  info->to_type = -1;

  root = cJSON_Parse(event_data);
  if (!root) return 0;
  if (talk_operation_parse(root, NULL, &op) != 0) {
    cJSON_Delete(root);
    return 0;
  }

  info->op_type = op.type;
  if (op.message) {
    info->has_message = 1;
    if (op.message->id && op.message->id[0])
      snprintf(info->message_id, sizeof(info->message_id), "%s", op.message->id);
    else if (op.param3 && op.param3[0])
      snprintf(info->message_id, sizeof(info->message_id), "%s", op.param3);
    if (op.message->from_mid && op.message->from_mid[0])
      snprintf(info->from_mid, sizeof(info->from_mid), "%s", op.message->from_mid);
    if (op.message->to_mid && op.message->to_mid[0])
      snprintf(info->to_mid, sizeof(info->to_mid), "%s", op.message->to_mid);
    info->to_type = op.message->toType;
  } else {
    /* No message object — read receipts (type 55) and similar ops.
       Populate message_id from param3 only for pendingSends lookup,
       but has_message stays 0 so no bubble is rendered. */
    if (op.param3 && op.param3[0])
      snprintf(info->message_id, sizeof(info->message_id), "%s", op.param3);
  }

  talk_operation_free(&op);
  cJSON_Delete(root);
  return info->message_id[0] || info->from_mid[0] || info->to_mid[0];
}

static int json_string_array_contains(cJSON *array, const char *value) {
  int i, n;
  if (!cJSON_IsArray(array) || !value || !value[0]) return 0;
  n = cJSON_GetArraySize(array);
  for (i = 0; i < n; i++) {
    cJSON *item = cJSON_GetArrayItem(array, i);
    if (cJSON_IsString(item) && item->valuestring &&
        strcmp(item->valuestring, value) == 0)
      return 1;
  }
  return 0;
}

/* ============================================================================
 * Pick the highest endTime across all segments for one reader. LINE currently
 * emits a single segment per reader in captured traffic, but the API shape is
 * an array — take the max so a future multi-segment response collapses to the
 * correct high-water-mark.
 * ==========================================================================*/
static long long read_range_max_end_time(cJSON *segs) {
  long long max_end = 0;
  int       j, n;
  if (!cJSON_IsArray(segs)) return 0;
  n = cJSON_GetArraySize(segs);
  for (j = 0; j < n; j++) {
    cJSON     *seg = cJSON_GetArrayItem(segs, j);
    long long  t   = 0;
    if (cJSON_IsObject(seg) && enil_json_get_int64(seg, "endTime", &t) == 0
        && t > max_end) max_end = t;
  }
  return max_end;
}

/* ============================================================================
 * Route one (chat_mid, reader_mid, endTime) tuple to the same DB write the
 * SSE op-55 handler uses — peerLastRead* columns for 1:1 (u/U prefix),
 * message_box_readers_v2 row for groups/rooms (c/C/r/R). Other prefixes are
 * silently ignored. The DB functions enforce monotonic overwrite, so calling
 * this after a fresher live op-55 is a no-op.
 * ==========================================================================*/
static void read_range_apply_one(sqlite3    *db,
                                  const char *chat_mid,
                                  const char *reader_mid,
                                  long long   end_time) {
  if (!db || !chat_mid || !chat_mid[0] || !reader_mid || !reader_mid[0]
      || end_time <= 0) return;
  switch (chat_mid[0]) {
    case 'u': case 'U':
      enil_db_message_box_set_peer_read(db, chat_mid, reader_mid, end_time);
      return;
    case 'c': case 'C': case 'r': case 'R':
      enil_db_message_box_set_group_reader(db, chat_mid, reader_mid, end_time);
      return;
  }
}

/* ============================================================================
 * Walk a getMessageReadRange response and dispatch each reader entry to the
 * matching SSE op-55 write path. Returns the number of reader tuples applied
 * (for logging). Caller wraps the call in a transaction.
 * ==========================================================================*/
static int read_range_apply_data(sqlite3 *db, cJSON *data) {
  int i, n_chats, applied = 0;
  if (!db || !cJSON_IsArray(data)) return 0;
  n_chats = cJSON_GetArraySize(data);
  for (i = 0; i < n_chats; i++) {
    cJSON      *entry    = cJSON_GetArrayItem(data, i);
    cJSON      *id_item  = entry ? cJSON_GetObjectItem(entry, "chatId") : NULL;
    cJSON      *ranges   = entry ? cJSON_GetObjectItem(entry, "ranges") : NULL;
    cJSON      *reader_node;
    const char *chat_mid = (id_item && cJSON_IsString(id_item)) ? id_item->valuestring : NULL;
    if (!chat_mid || !chat_mid[0] || !cJSON_IsObject(ranges)) continue;
    cJSON_ArrayForEach(reader_node, ranges) {
      read_range_apply_one(db, chat_mid, reader_node->string,
                           read_range_max_end_time(reader_node));
      applied++;
    }
  }
  return applied;
}

/* --- Main sync entry point --- */

/* PHASE 1 — CONNECTING: dup the caller's access token and acquire the OBS
 * media token. The caller's token is kept fresh by ENILAccount's organic
 * refresh timer, so sync no longer refreshes it here. Returns a malloc'd
 * working token (caller owns it), or NULL on failure. */
static char *sync_phase_connecting(const char *access_token,
                                   const char *session_path)
{
  char *current_token;

  report(ENIL_SYNC_PHASE_CONNECTING, 0, 2, "Connecting...", 0);
  if (!enil_session_bind_identity(session_path)) {
    report(ENIL_SYNC_PHASE_CONNECTING, 0, 2, "Invalid client identity", 1);
    return NULL;
  }
  if (!access_token || !access_token[0]) {
    report(ENIL_SYNC_PHASE_CONNECTING, 0, 2, "Token refresh failed", 1);
    return NULL;
  }
  current_token = strdup(access_token);
  if (!current_token) {
    report(ENIL_SYNC_PHASE_CONNECTING, 0, 2, "Token refresh failed", 1);
    return NULL;
  }
  report(ENIL_SYNC_PHASE_CONNECTING, 1, 2, "Connecting...", 0);
  /* OBS media token — non-fatal if it fails */
  {
    char *obs = enil_line_acquire_obs_token(session_path, current_token);
    free(obs);
  }
  report(ENIL_SYNC_PHASE_CONNECTING, 2, 2, "Connecting...", 0);
  return current_token;
}

/* Upsert the logged-in user's own profile into contacts_v2 with registered=0.
 * Your own messages in a group carry your mid as `from`; the message-name join
 * (enil_db.c kSQL_GetMessageById) spans all of contacts_v2, so this makes them
 * resolve to your name. registered=0 keeps you out of the friends list, whose
 * query filters registered=1 — and you are never your own friend anyway.
 * Strings are borrowed from `profile` (not freed here). Best-effort. */
static int upsert_self_contact(sqlite3 *db, const talk_profile_t *profile) {
  talk_contact_t self;
  if (!db || !profile || !profile->mid) return SQLITE_OK;
  memset(&self, 0, sizeof(self));
  self.mid            = profile->mid;
  self.display_name   = profile->display_name;
  self.status_message = profile->status_message;
  self.picture_path   = profile->picture_path;
  self.registered     = 0;
  return enil_db_talk_contact_upsert(db, &self);
}

/* Linear-probe string set used to dedupe group member mids before backfill.
 * Group/member counts are small, so O(n) membership is fine. */
static int mid_set_contains(char **set, int count, const char *mid) {
  int i;
  for (i = 0; i < count; i++)
    if (strcmp(set[i], mid) == 0) return 1;
  return 0;
}

static void mid_set_add(char ***set, int *count, int *cap, const char *mid) {
  char **tmp;
  int    nc;
  if (!mid || !mid[0] || mid_set_contains(*set, *count, mid)) return;
  if (*count == *cap) {
    nc = *cap ? *cap * 2 : 16;
    tmp = (char **)realloc(*set, (size_t)nc * sizeof(char *));
    if (!tmp) return;
    *set = tmp; *cap = nc;
  }
  (*set)[*count] = strdup(mid);
  (*count)++;
}

/* A LINE mid map (memberMids / inviteeMids) is a JSON object keyed by mid;
 * add each key to the set. */
static void mid_set_add_keys(cJSON *map, char ***set, int *count, int *cap) {
  cJSON *e;
  if (!cJSON_IsObject(map)) return;
  for (e = map->child; e; e = e->next)
    mid_set_add(set, count, cap, e->string);
}

/* Pull member + invitee mids out of a chat's raw JSON
 * (extra.groupExtra.{memberMids,inviteeMids}) into the dedup set. */
static void collect_chat_member_mids(cJSON *chat_raw,
                                     char ***set, int *count, int *cap) {
  cJSON *extra, *ge;
  if (!chat_raw) return;
  extra = cJSON_GetObjectItem(chat_raw, "extra");
  ge    = extra ? cJSON_GetObjectItem(extra, "groupExtra") : NULL;
  if (!ge) return;
  mid_set_add_keys(cJSON_GetObjectItem(ge, "memberMids"),  set, count, cap);
  mid_set_add_keys(cJSON_GetObjectItem(ge, "inviteeMids"), set, count, cap);
}

/* For each collected mid we don't already know, fetch its public profile via
 * getContactsV2 (which returns a profile for ANY user, friend or not) and store
 * it with registered=0 so non-friend group members resolve to a name instead
 * of a raw mid, without entering the friends roster. Idempotent: mids already
 * in contacts_v2 (friends synced earlier, or self) are skipped, so repeat syncs
 * issue no extra network calls. Best-effort — a failed fetch just leaves the
 * mid showing until next time. */
static void backfill_member_contacts(sqlite3 *db, const char *current_token,
                                     const char *session_path,
                                     char **mids, int count) {
  int i;
  for (i = 0; i < count; i++) {
    talk_contact_t c;
    if (enil_db_contact_exists(db, mids[i])) continue;
    memset(&c, 0, sizeof(c));
    if (talk_get_contact_by_mid(current_token, mids[i], &c) != 0) {
      ENIL_LOG("Sync.members", "getContactsV2 failed for %s", mids[i]);
      continue;
    }
    c.registered = 0;  /* group-only contact: resolvable, not a friend */
    if (enil_db_talk_contact_upsert(db, &c) == SQLITE_OK)
      refresh_avatar_for_mid(session_path, mids[i], c.picture_path);
    talk_contact_free(&c);
  }
}

/* PHASE 2 — ACCOUNT: profile + friends + chats + message boxes + avatars.
 * Returns 0 on success, non-zero to abort the whole sync. */
static int sync_phase_account(sqlite3    *db,
                              const char *current_token,
                              const char *session_path)
{
  int rc;

  report(ENIL_SYNC_PHASE_ACCOUNT, 0, 3, "Syncing account...", 0);

  {
    talk_get_profile_req_t profile_req = { 3 /* SYNC_REASON_OPERATION */ };
    talk_profile_t         profile;
    memset(&profile, 0, sizeof(profile));
    if (talk_get_profile(current_token, &profile_req, &profile) != 0) {
      report(ENIL_SYNC_PHASE_ACCOUNT, 0, 3, "Profile fetch failed", 1);
      return 1;
    }
    rc = enil_db_talk_profile_upsert(db, &profile);
    if (rc == SQLITE_OK)
      upsert_self_contact(db, &profile);  /* best-effort name resolution */
    talk_profile_free(&profile);
    if (rc != SQLITE_OK) {
      report(ENIL_SYNC_PHASE_ACCOUNT, 0, 3, "Profile write failed", rc);
      return 1;
    }
  }
  report(ENIL_SYNC_PHASE_ACCOUNT, 1, 3, "Syncing account...", 0);

  {
    talk_contact_t *contacts_arr = NULL;
    int contacts_n = 0, ci;
    if (talk_get_contacts(current_token, &contacts_arr, &contacts_n) != 0) {
      report(ENIL_SYNC_PHASE_ACCOUNT, 1, 3, "Friends fetch failed", 1);
      return 1;
    }
    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
    rc = SQLITE_OK;
    for (ci = 0; ci < contacts_n && rc == SQLITE_OK; ci++) {
      rc = enil_db_talk_contact_upsert(db, &contacts_arr[ci]);
      talk_contact_free(&contacts_arr[ci]);
    }
    for (; ci < contacts_n; ci++) talk_contact_free(&contacts_arr[ci]);
    free(contacts_arr);
    sqlite3_exec(db, rc == SQLITE_OK ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
      report(ENIL_SYNC_PHASE_ACCOUNT, 1, 3, "Friends write failed", rc);
      return 1;
    }
  }
  report(ENIL_SYNC_PHASE_ACCOUNT, 2, 3, "Syncing account...", 0);

  /* Fetch group/room chat metadata: getAllChatMids → batched getChats.
     Member and invited mids are merged for the getChats batch, then the
     per-chat isInvited flag is projected back from the invitedChatMids set
     before sqlite upsert so chats_v2 preserves the membership distinction. */
  {
    talk_get_all_chat_mids_req_t mids_req;
    talk_get_all_chat_mids_t     mids;
    const char                 **all_mids = NULL;
    int                          all_n = 0, ci, ii;
    talk_chat_t                 *chats_arr = NULL;
    int                          chats_n = 0;
    char                       **member_set = NULL;  /* deduped group members */
    int                          member_n = 0, member_cap = 0;

    memset(&mids_req, 0, sizeof(mids_req));
    mids_req.withMemberChats  = 1;
    mids_req.withInvitedChats = 1;
    memset(&mids, 0, sizeof(mids));

    if (talk_get_all_chat_mids(current_token, &mids_req, &mids) != 0) {
      report(ENIL_SYNC_PHASE_ACCOUNT, 2, 3, "Chat mids fetch failed", 1);
      return 1;
    }

    all_n = mids.memberCount + mids.invitedCount;
    if (all_n > 0) {
      all_mids = (const char **)malloc((size_t)all_n * sizeof(char *));
      if (!all_mids) {
        talk_get_all_chat_mids_free(&mids);
        report(ENIL_SYNC_PHASE_ACCOUNT, 2, 3, "Chat mids alloc failed", 1);
        return 1;
      }
      for (ci = 0; ci < mids.memberCount; ci++)
        all_mids[ci] = mids.memberChatMids[ci];
      for (ii = 0; ii < mids.invitedCount; ii++)
        all_mids[mids.memberCount + ii] = mids.invitedChatMids[ii];

      if (talk_get_chats(current_token, all_mids, all_n,
                         &chats_arr, &chats_n) != 0) {
        free(all_mids);
        talk_get_all_chat_mids_free(&mids);
        report(ENIL_SYNC_PHASE_ACCOUNT, 2, 3, "Chats fetch failed", 1);
        return 1;
      }
    }
    free(all_mids);

    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
    rc = SQLITE_OK;
    for (ci = 0; ci < chats_n && rc == SQLITE_OK; ci++) {
      chats_arr[ci].isInvited = 0;
      for (ii = 0; ii < mids.invitedCount; ii++) {
        if (chats_arr[ci].chatMid &&
            strcmp(chats_arr[ci].chatMid, mids.invitedChatMids[ii]) == 0) {
          chats_arr[ci].isInvited = 1;
          break;
        }
      }
      rc = enil_db_chat_upsert(db, &chats_arr[ci]);
      collect_chat_member_mids(chats_arr[ci].raw,
                               &member_set, &member_n, &member_cap);
      talk_chat_free(&chats_arr[ci]);
    }
    for (; ci < chats_n; ci++) talk_chat_free(&chats_arr[ci]);
    free(chats_arr);
    talk_get_all_chat_mids_free(&mids);
    sqlite3_exec(db, rc == SQLITE_OK ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);

    /* Resolve names for non-friend group members. Runs AFTER the chats commit
     * (it issues network calls and its own row writes — never nested in the
     * transaction above) and only when the chats wrote cleanly. */
    if (rc == SQLITE_OK)
      backfill_member_contacts(db, current_token, session_path,
                               member_set, member_n);
    for (ci = 0; ci < member_n; ci++) free(member_set[ci]);
    free(member_set);

    if (rc != SQLITE_OK) {
      report(ENIL_SYNC_PHASE_ACCOUNT, 2, 3, "Chats write failed", rc);
      return 1;
    }
  }

  /* Fetch message boxes: unread counts and last-delivered timestamps */
  {
    talk_message_box_t *boxes = NULL;
    int boxes_n = 0, bi;

    if (talk_get_message_boxes(current_token, &boxes, &boxes_n) != 0) {
      report(ENIL_SYNC_PHASE_ACCOUNT, 2, 3, "Message boxes fetch failed", 1);
      return 1;
    }
    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
    rc = SQLITE_OK;
    for (bi = 0; bi < boxes_n && rc == SQLITE_OK; bi++) {
      rc = enil_db_message_box_upsert(db, &boxes[bi]);
      talk_message_box_free(&boxes[bi]);
    }
    for (; bi < boxes_n; bi++) talk_message_box_free(&boxes[bi]);
    free(boxes);
    sqlite3_exec(db, rc == SQLITE_OK ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
      report(ENIL_SYNC_PHASE_ACCOUNT, 2, 3, "Message boxes write failed", rc);
      return 1;
    }
  }

  report(ENIL_SYNC_PHASE_ACCOUNT, 3, 3, "Syncing account...", 0);
  download_avatars(db, session_path);
  return 0;
}

/* PHASE 3 — MESSAGES: fetch recent messages per chat + inline sticker scan. */
static void sync_phase_messages(sqlite3 *db, const char *current_token)
{
  talk_message_box_t *boxes = NULL;
  int boxes_n = 0, done = 0, i;

  enil_db_message_box_get_all(db, &boxes, &boxes_n);
  report(ENIL_SYNC_PHASE_MESSAGES, 0, boxes_n, "Syncing messages...", 0);

  for (i = 0; i < boxes_n; i++) {
    const char     *chat_id = boxes[i].id;
    talk_message_t *tmsgs   = NULL;
    int             tn      = 0;

    if (!chat_id) {
      done++;
      report(ENIL_SYNC_PHASE_MESSAGES, done, boxes_n,
             "Syncing messages...", 0);
      talk_message_box_free(&boxes[i]);
      continue;
    }

    if (talk_get_recent_messages(current_token, chat_id, MESSAGES_PER_CHAT,
                                 &tmsgs, &tn) == 0 && tn > 0) {
      cJSON *scan_arr = cJSON_CreateArray();
      int ti;
      sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
      for (ti = 0; ti < tn; ti++) {
        enil_db_message_upsert(db, &tmsgs[ti]);
        if (scan_arr && tmsgs[ti].raw)
          cJSON_AddItemReferenceToArray(scan_arr, tmsgs[ti].raw);
      }
      sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
      if (scan_arr) {
        scan_messages_for_stickers(db, scan_arr);
        cJSON_Delete(scan_arr);
      }
      for (ti = 0; ti < tn; ti++) talk_message_free(&tmsgs[ti]);
    }
    free(tmsgs);

    done++;
    report(ENIL_SYNC_PHASE_MESSAGES, done, boxes_n,
           "Syncing messages...", 0);
    talk_message_box_free(&boxes[i]);
  }
  free(boxes);
}

/* PHASE 3b — READ RANGE: eager peer-read-state for every chat.
 * Mirrors the SSE op-55 write path exactly (set_peer_read for 1:1,
 * set_group_reader for groups/rooms) so first paint of every chat shows
 * accurate receipts instead of waiting for the next live op-55. Runs
 * after PHASE_MESSAGES because the marker renderer resolves the bubble
 * id by joining peerLastReadTime against the user's outgoing messages
 * in messages_v2, which only just landed. */
static void sync_phase_read_range(sqlite3 *db, const char *current_token)
{
  enum { READ_RANGE_BATCH_SIZE = 100 };
  talk_message_box_t *boxes   = NULL;
  int                 boxes_n = 0, i;

  enil_db_message_box_get_all(db, &boxes, &boxes_n);
  if (boxes_n > 0) {
    const char **batch = (const char **)calloc((size_t)boxes_n, sizeof(*batch));
    int          j;
    if (batch) {
      for (j = 0; j < boxes_n; j++) batch[j] = boxes[j].id;
      for (j = 0; j < boxes_n; j += READ_RANGE_BATCH_SIZE) {
        int    chunk = (boxes_n - j < READ_RANGE_BATCH_SIZE)
                       ? (boxes_n - j) : READ_RANGE_BATCH_SIZE;
        cJSON *data  = talk_get_message_read_range(current_token,
                                                    &batch[j], chunk);
        if (!data) continue;
        sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
        read_range_apply_data(db, data);
        sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
        cJSON_Delete(data);
      }
      free(batch);
    }
  }
  for (i = 0; i < boxes_n; i++) talk_message_box_free(&boxes[i]);
  free(boxes);
}

/* PHASE 4 — DECRYPTING: decrypt pending E2EE messages, then re-extract the
 * REPLACE field for sticon messages decrypted before REPLACE storage existed.
 * Both sub-loops share one done/total counter. */
static void sync_phase_decrypting(sqlite3    *db,
                                  const char *current_token,
                                  const char *my_mid,
                                  const char *session_path)
{
  cJSON *pending      = enil_db_get_messages_needing_decrypt(db);
  cJSON *sticon_msgs  = enil_db_get_messages_needing_replace(db);
  int    n_decrypt    = pending     ? cJSON_GetArraySize(pending)     : 0;
  int    n_replace    = sticon_msgs ? cJSON_GetArraySize(sticon_msgs) : 0;
  int    total        = n_decrypt + n_replace;
  int    done         = 0;
  int    i;
  session_t session;
  cJSON *restore_state = NULL, *e2ee_keys = NULL;
  int    restore_changed = 0;
  e2ee_pubkey_cache_t pubkey_cache;
  group_key_cache_t   group_key_cache;
  memset(&session, 0, sizeof(session));
  memset(&pubkey_cache, 0, sizeof(pubkey_cache));
  memset(&group_key_cache, 0, sizeof(group_key_cache));

  report(ENIL_SYNC_PHASE_DECRYPTING, 0, total, "Decrypting...", 0);
  sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);

  if (session_path)
    load_session(session_path, &session, &restore_state, &e2ee_keys);

  for (i = 0; i < n_decrypt; i++) {
    cJSON      *entry      = cJSON_GetArrayItem(pending, i);
    cJSON      *mid_item   = cJSON_GetObjectItem(entry, "message_id");
    cJSON      *raw_item   = cJSON_GetObjectItem(entry, "raw_json");
    const char *message_id = cJSON_IsString(mid_item) ? mid_item->valuestring : NULL;
    const char *raw_json   = cJSON_IsString(raw_item)  ? raw_item->valuestring  : NULL;
    cJSON      *msg, *chunks, *meta, *to_type_item;
    cJSON      *old_restore;
    char       *plaintext = NULL;
    char       *replace_json = NULL;
    int         to_type;

    if (!message_id || !raw_json) {
      done++;
      report(ENIL_SYNC_PHASE_DECRYPTING, done, total, "Decrypting...", 0);
      continue;
    }

    msg = cJSON_Parse(raw_json);
    if (!msg) {
      done++;
      report(ENIL_SYNC_PHASE_DECRYPTING, done, total, "Decrypting...", 0);
      continue;
    }

    if (is_unsupported_location_message(msg)) {
      ENIL_LOG("Sync.decrypting",
               "%s: unsupported location; leaving undecrypted", message_id);
      enil_db_set_message_text(db, message_id, NULL, ENIL_DECRYPT_NOT_TEXT);
      cJSON_Delete(msg);
      done++;
      report(ENIL_SYNC_PHASE_DECRYPTING, done, total, "Decrypting...", 0);
      continue;
    }

    chunks       = cJSON_GetObjectItem(msg, "chunks");
    meta         = cJSON_GetObjectItem(msg, "contentMetadata");
    to_type_item = cJSON_GetObjectItem(msg, "toType");

    if (cJSON_IsArray(chunks) && cJSON_GetArraySize(chunks) >= 5 &&
        cJSON_IsObject(meta)  && cJSON_GetObjectItem(meta, "e2eeVersion")) {
      char *key_material = NULL;
      to_type     = cJSON_IsNumber(to_type_item) ? (int)to_type_item->valuedouble : 0;
      old_restore = restore_state;

      if (to_type == 0)
        plaintext = decrypt_user_message(msg, my_mid, current_token,
                                         e2ee_keys, &restore_state, &key_material,
                                         &replace_json, &pubkey_cache);
      else
        plaintext = decrypt_group_message(msg, current_token,
                                          e2ee_keys, &restore_state, &key_material,
                                          &replace_json, &pubkey_cache, &group_key_cache);

      if (restore_state != old_restore) restore_changed = 1;

      if (plaintext) {
        char *updated_raw = NULL;
        if (replace_json && cJSON_IsObject(meta)) {
          cJSON_DeleteItemFromObject(meta, "REPLACE");
          cJSON_AddStringToObject(meta, "REPLACE", replace_json);
          updated_raw = cJSON_PrintUnformatted(msg);
        }
        if (updated_raw) {
          enil_db_set_message_plaintext(db, message_id, plaintext,
                                        updated_raw, replace_json,
                                        ENIL_DECRYPT_SUCCESS);
          free(updated_raw);
        } else {
          enil_db_set_message_text(db, message_id, plaintext, ENIL_DECRYPT_SUCCESS);
        }
        free(plaintext);
      } else if (key_material) {
        enil_db_set_message_enc_km(db, message_id, key_material);
      } else {
        enil_db_set_message_text(db, message_id, NULL, ENIL_DECRYPT_FAILED);
      }
      free(key_material);
      free(replace_json);
    } else {
      enil_db_set_message_text(db, message_id, NULL, ENIL_DECRYPT_NOT_TEXT);
    }

    cJSON_Delete(msg);
    done++;
    report(ENIL_SYNC_PHASE_DECRYPTING, done, total, "Decrypting...", 0);
  }

  /* Commit the initial decrypt writes. The REPLACE loop below runs without
   * a wrapping transaction so each write auto-commits before report() is
   * called. This ensures all DB writes are visible to the main thread
   * before syncDidFinish fires (report fires it via waitUntilDone:NO). */
  sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);

  /* Re-extract REPLACE for already-decrypted sticon messages.
   * These were decrypted before REPLACE extraction was added. We re-call
   * the worker with the same ciphertext (ECDH is deterministic) and only
   * update raw_json — never touch text or decrypt_status. */
  {
    int si;
    for (si = 0; si < n_replace; si++) {
      cJSON      *entry      = cJSON_GetArrayItem(sticon_msgs, si);
      const char *message_id = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "message_id"));
      const char *raw_json_s = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "raw_json"));
      cJSON      *msg2, *chunks2, *meta2, *to_type_item2;
      char       *replace_json2 = NULL;
      char       *dummy_text = NULL, *dummy_km = NULL;
      cJSON      *old_restore2;
      int         to_type2;

      if (!message_id || !raw_json_s) {
        done++;
        report(ENIL_SYNC_PHASE_DECRYPTING, done, total, "Decrypting...", 0);
        continue;
      }
      msg2 = cJSON_Parse(raw_json_s);
      if (!msg2) {
        ENIL_LOG("Sync.replace", "%s: raw_json parse failed", message_id);
        done++;
        report(ENIL_SYNC_PHASE_DECRYPTING, done, total, "Decrypting...", 0);
        continue;
      }

      chunks2       = cJSON_GetObjectItem(msg2, "chunks");
      meta2         = cJSON_GetObjectItem(msg2, "contentMetadata");
      to_type_item2 = cJSON_GetObjectItem(msg2, "toType");
      to_type2      = cJSON_IsNumber(to_type_item2) ? (int)to_type_item2->valuedouble : 0;

      if (!cJSON_IsArray(chunks2) || cJSON_GetArraySize(chunks2) < 5 ||
          !cJSON_IsObject(meta2)) {
        cJSON_Delete(msg2);
        done++;
        report(ENIL_SYNC_PHASE_DECRYPTING, done, total, "Decrypting...", 0);
        continue;
      }

      old_restore2 = restore_state;
      if (to_type2 == 0)
        dummy_text = decrypt_user_message(msg2, my_mid, current_token,
                                          e2ee_keys, &restore_state, &dummy_km,
                                          &replace_json2, &pubkey_cache);
      else
        dummy_text = decrypt_group_message(msg2, current_token,
                                           e2ee_keys, &restore_state, &dummy_km,
                                           &replace_json2, &pubkey_cache, &group_key_cache);

      if (restore_state != old_restore2) restore_changed = 1;
      free(dummy_text);
      free(dummy_km);

      if (replace_json2 && cJSON_IsObject(meta2)) {
        char *updated_raw2;
        cJSON_DeleteItemFromObject(meta2, "REPLACE");
        cJSON_AddStringToObject(meta2, "REPLACE", replace_json2);
        updated_raw2 = cJSON_PrintUnformatted(msg2);
        if (updated_raw2) {
          enil_db_set_message_raw_json(db, message_id, updated_raw2, replace_json2);
          free(updated_raw2);
        }
      }
      free(replace_json2);
      cJSON_Delete(msg2);
      done++;
      report(ENIL_SYNC_PHASE_DECRYPTING, done, total, "Decrypting...", 0);
    }
  }

  cJSON_Delete(pending);
  cJSON_Delete(sticon_msgs);

  ENIL_LOG("Sync.decrypting", "public-key cache held %d distinct (mid,keyId)",
           pubkey_cache.count);
  ENIL_LOG("Sync.decrypting", "group-key cache held %d distinct (groupMid,keyId)",
           group_key_cache.count);
  pubkey_cache_free(&pubkey_cache);
  group_key_cache_free(&group_key_cache);

  if (restore_changed && restore_state)
    save_restore_state(session_path, &session, restore_state);

  enil_session_free(&session);
}

/* PHASE 5 — PREPARING DOWNLOADS: stickershop + sticonshop catalog and the
 * sticon meta.json expansion/enrichment inventory. */
static void sync_phase_preparing(sqlite3    *db,
                                 const char *current_token,
                                 const char *session_path)
{
  sticker_package_t *stk_pkgs = NULL;
  sticon_package_t  *sti_pkgs = NULL;
  int n_stk = 0, n_sti = 0, i;
  /* Pre-existing discovered packs that need alt_text enrichment. Captured
   * before the owned loop so we don't enrich a pack we're about to expand. */
  cJSON *needs_alt    = enil_db_sticon_packages_needing_alt(db);
  int    n_alt        = needs_alt ? cJSON_GetArraySize(needs_alt) : 0;
  cJSON *pending_meta;
  cJSON *remaining_meta;
  int total = 0, processed = 0;
  int n_meta = 0;
  int p;

  sticker_package_fetch_all(current_token, &stk_pkgs, &n_stk);
  sticon_package_fetch_all (current_token, &sti_pkgs, &n_sti);
  pending_meta = enil_db_sticon_packages_needing_meta(db);
  n_meta       = pending_meta ? cJSON_GetArraySize(pending_meta) : 0;
  total = n_stk + n_sti + n_meta + n_alt;
  report(ENIL_SYNC_PHASE_PREPARING_DOWNLOADS, 0, total,
         "Preparing downloads...", 0);

  /* --- Stickershop: upsert package + expand stickerIdRanges into stickers_v2 */
  sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
  for (p = 0; p < n_stk; p++) {
    const sticker_package_t *pkg = &stk_pkgs[p];
    const char *opt;
    int r, nr;
    long long total_stickers = 0;

    enil_db_sticker_package_upsert(db, pkg);

    opt = sticker_option_for_item(
            cJSON_GetObjectItem(
              cJSON_GetObjectItem(
                cJSON_GetObjectItem(pkg->raw, "productTypeSummary"),
                "stickerSummary"),
              "stickerResourceType"));
    nr = cJSON_IsArray(pkg->stickerIdRanges) ? cJSON_GetArraySize(pkg->stickerIdRanges) : 0;
    for (r = 0; r < nr; r++) {
      cJSON *range    = cJSON_GetArrayItem(pkg->stickerIdRanges, r);
      long long start = enil_json_coerce_int64(cJSON_GetObjectItem(range, "start"));
      long long size  = enil_json_coerce_int64(cJSON_GetObjectItem(range, "size"));
      long long k;
      for (k = 0; k < size; k++) {
        char stk_id[32];
        snprintf(stk_id, sizeof(stk_id), "%lld", start + k);
        enil_db_sticker_upsert(db, stk_id, pkg->id, opt, NULL);
      }
      total_stickers += size;
    }
    ENIL_LOG("Sync.catalog", "%s: %lld stickers", pkg->id, total_stickers);

    processed++;
    report(ENIL_SYNC_PHASE_PREPARING_DOWNLOADS,
           processed, total, "Preparing downloads...", 0);
  }

  /* --- Sticonshop: upsert package + full expansion (owned packs only) */
  for (p = 0; p < n_sti; p++) {
    const sticon_package_t *pkg = &sti_pkgs[p];
    enil_db_sticon_package_upsert(db, pkg);
    sync_sticonshop_meta(db, session_path, pkg->id, 1 /* expand */);
    processed++;
    report(ENIL_SYNC_PHASE_PREPARING_DOWNLOADS,
           processed, total, "Preparing downloads...", 0);
  }
  sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);

  for (p = 0; p < n_stk; p++) sticker_package_free(&stk_pkgs[p]);
  for (p = 0; p < n_sti; p++) sticon_package_free (&sti_pkgs[p]);
  free(stk_pkgs);
  free(sti_pkgs);

  /* --- Catch-up: owned packs that were marked as needing meta before this
   * run (e.g. previously fetched via the API but never expanded). The
   * remaining_meta filter avoids re-expanding any pack the owned loop above
   * already handled this run. */
  remaining_meta = enil_db_sticon_packages_needing_meta(db);
  if (n_meta > 0) {
    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
    for (i = 0; i < n_meta; i++) {
      cJSON      *item   = cJSON_GetArrayItem(pending_meta, i);
      const char *pkg_id = cJSON_IsString(item) ? item->valuestring : NULL;
      if (json_string_array_contains(remaining_meta, pkg_id))
        sync_sticonshop_meta(db, session_path, pkg_id, 1 /* expand */);
      processed++;
      report(ENIL_SYNC_PHASE_PREPARING_DOWNLOADS,
             processed, total, "Preparing downloads...", 0);
    }
    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  }
  cJSON_Delete(remaining_meta);
  cJSON_Delete(pending_meta);

  /* --- Enrichment: discovered (unowned) packs that have sticons_v2 rows
   * missing alt_text. One meta.json fetch per pack updates alt_text on the
   * existing rows only — no new sticon rows are created, so we don't
   * pre-download images for sticons the user hasn't seen in messages. */
  if (n_alt > 0) {
    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
    for (i = 0; i < n_alt; i++) {
      cJSON      *item   = cJSON_GetArrayItem(needs_alt, i);
      const char *pkg_id = cJSON_IsString(item) ? item->valuestring : NULL;
      if (pkg_id && pkg_id[0])
        sync_sticonshop_meta(db, session_path, pkg_id, 0 /* enrich */);
      processed++;
      report(ENIL_SYNC_PHASE_PREPARING_DOWNLOADS,
             processed, total, "Preparing downloads...", 0);
    }
    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  }
  cJSON_Delete(needs_alt);
  report(ENIL_SYNC_PHASE_PREPARING_DOWNLOADS, total, total,
         "Preparing downloads...", 0);
}

/* PHASE 6 — DOWNLOADING: encrypted media, then purchased and seen sticker/
 * sticon images. Count only final download rows so the unit total stays fixed. */
static void sync_phase_downloading(sqlite3    *db,
                                   const char *current_token,
                                   const char *session_path)
{
  cJSON *pending_media     = enil_db_get_messages_needing_media(db);
  cJSON *count_owned_stk = enil_db_get_stickers_needing_download(db, 1);
  cJSON *count_owned_sti = enil_db_get_sticons_needing_download (db, 1);
  cJSON *count_seen_stk  = enil_db_get_stickers_needing_download(db, 0);
  cJSON *count_seen_sti  = enil_db_get_sticons_needing_download (db, 0);
  int    n_media     = pending_media   ? cJSON_GetArraySize(pending_media)   : 0;
  int    n_owned_stk = count_owned_stk ? cJSON_GetArraySize(count_owned_stk) : 0;
  int    n_owned_sti = count_owned_sti ? cJSON_GetArraySize(count_owned_sti) : 0;
  int    n_seen_stk  = count_seen_stk  ? cJSON_GetArraySize(count_seen_stk)  : 0;
  int    n_seen_sti  = count_seen_sti  ? cJSON_GetArraySize(count_seen_sti)  : 0;
  int    total     = n_media + n_owned_stk + n_owned_sti + n_seen_stk + n_seen_sti;
  int    counter   = 0;
  int    downloaded = 0, skipped = 0, errors = 0;
  int    i;

  (void)current_token;

  /* Discard the count probes; run_sticker_downloads re-queries. */
  cJSON_Delete(count_owned_stk);
  cJSON_Delete(count_owned_sti);
  cJSON_Delete(count_seen_stk);
  cJSON_Delete(count_seen_sti);

  report(ENIL_SYNC_PHASE_DOWNLOADING, 0, total, "Downloading...", 0);

  /* --- Sub-loop A: encrypted media downloads --- */
  sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
  {

  /* derive account dir from session_path by stripping the filename */
  char account_dir[1024];
  const char *last_slash = strrchr(session_path, '/');
  if (last_slash && (size_t)(last_slash - session_path) < sizeof(account_dir) - 1) {
    size_t dir_len = (size_t)(last_slash - session_path);
    memcpy(account_dir, session_path, dir_len);
    account_dir[dir_len] = '\0';
  } else {
    account_dir[0] = '\0';
  }

  for (i = 0; i < n_media; i++) {
    cJSON      *entry      = cJSON_GetArrayItem(pending_media, i);
    const char *message_id = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "message_id"));
    const char *chat_id    = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "chat_id"));
    const char *media_path = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "media_path"));
    const char *enc_km, *sid, *oid, *obs_pop, *e2ee_version;
    char        media_dir[1280], chat_dir[1536], dest[1600];
    char        rel_path[512];
    size_t      plain_size;

    if (!message_id || !chat_id || !account_dir[0]) {
      /* Count as an error so the summary balances (was silently dropped). */
      errors++;
      ENIL_LOG("Sync.media", "skipping malformed entry: %s%s%s",
               !message_id     ? "no message_id " : "",
               !chat_id        ? "no chat_id "    : "",
               !account_dir[0] ? "no account_dir" : "");
      counter++;
      report(ENIL_SYNC_PHASE_DOWNLOADING, counter, total, "Downloading...", 0);
      continue;
    }

    enc_km       = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "enc_km"));
    sid          = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "sid"));
    oid          = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "oid"));
    obs_pop      = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "obs_pop"));
    e2ee_version = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "e2ee_version"));
    plain_size   = (size_t)cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "plain_size"));
    /* build dest path: <account_dir>/media/<chat_id>/<message_id>.jpg */
    snprintf(media_dir, sizeof(media_dir), "%s/media", account_dir);
    snprintf(chat_dir,  sizeof(chat_dir),  "%s/%s", media_dir, chat_id);
    snprintf(dest,      sizeof(dest),      "%s/%s.jpg", chat_dir, message_id);
    snprintf(rel_path,  sizeof(rel_path),  "media/%s/%s.jpg", chat_id, message_id);

    mkdir(media_dir, 0755);
    mkdir(chat_dir,  0755);

    if (media_path && media_path[0]) {
      struct stat st;
      char existing[1600];
      snprintf(existing, sizeof(existing), "%s/%s", account_dir, media_path);
      if (stat(existing, &st) == 0 &&
          (plain_size == 0 || (size_t)st.st_size == plain_size)) {
        /* The DB listed this as needing media but the file is already on disk
         * with the expected size — surfaces DB<->disk drift, not steady state
         * (this loop only ever sees rows enil_db_get_messages_needing_media
         * returned). */
        skipped++;
        ENIL_LOG("Sync.media", "%s (%s): already on disk, skipping",
                 message_id, chat_id);
        counter++;
        report(ENIL_SYNC_PHASE_DOWNLOADING, counter, total, "Downloading...", 0);
        continue;
      }
      /* File missing or wrong size — clear stale path so UI won't use it */
      enil_db_set_media_info(db, message_id, NULL, 0, 0, NULL, 0, 0);
      media_path = NULL;
    }

    if (enil_obs_download_message(session_path, message_id,
                                  sid, oid, obs_pop, e2ee_version,
                                  enc_km, plain_size, dest) == 0) {
      ENILThumbInfo tinfo;
      char thumb_dest[1600], thumb_rel[512];
      memset(&tinfo, 0, sizeof(tinfo));
      snprintf(thumb_dest, sizeof(thumb_dest), "%s/%s_t.jpg", chat_dir, message_id);
      snprintf(thumb_rel,  sizeof(thumb_rel),  "media/%s/%s_t.jpg", chat_id, message_id);
      if (enil_thumb_generate(dest, thumb_dest, &tinfo) != 0) {
        thumb_rel[0] = '\0';
        ENIL_LOG("Sync.media", "thumb failed for %s", message_id);
      }
      enil_db_set_media_info(db, message_id, rel_path,
                              tinfo.orig_width, tinfo.orig_height,
                              thumb_rel[0] ? thumb_rel : NULL,
                              tinfo.thumb_width, tinfo.thumb_height);
      downloaded++;
    } else {
      errors++;
      ENIL_LOG("Sync.media", "%s (%s): OBS download failed",
               message_id, chat_id);
    }

    counter++;
    report(ENIL_SYNC_PHASE_DOWNLOADING, counter, total, "Downloading...", 0);
  }
  sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  cJSON_Delete(pending_media);
  ENIL_LOG("Sync.media", "%d downloaded, %d skipped, %d errors", downloaded, skipped, errors);
  } /* end sub-loop A inner block */

  /* --- Sub-loop B: purchased sticker / sticon images --- */
  run_sticker_downloads(db, session_path, 1, 0, &counter, total, 1);
  run_sticker_downloads(db, session_path, 1, 1, &counter, total, 1);

  /* --- Sub-loop C: sticker / sticon images seen in messages (not purchased) --- */
  run_sticker_downloads(db, session_path, 0, 0, &counter, total, 1);
  run_sticker_downloads(db, session_path, 0, 1, &counter, total, 1);

  report(ENIL_SYNC_PHASE_DOWNLOADING, total, total, "Downloading...", 0);
}

/* DONE — seed localRev from getLastOpRevision so the SSE stream can start. */
static void sync_phase_finish(sqlite3 *db, const char *current_token)
{
  long long local_rev = TalkService_getLastOpRevision(current_token);
  if (local_rev < 0 ||
      enil_db_set_local_rev(db, local_rev) != SQLITE_OK) {
    report(ENIL_SYNC_PHASE_DONE, 0, 0, "Could not enable SSE", 1);
    return;
  }
  report(ENIL_SYNC_PHASE_DONE, 0, 0, "Done", 0);
}

/* ============================================================================
 * Run a full sync — profile, friends, chats, recent messages, sticker/sticon
 * package metadata, and media. Each phase is a static helper above; they
 * communicate through sqlite (the DB is the shared state), so this entry
 * point is just the orchestrator. Posts progress notifications between phases.
 * ==========================================================================*/
void enil_sync_all(sqlite3    *db,
                   const char *access_token,
                   const char *my_mid,
                   const char *session_path)
{
  char *current_token = sync_phase_connecting(access_token, session_path);
  if (!current_token) return;

  if (sync_phase_account(db, current_token, session_path) == 0) {
    sync_phase_messages(db, current_token);
    sync_phase_read_range(db, current_token);
    sync_phase_decrypting(db, current_token, my_mid, session_path);
    sync_phase_preparing(db, current_token, session_path);
    sync_phase_downloading(db, current_token, session_path);
    sync_phase_finish(db, current_token);
  }

  free(current_token);
}

/* ============================================================================
 * Build the JS appendMessage(...) call the WebView should evaluate for a
 * just-received SSE message. Returns malloc'd JS or NULL.
 * ==========================================================================*/
char *enil_sync_js_for_sse_message(sqlite3    *db,
                                    const char *my_mid,
                                    const char *event_data,
                                    const char *temp_id,
                                    char       *chat_id_buf,
                                    size_t      chat_id_size)
{
  cJSON         *root, *msg_json, *row;
  const char    *message_id, *chat_id, *msgid, *sender, *text;
  const char    *mpath, *tpath, *from_mid, *stkid, *stkpkg;
  long long      ca;
  int            ctype, ow, oh, tw, th, is_out, decrypt_st;
  char           stk_buf[512];
  int            stk_w, stk_h;
  char          *text_html, *js, *avatar_path;
  ENILHTMLMessage m;

  if (!db || !event_data) return NULL;
  root = cJSON_Parse(event_data);
  if (!root) return NULL;

  msg_json   = cJSON_GetObjectItem(root, "message");
  message_id = msg_json
    ? cJSON_GetStringValue(cJSON_GetObjectItem(msg_json, "id"))
    : NULL;
  if (!message_id || !message_id[0])
    message_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "param3"));
  if (!message_id || !message_id[0]) {
    cJSON_Delete(root);
    return NULL;
  }

  row = enil_db_get_message_by_id(db, message_id);
  if (!row) {
    cJSON_Delete(root);
    return NULL;
  }

  msgid    = cJSON_GetStringValue(cJSON_GetObjectItem(row, "message_id"));
  sender   = cJSON_GetStringValue(cJSON_GetObjectItem(row, "sender_name"));
  chat_id  = cJSON_GetStringValue(cJSON_GetObjectItem(row, "chat_id"));
  ca       = (long long)cJSON_GetNumberValue(cJSON_GetObjectItem(row, "created_at"));
  text     = cJSON_GetStringValue(cJSON_GetObjectItem(row, "text"));
  ctype    = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(row, "content_type"));
  mpath    = cJSON_GetStringValue(cJSON_GetObjectItem(row, "media_path"));
  tpath    = cJSON_GetStringValue(cJSON_GetObjectItem(row, "thumb_path"));
  from_mid = cJSON_GetStringValue(cJSON_GetObjectItem(row, "from_mid"));
  ow       = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(row, "orig_width"));
  oh       = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(row, "orig_height"));
  tw       = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(row, "thumb_width"));
  th       = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(row, "thumb_height"));
  stkid    = cJSON_GetStringValue(cJSON_GetObjectItem(row, "sticker_id"));
  stkpkg   = cJSON_GetStringValue(cJSON_GetObjectItem(row, "sticker_pkg_id"));
  decrypt_st = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(row, "decrypt_status"));
  is_out   = (my_mid && from_mid && from_mid[0] && strcmp(from_mid, my_mid) == 0);

  stk_buf[0] = '\0'; stk_w = 0; stk_h = 0;
  if (ctype == 7 && stkid && stkid[0])
    enil_message_sticker_image_info(db, stkid, stkpkg,
                                    stk_buf, sizeof(stk_buf), &stk_w, &stk_h);

  text_html = enil_message_text_html(db, msgid, text);
  avatar_path = enil_db_avatar_relpath(db, is_out ? NULL : from_mid);

  if (chat_id_buf && chat_id_size > 0)
    snprintf(chat_id_buf, chat_id_size, "%s", chat_id ? chat_id : "");

  memset(&m, 0, sizeof(m));
  m.message_id    = msgid    ? msgid    : "";
  m.sender_name   = sender   ? sender   : "";
  m.text          = text     ? text     : "";
  m.text_html     = text_html ? text_html : "";
  m.media_path    = mpath    ? mpath    : "";
  m.thumb_path    = tpath    ? tpath    : "";
  m.sticker_path  = stk_buf[0] ? stk_buf : "";
  m.avatar_path   = avatar_path ? avatar_path : "";
  m.created_at    = ca;
  m.content_type  = ctype;
  m.is_outgoing   = is_out;
  m.encrypted_unresolved = (decrypt_st == ENIL_DECRYPT_PENDING ||
                            decrypt_st == ENIL_DECRYPT_FAILED) ? 1 : 0;
  m.sticker_width  = stk_w;
  m.sticker_height = stk_h;
  m.orig_width    = ow;
  m.orig_height   = oh;
  m.thumb_width   = tw;
  m.thumb_height  = th;

  js = temp_id ? enil_html_js_update(temp_id, &m) : enil_html_js_append(&m);

  free(text_html);
  free(avatar_path);
  cJSON_Delete(row);
  cJSON_Delete(root);
  return js;
}

char *enil_sync_js_for_sse_reaction(sqlite3    *db,
                                     const char *event_data,
                                     char       *chat_id_buf,
                                     size_t      chat_id_size)
{
  cJSON            *root, *row;
  talk_operation_t  op;
  const char       *chat_id, *reactions;
  char             *js;

  if (chat_id_buf && chat_id_size > 0) chat_id_buf[0] = '\0';
  if (!db || !event_data) return NULL;
  root = cJSON_Parse(event_data);
  if (!root) return NULL;
  if (talk_operation_parse(root, NULL, &op) != 0) {
    cJSON_Delete(root);
    return NULL;
  }
  if (op.type != ENIL_OP_SEND_REACTION &&
      op.type != ENIL_OP_NOTIFIED_SEND_REACTION) {
    talk_operation_free(&op);
    cJSON_Delete(root);
    return NULL;
  }
  /* param1 is the target message id (same field apply_op_reaction keys on). */
  if (!op.param1 || !op.param1[0]) {
    talk_operation_free(&op);
    cJSON_Delete(root);
    return NULL;
  }

  row = enil_db_get_message_by_id(db, op.param1);
  if (!row) {
    talk_operation_free(&op);
    cJSON_Delete(root);
    return NULL;
  }
  chat_id   = cJSON_GetStringValue(cJSON_GetObjectItem(row, "chat_id"));
  reactions = cJSON_GetStringValue(cJSON_GetObjectItem(row, "reactions_json"));
  if (chat_id_buf && chat_id_size > 0)
    snprintf(chat_id_buf, chat_id_size, "%s", chat_id ? chat_id : "");

  js = enil_html_js_reactions(op.param1, reactions);

  cJSON_Delete(row);
  talk_operation_free(&op);
  cJSON_Delete(root);
  return js;
}

char *enil_sync_js_for_sse_delete(sqlite3    *db,
                                   const char *event_data,
                                   char       *chat_id_buf,
                                   size_t      chat_id_size)
{
  cJSON            *root, *row;
  talk_operation_t  op;
  const char       *chat_id;
  char             *js;

  if (chat_id_buf && chat_id_size > 0) chat_id_buf[0] = '\0';
  if (!db || !event_data) return NULL;
  root = cJSON_Parse(event_data);
  if (!root) return NULL;
  if (talk_operation_parse(root, NULL, &op) != 0) {
    cJSON_Delete(root);
    return NULL;
  }
  if (op.type != ENIL_OP_DESTROY_MESSAGE &&
      op.type != ENIL_OP_NOTIFIED_DESTROY_MESSAGE) {
    talk_operation_free(&op);
    cJSON_Delete(root);
    return NULL;
  }
  /* param2 is the unsent message id; param1 is the chat mid. */
  if (!op.param2 || !op.param2[0]) {
    talk_operation_free(&op);
    cJSON_Delete(root);
    return NULL;
  }

  /* Resolve the chat from the stored row so the UI only repaints when that
     chat is open; fall back to param1 if the row is not in the DB. */
  row = enil_db_get_message_by_id(db, op.param2);
  chat_id = row ? cJSON_GetStringValue(cJSON_GetObjectItem(row, "chat_id"))
                : NULL;
  if ((!chat_id || !chat_id[0]) && op.param1 && op.param1[0])
    chat_id = op.param1;
  if (chat_id_buf && chat_id_size > 0)
    snprintf(chat_id_buf, chat_id_size, "%s", chat_id ? chat_id : "");

  js = enil_html_js_deleted(op.param2);

  if (row) cJSON_Delete(row);
  talk_operation_free(&op);
  cJSON_Delete(root);
  return js;
}

/* ============================================================================
 * Build setReadMarker(id) JS for an SSE op 55 (NOTIFIED_READ_MESSAGE). The
 * op's DB write has already landed (peerLastReadTime is fresh), so we just
 * resolve "which message id does the marker belong on now" and emit the
 * push. 1:1 only — param1 of op 55 in a group is the group mid and the
 * stored peerLastReadTime there reflects whoever read last, not a global
 * watermark; render-time gates groups identically.
 * ==========================================================================*/
char *enil_sync_js_for_sse_read_receipt(sqlite3    *db,
                                         const char *my_mid,
                                         const char *event_data,
                                         char       *chat_id_buf,
                                         size_t      chat_id_size)
{
  cJSON            *root;
  talk_operation_t  op;
  char             *msg_id, *js;

  if (chat_id_buf && chat_id_size > 0) chat_id_buf[0] = '\0';
  if (!db || !my_mid || !my_mid[0] || !event_data) return NULL;
  root = cJSON_Parse(event_data);
  if (!root) return NULL;
  if (talk_operation_parse(root, NULL, &op) != 0) {
    cJSON_Delete(root);
    return NULL;
  }
  if (op.type != ENIL_OP_NOTIFIED_READ_MESSAGE ||
      !op.param1 || !op.param1[0] ||
      (op.param1[0] != 'u' && op.param1[0] != 'U')) {
    talk_operation_free(&op);
    cJSON_Delete(root);
    return NULL;
  }
  if (chat_id_buf && chat_id_size > 0)
    snprintf(chat_id_buf, chat_id_size, "%s", op.param1);

  msg_id = enil_db_last_read_outgoing_msg_id(db, op.param1, my_mid);
  js = enil_html_js_set_read_marker(msg_id);
  free(msg_id);
  talk_operation_free(&op);
  cJSON_Delete(root);
  return js;
}

/* ============================================================================
 * Build setSeenMarker(...) JS for an SSE op 40 (SEND_CHAT_CHECKED) — this
 * account has seen the chat on another device (e.g. the paired phone). The DB
 * write already landed (enil_db_message_box_mark_seen advanced
 * lastSeenMessageId and zeroed unreadCount), so re-anchor the gray "Seen"
 * marker at the new lastSeenMessageId. The Seen line is always shown — even
 * when caught up — so op 40 moves it to the bottom (below the just-checked
 * message) rather than clearing it. A NULL id (never seen) yields no marker.
 * param1 = chat mid; fills chat_id_buf. Works for 1:1 and group alike. Caller
 * frees with enil_html_free.
 * ==========================================================================*/
char *enil_sync_js_for_sse_chat_checked(sqlite3    *db,
                                         const char *event_data,
                                         char       *chat_id_buf,
                                         size_t      chat_id_size)
{
  cJSON            *root;
  talk_operation_t  op;
  char             *seen_id, *js;

  if (chat_id_buf && chat_id_size > 0) chat_id_buf[0] = '\0';
  if (!db || !event_data) return NULL;
  root = cJSON_Parse(event_data);
  if (!root) return NULL;
  if (talk_operation_parse(root, NULL, &op) != 0) {
    cJSON_Delete(root);
    return NULL;
  }
  if (op.type != ENIL_OP_SEND_CHAT_CHECKED || !op.param1 || !op.param1[0]) {
    talk_operation_free(&op);
    cJSON_Delete(root);
    return NULL;
  }
  if (chat_id_buf && chat_id_size > 0)
    snprintf(chat_id_buf, chat_id_size, "%s", op.param1);

  seen_id = enil_db_message_box_last_seen(db, op.param1);
  js = enil_html_js_set_seen_marker(seen_id);
  free(seen_id);
  talk_operation_free(&op);
  cJSON_Delete(root);
  return js;
}

/* ============================================================================
 * Group variant of enil_sync_js_for_sse_read_receipt — emits
 * setReadMarkerForReader(reader_mid,name,color_idx,msg_id) so the named pill
 * moves/appears without a full chat reload. Restricted to c/C/r/R-prefixed
 * chats (groups/rooms); returns NULL for 1:1 ops. The DB write has already
 * landed (set_group_reader); we resolve display name + last-read msg id via
 * get_group_reader and compute the same palette index the renderer uses.
 * Caller frees the JS with enil_html_free.
 * ==========================================================================*/
char *enil_sync_js_for_sse_group_read_receipt(sqlite3    *db,
                                               const char *my_mid,
                                               const char *event_data,
                                               char       *chat_id_buf,
                                               size_t      chat_id_size)
{
  cJSON               *root;
  talk_operation_t     op;
  enil_group_reader_t  reader;
  char                *js;

  if (chat_id_buf && chat_id_size > 0) chat_id_buf[0] = '\0';
  if (!db || !my_mid || !my_mid[0] || !event_data) return NULL;
  root = cJSON_Parse(event_data);
  if (!root) return NULL;
  if (talk_operation_parse(root, NULL, &op) != 0) {
    cJSON_Delete(root);
    return NULL;
  }
  if (op.type != ENIL_OP_NOTIFIED_READ_MESSAGE ||
      !op.param1 || !op.param1[0] ||
      !op.param2 || !op.param2[0] ||
      (op.param1[0] != 'c' && op.param1[0] != 'C' &&
       op.param1[0] != 'r' && op.param1[0] != 'R')) {
    talk_operation_free(&op);
    cJSON_Delete(root);
    return NULL;
  }
  if (chat_id_buf && chat_id_size > 0)
    snprintf(chat_id_buf, chat_id_size, "%s", op.param1);

  if (enil_db_message_box_get_group_reader(db, op.param1, op.param2, my_mid,
                                            &reader) != SQLITE_OK ||
      !reader.last_read_message_id) {
    enil_group_reader_free(&reader);
    talk_operation_free(&op);
    cJSON_Delete(root);
    return NULL;
  }
  js = enil_html_js_set_read_marker_for_reader(reader.reader_mid,
                                                reader.display_name,
                                                reader.last_read_message_id);
  enil_group_reader_free(&reader);
  talk_operation_free(&op);
  cJSON_Delete(root);
  return js;
}
