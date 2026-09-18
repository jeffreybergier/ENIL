#include "enil_thrift.h"
#include "enil_b64.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LIMIT (8U * 1024U * 1024U)
static const char schema_text[] = "{"
#include "enil_thrift_schema.inc"
                                  "}";
static cJSON *schema;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void init_schema(void) { schema = cJSON_Parse(schema_text); }
static cJSON *at(cJSON *a, int i) { return cJSON_GetArrayItem(a, i); }
static const char *str(cJSON *v) { return v && cJSON_IsString(v) ? v->valuestring : ""; }
static const char *kind(cJSON *t) { return str(cJSON_IsArray(t) ? at(t, 0) : t); }
static cJSON *fields(cJSON *t) { return cJSON_GetObjectItemCaseSensitive(schema, str(t)); }
static int type(cJSON *t) {
  const char *s = kind(t);
  if (!strcmp(s, "bool"))
    return 1;
  if (!strcmp(s, "byte"))
    return 3;
  if (!strcmp(s, "i16"))
    return 4;
  if (!strcmp(s, "i32"))
    return 5;
  if (!strcmp(s, "i64"))
    return 6;
  if (!strcmp(s, "double"))
    return 7;
  if (!strcmp(s, "string") || !strcmp(s, "binary"))
    return 8;
  if (!strcmp(s, "list"))
    return 9;
  if (!strcmp(s, "set"))
    return 10;
  if (!strcmp(s, "map"))
    return 11;
  return 12;
}
static int put(ENILBuf *b, const void *p, size_t n) {
  char *q;
  if (n > LIMIT || b->size > LIMIT - n)
    return 0;
  q = realloc(b->data, b->size + n + 1);
  if (!q)
    return 0;
  b->data = q;
  if (n)
    memcpy(q + b->size, p, n);
  b->size += n;
  q[b->size] = 0;
  return 1;
}
static int byte(ENILBuf *b, unsigned char c) { return put(b, &c, 1); }
static int var(ENILBuf *b, uint64_t n) {
  do {
    unsigned char c = n & 127;
    n >>= 7;
    if (!byte(b, c | (n ? 128 : 0)))
      return 0;
  } while (n);
  return 1;
}
static uint64_t zig(int64_t n) { return ((uint64_t)n << 1) ^ (uint64_t)-(n < 0); }
static int integer(cJSON *v, int64_t *n) {
  if (cJSON_IsString(v)) {
    char *end;
    errno = 0;
    *n = strtoll(v->valuestring, &end, 10);
    return !errno && end != v->valuestring && !*end;
  }
  if (cJSON_IsNumber(v) && v->valuedouble >= -9007199254740991.0 &&
      v->valuedouble <= 9007199254740991.0) {
    *n = (int64_t)v->valuedouble;
    return (double)*n == v->valuedouble;
  }
  return 0;
}
static int write_value(ENILBuf *, cJSON *, cJSON *, int);
static int write_struct(ENILBuf *b, cJSON *fs, cJSON *v, int depth) {
  cJSON *f;
  int last = 0, index = 0;
  if (depth > 40 || !fs || (!cJSON_IsObject(v) && !cJSON_IsArray(v)))
    return 0;
  for (f = fs->child; f; f = f->next, index++) {
    int id = at(f, 0)->valueint, code = type(at(f, 2)), delta = id - last;
    cJSON *val =
        cJSON_IsArray(v) ? at(v, index) : cJSON_GetObjectItemCaseSensitive(v, str(at(f, 1)));
    if (!val || cJSON_IsNull(val))
      continue;
    if (code == 1) {
      if (!cJSON_IsBool(val))
        return 0;
      code = cJSON_IsTrue(val) ? 1 : 2;
    }
    if (!byte(b, (unsigned char)((delta > 0 && delta < 16 ? delta << 4 : 0) | code)))
      return 0;
    if ((delta <= 0 || delta >= 16) && !var(b, zig(id)))
      return 0;
    if (code > 2 && !write_value(b, at(f, 2), val, depth + 1))
      return 0;
    last = id;
  }
  return byte(b, 0);
}
static int write_value(ENILBuf *b, cJSON *t, cJSON *v, int depth) {
  int code = type(t);
  int64_t n;
  cJSON *item;
  if (depth > 40)
    return 0;
  if (code == 1)
    return cJSON_IsBool(v) && byte(b, cJSON_IsTrue(v) ? 1 : 2);
  if (code >= 3 && code <= 6) {
    if (!integer(v, &n))
      return 0;
    if ((code == 3 && (n < -128 || n > 127)) || (code == 4 && (n < -32768 || n > 32767)) ||
        (code == 5 && (n < INT32_MIN || n > INT32_MAX)))
      return 0;
    return code == 3 ? byte(b, (unsigned char)n) : var(b, zig(n));
  }
  if (code == 7) {
    uint64_t bits;
    int i;
    if (!cJSON_IsNumber(v))
      return 0;
    memcpy(&bits, &v->valuedouble, 8);
    for (i = 0; i < 8; i++)
      if (!byte(b, (unsigned char)(bits >> (i * 8))))
        return 0;
    return 1;
  }
  if (code == 8) {
    unsigned char *bytes = NULL;
    int len, ok;
    if (!cJSON_IsString(v))
      return 0;
    if (strcmp(kind(t), "binary"))
      return var(b, strlen(str(v))) && put(b, str(v), strlen(str(v)));
    len = enil_b64_decode_alloc(str(v), &bytes);
    if (len < 0)
      return 0;
    ok = var(b, (uint64_t)len) && put(b, bytes, (size_t)len);
    free(bytes);
    return ok;
  }
  if (code == 9 || code == 10) {
    int count = cJSON_GetArraySize(v), ct = type(at(t, 1));
    if (!cJSON_IsArray(v) || !byte(b, (unsigned char)((count < 15 ? count : 15) << 4 | ct)))
      return 0;
    if (count >= 15 && !var(b, (uint64_t)count))
      return 0;
    for (item = v->child; item; item = item->next)
      if (!write_value(b, at(t, 1), item, depth + 1))
        return 0;
    return 1;
  }
  if (code == 11) {
    int count = cJSON_GetArraySize(v);
    if (!cJSON_IsObject(v) || !var(b, (uint64_t)count))
      return 0;
    if (count && !byte(b, (unsigned char)(type(at(t, 1)) << 4 | type(at(t, 2)))))
      return 0;
    for (item = v->child; item; item = item->next) {
      cJSON *key = cJSON_CreateString(item->string);
      int ok = key && write_value(b, at(t, 1), key, depth + 1);
      cJSON_Delete(key);
      if (!ok || !write_value(b, at(t, 2), item, depth + 1))
        return 0;
    }
    return 1;
  }
  return write_struct(b, fields(t), v, depth + 1);
}
typedef struct {
  const unsigned char *p;
  size_t n, pos;
  int bad;
  int strict_strings;
} Reader;
static unsigned int get(Reader *r) {
  if (r->pos >= r->n) {
    r->bad = 1;
    return 0;
  }
  return r->p[r->pos++];
}
static uint64_t uv(Reader *r) {
  uint64_t n = 0;
  int i;
  for (i = 0; i < 10; i++) {
    unsigned int c = get(r);
    if (i == 9 && c > 1)
      r->bad = 1;
    n |= (uint64_t)(c & 127) << (i * 7);
    if (!(c & 128))
      return n;
  }
  r->bad = 1;
  return 0;
}
static int64_t unzig(uint64_t n) { return (int64_t)(n >> 1) ^ -(int64_t)(n & 1); }
static cJSON *read_value(Reader *, cJSON *, int, int, int);
static cJSON *read_struct(Reader *r, cJSON *fs, int depth) {
  cJSON *o = cJSON_CreateObject();
  int last = 0;
  if (depth > 40 || !o) {
    r->bad = 1;
    return o;
  }
  while (!r->bad) {
    int h = (int)get(r), id;
    cJSON *f, *t = NULL, *v;
    const char *name = NULL;
    char number[20];
    if (!h)
      break;
    id = (h >> 4) ? last + (h >> 4) : (int)unzig(uv(r));
    last = id;
    if (id < -32768 || id > 32767) {
      r->bad = 1;
      break;
    }
    for (f = fs ? fs->child : NULL; f; f = f->next)
      if (at(f, 0)->valueint == id) {
        t = at(f, 2);
        name = str(at(f, 1));
        break;
      }
    v = read_value(r, t, h & 15, depth + 1, 1);
    if (!v) {
      r->bad = 1;
      break;
    }
    if (!name) {
      snprintf(number, sizeof(number), "%d", id);
      name = number;
    }
    if (cJSON_HasObjectItem(o, name) || !cJSON_AddItemToObject(o, name, v)) {
      cJSON_Delete(v);
      r->bad = 1;
      break;
    }
  }
  return o;
}
static cJSON *read_value(Reader *r, cJSON *t, int code, int depth, int field) {
  if (depth > 40) {
    r->bad = 1;
    return NULL;
  }
  if (t && strcmp(kind(t), "_any") && code != type(t) && !(type(t) == 1 && code == 2)) {
    r->bad = 1;
    return NULL;
  }
  if (code == 1 || code == 2) {
    if (!field)
      code = (int)get(r);
    if (code != 1 && code != 2)
      r->bad = 1;
    return cJSON_CreateBool(code == 1);
  }
  if (code >= 3 && code <= 6) {
    int64_t n = code == 3 ? (int8_t)get(r) : unzig(uv(r));
    char s[32];
    if (code == 6) {
      snprintf(s, sizeof(s), "%lld", (long long)n);
      return cJSON_CreateString(s);
    }
    return cJSON_CreateNumber((double)n);
  }
  if (code == 7) {
    uint64_t bits = 0;
    double d;
    int i;
    for (i = 0; i < 8; i++)
      bits |= (uint64_t)get(r) << (i * 8);
    memcpy(&d, &bits, 8);
    return cJSON_CreateNumber(d);
  }
  if (code == 8) {
    uint64_t n = uv(r);
    cJSON *v;
    char *s;
    if (n > r->n - r->pos || n > LIMIT) {
      r->bad = 1;
      return NULL;
    }
    if (!strcmp(kind(t), "binary"))
      s = enil_b64_encode(r->p + r->pos, (size_t)n);
    else {
      if ((t || r->strict_strings) && memchr(r->p + r->pos, 0, (size_t)n)) {
        r->bad = 1;
        return NULL;
      }
      s = malloc((size_t)n + 1);
      if (s) {
        memcpy(s, r->p + r->pos, (size_t)n);
        s[n] = 0;
      }
    }
    r->pos += (size_t)n;
    v = s ? cJSON_CreateString(s) : NULL;
    free(s);
    return v;
  }
  if (code == 12)
    return read_struct(r, fields(t), depth + 1);
  if (code >= 9 && code <= 11) {
    uint64_t n, i;
    int kt = 0, vt;
    cJSON *o;
    if (code == 11) {
      n = uv(r);
      vt = n ? (int)get(r) : 0;
      kt = vt >> 4;
      vt &= 15;
      o = cJSON_CreateObject();
    } else {
      vt = (int)get(r);
      n = (unsigned)vt >> 4;
      vt &= 15;
      if (n == 15)
        n = uv(r);
      o = cJSON_CreateArray();
    }
    if (n > 100000 || n > r->n - r->pos || !o) {
      r->bad = 1;
      return o;
    }
    for (i = 0; i < n && !r->bad; i++) {
      cJSON *k = NULL, *v;
      char key[40];
      if (code == 11)
        k = read_value(r, at(t, 1), kt, depth + 1, 0);
      v = read_value(r, at(t, code == 11 ? 2 : 1), vt, depth + 1, 0);
      if (!v || (code == 11 && !k)) {
        r->bad = 1;
        cJSON_Delete(k);
        cJSON_Delete(v);
        break;
      }
      if (code == 11) {
        if (cJSON_IsString(k))
          cJSON_AddItemToObject(o, str(k), v);
        else if (cJSON_IsNumber(k)) {
          snprintf(key, sizeof(key), "%.0f", k->valuedouble);
          cJSON_AddItemToObject(o, key, v);
        } else {
          r->bad = 1;
          cJSON_Delete(v);
        }
        cJSON_Delete(k);
      } else
        cJSON_AddItemToArray(o, v);
    }
    return o;
  }
  r->bad = 1;
  return NULL;
}
int enil_thrift_encode(const char *method, cJSON *args, ENILBuf *out) {
  char name[160];
  cJSON *fs;
  int ok;
  pthread_once(&once, init_schema);
  snprintf(name, sizeof(name), "%s_args", method);
  fs = cJSON_GetObjectItemCaseSensitive(schema, name);
  if (!fs)
    return 0;
  ok = byte(out, 0x82) && byte(out, 0x21) && var(out, 0) && var(out, strlen(method)) &&
       put(out, method, strlen(method)) && write_struct(out, fs, args, 0);
  if (!ok)
    enil_buf_free(out);
  return ok;
}
static cJSON *decode(const char *method, const void *data, size_t size,
                      int *exception) {
  Reader r;
  uint64_t n;
  char name[160];
  cJSON *o;
  int mt;
  pthread_once(&once, init_schema);
  if (!data || size > LIMIT)
    return NULL;
  r.p = data;
  r.n = size;
  r.pos = 0;
  r.bad = 0;
  r.strict_strings = exception != NULL;
  if (get(&r) != 0x82)
    return NULL;
  mt = (int)get(&r);
  if ((mt & 31) != 1 || ((mt >> 5) != 2 && !(exception && (mt >> 5) == 3)))
    return NULL;
  if (exception) *exception = (mt >> 5) == 3;
  if (uv(&r) != 0)
    return NULL;
  n = uv(&r);
  if (n != strlen(method) || n > r.n - r.pos || memcmp(r.p + r.pos, method, (size_t)n))
    return NULL;
  r.pos += (size_t)n;
  snprintf(name, sizeof(name), "%s_result", method);
  o = read_struct(&r, exception ? NULL : cJSON_GetObjectItemCaseSensitive(schema, name), 0);
  /* Some servers append one redundant STOP. */
  if (r.pos < r.n && r.n - r.pos == 1 && r.p[r.pos] == 0)
    r.pos++;
  if (r.bad || r.pos != r.n) {
    cJSON_Delete(o);
    return NULL;
  }
  return o;
}

cJSON *enil_thrift_decode(const char *method, const void *data, size_t size) {
  return decode(method, data, size, NULL);
}

cJSON *enil_thrift_decode_raw(const char *method, const void *data, size_t size,
                              int *exception) {
  if (!exception) return NULL;
  *exception = 0;
  return decode(method, data, size, exception);
}
