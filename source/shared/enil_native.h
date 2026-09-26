#ifndef ENIL_NATIVE_H
#define ENIL_NATIVE_H
#include "enil_line.h"
ENILLineResponse enil_native_post(const char *path, const char *body, const char *token,
                                  long timeout_ms, const volatile int *cancel);
#endif
