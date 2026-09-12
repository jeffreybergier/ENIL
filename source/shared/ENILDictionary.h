#ifndef ENIL_DICTIONARY_H
#define ENIL_DICTIONARY_H

#import <Foundation/Foundation.h>
#include "enil_api_types.h"

typedef void (*enil_struct_free_fn)(void *);

/* NSDictionary class-cluster subclass backed by a C struct. Overrides the
   three immutable primitives (count, objectForKey:, keyEnumerator). Takes
   ownership of `base` — frees it (and contents via freeFn) on dealloc.
   Immutable: -mutableCopy and the inherited designated init both raise. */
@interface ENILDictionary : NSDictionary {
  void                *base_;
  const enil_field_t  *fields_;
  size_t               fieldsCount_;
  enil_struct_free_fn  freeFn_;
}

- (id)initWithBase:(void *)base
            fields:(const enil_field_t *)fields
             count:(size_t)count
            freeFn:(enil_struct_free_fn)freeFn;

@end

#endif
