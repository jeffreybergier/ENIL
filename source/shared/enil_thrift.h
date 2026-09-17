#ifndef ENIL_THRIFT_H
#define ENIL_THRIFT_H
#include "cJSON.h"
#include "enil_http.h"
/* Named JSON / Compact Thrift adapter. i64 values stay decimal strings;
 * binary fields use base64. Unknown response fields are skipped safely. */
int enil_thrift_encode(const char *method, cJSON *args, ENILBuf *out);
cJSON *enil_thrift_decode(const char *method, const void *data, size_t size);
#endif
