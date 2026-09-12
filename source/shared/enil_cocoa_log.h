#ifndef ENIL_COCOA_LOG_H
#define ENIL_COCOA_LOG_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Route a printf-style log line through NSLog with a "[<tag>] " prefix.
 * Safe to call from any thread. `tag` should follow the
 * "ModuleName.functionName" convention (matches the project's existing
 * namespaced-logging rule). `fmt` and the variadic args use the standard
 * printf conversion specifiers.
 *
 * Implementation lives in enil_cocoa.m (the single Apple-frameworks bridge —
 * see CLAUDE.md). C callers should prefer the ENIL_LOG macro below. ObjC
 * callers already have NSLog and should use the ENILLog macro in
 * XPFoundation.h instead — this header is only the C↔NSLog bridge. */
void enil_log_v(const char *tag, const char *fmt, va_list ap);
void enil_log  (const char *tag, const char *fmt, ...)
  __attribute__((format(printf, 2, 3)));

/* C convenience wrapper that matches the call sites' rhythm. */
#define ENIL_LOG(tag, ...) enil_log((tag), __VA_ARGS__)

#ifdef __cplusplus
}
#endif

/* The Objective-C convenience macro ENILLog lives in XPFoundation.h, not here:
 * it is a pure NSLog passthrough with no enil_cocoa.m implementation, so it
 * does not belong in a C↔NSLog bridge header that .c files include. */

#endif /* ENIL_COCOA_LOG_H */
