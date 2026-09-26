#ifndef ENIL_THRIFT_H
#define ENIL_THRIFT_H
#include "cJSON.h"
#include "enil_http.h"
/* Named JSON / Compact Thrift adapter. i64 values stay decimal strings;
 * binary fields use base64. Unknown response fields are skipped safely. */
int enil_thrift_encode(const char *method, cJSON *args, ENILBuf *out);
cJSON *enil_thrift_decode(const char *method, const void *data, size_t size);
/* QR recovery retains numeric field IDs and accepts Thrift application
 * exceptions. Uses the same bounded decoder as ordinary named RPC replies. */
cJSON *enil_thrift_decode_raw(const char *method, const void *data, size_t size,
                              int *exception);
#endif
