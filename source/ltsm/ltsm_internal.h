#ifndef ENIL_LTSM_INTERNAL_H
#define ENIL_LTSM_INTERNAL_H
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "enil_ltsm.h"
#include "ltsm.h"

#define LTSM_MAX_INPUT (9U * 1024U * 1024U)
#define LTSM_MAX_HOST (64U * 1024U * 1024U)
#define LTSM_MAX_TYPES 128
#define LTSM_MAX_METHODS 128
#define LTSM_MAX_HANDLES 1024

typedef union { U32 i; U64 j; F32 f; F64 d; } Wire;
typedef enum { V_UNDEFINED, V_NUMBER, V_BYTES, V_STRING, V_OBJECT,
               V_ARRAY, V_UINT8_CONSTRUCTOR } ValueKind;
typedef struct Value Value;
struct Value {
  ValueKind kind;
  U64 number;
  U32 pointer, type;
  size_t length;
  unsigned char *data;
  Value **items;
};
typedef enum { T_VOID, T_INT, T_BIGINT, T_FLOAT, T_STRING, T_VAL,
               T_CLASS, T_VIEW, T_WSTRING } TypeKind;
typedef struct {
  U32 id, owner, base, upcast, size, view;
  TypeKind kind;
  const char *name;
} Type;
typedef struct {
  U32 owner, invoker, target, types[16];
  size_t count;
  int instance;
  const char *name;
} Method;
typedef struct Allocation Allocation;
typedef struct {
  m4_ltsmInstance wasm; /* Must be first: imported functions receive this. */
  jmp_buf escape;
  const char *error;
  ENILLTSMCancel cancel;
  void *cancel_context;
  U32 calls;
  Allocation *allocations;
  size_t allocated;
  Type types[LTSM_MAX_TYPES];
  size_t type_count;
  Method methods[LTSM_MAX_METHODS];
  size_t method_count;
  struct { Value *value; unsigned references; } handles[LTSM_MAX_HANDLES];
  Value small_numbers[256], empty_destructors;
  cJSON *request, *response;
} Context;

/* Namespace cross-file helpers without exposing this private ABI in the
 * public header. Function-like aliases leave Value.number and other fields
 * untouched. Generated WASM symbols have their own m4_ltsm prefix. */
#define fail(...) enil_ltsm_fail(__VA_ARGS__)
#define allocate(...) enil_ltsm_allocate(__VA_ARGS__)
#define wipe(...) enil_ltsm_wipe(__VA_ARGS__)
#define bytes(...) enil_ltsm_bytes(__VA_ARGS__)
#define string(...) enil_ltsm_string(__VA_ARGS__)
#define number(...) enil_ltsm_number(__VA_ARGS__)
#define invoke(...) enil_ltsm_invoke(__VA_ARGS__)
#define bridge_start(...) enil_ltsm_bridge_start(__VA_ARGS__)
#define bridge_end(...) enil_ltsm_bridge_end(__VA_ARGS__)
#define service(...) enil_ltsm_service(__VA_ARGS__)
#define legy_service(...) enil_ltsm_legy_service(__VA_ARGS__)
#define valid_utf8(...) enil_ltsm_valid_utf8(__VA_ARGS__)
#define json_check_text(...) enil_ltsm_json_check_text(__VA_ARGS__)
#define json_string(...) enil_ltsm_json_string(__VA_ARGS__)
#define json_uint(...) enil_ltsm_json_uint(__VA_ARGS__)
#define unbase64(...) enil_ltsm_unbase64(__VA_ARGS__)
#define base64(...) enil_ltsm_base64(__VA_ARGS__)
#define json_add(...) enil_ltsm_json_add(__VA_ARGS__)

void fail(Context *c, const char *message) __attribute__((noreturn));
void *allocate(Context *c, size_t length);
void wipe(void *bytes, size_t length);
Value *bytes(Context *c, const void *data, size_t length);
Value *string(Context *c, const char *text);
Value *number(Context *c, U64 n);
Value *invoke(Context *c, const char *class_name, const char *method_name,
              Value *self, size_t count, ...);
void enil_ltsm_checkpoint(wasmModuleInstance *instance);
void enil_ltsm_allocate_memory(m4_ltsmInstance *instance);
void enil_ltsm_allocate_table(m4_ltsmInstance *instance);
void bridge_start(Context *c);
void bridge_end(Context *c);
void service(Context *c, const char *operation);
void legy_service(Context *c, const char *operation);
int valid_utf8(const unsigned char *s, size_t n);
void json_check_text(Context *c, const char *text);
const char *json_string(Context *c, const cJSON *object, const char *key);
U64 json_uint(Context *c, const cJSON *object, const char *key);
Value *unbase64(Context *c, const char *text);
char *base64(Context *c, const Value *value);
void json_add(Context *c, cJSON *object, const char *key, cJSON *value);
#endif
