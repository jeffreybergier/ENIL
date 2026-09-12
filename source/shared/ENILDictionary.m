#import "ENILDictionary.h"
#include <string.h>

@implementation ENILDictionary

- (id)initWithObjects:(const id *)objects
              forKeys:(const id <NSCopying> *)keys
                count:(NSUInteger)cnt;
{
  (void)objects; (void)keys; (void)cnt;
  [NSException raise:NSInternalInconsistencyException
              format:@"[ENILDictionary] use -initWithBase:fields:count:freeFn:"];
  [self release];
  return nil;
}

- (id)mutableCopyWithZone:(NSZone *)zone;
{
  (void)zone;
  [NSException raise:NSInternalInconsistencyException
              format:@"[ENILDictionary] is immutable; copy keys manually if needed"];
  return nil;
}

- (id)initWithBase:(void *)base
            fields:(const enil_field_t *)fields
             count:(size_t)count
            freeFn:(enil_struct_free_fn)freeFn;
{
  if (!base || !fields) {
    [NSException raise:NSInvalidArgumentException
                format:@"[ENILDictionary.init] base and fields required"];
  }
  if ((self = [super init])) {
    base_        = base;
    fields_      = fields;
    fieldsCount_ = count;
    freeFn_      = freeFn;
  }
  return self;
}

- (void)dealloc;
{
  if (freeFn_ && base_) freeFn_(base_);
  free(base_);
  [super dealloc];
}

- (const enil_field_t *)fieldForKey:(NSString *)key;
{
  const char *k;
  size_t i;
  if (!key || ![key isKindOfClass:[NSString class]]) return NULL;
  k = [key UTF8String];
  if (!k) return NULL;
  for (i = 0; i < fieldsCount_; i++) {
    if (strcmp(fields_[i].key, k) == 0) return &fields_[i];
  }
  return NULL;
}

- (id)objectForKey:(id)aKey;
{
  const enil_field_t *f;
  const void *p;
  if (!aKey || ![aKey isKindOfClass:[NSString class]]) return nil;
  f = [self fieldForKey:aKey];
  if (!f) return nil;
  p = (const char *)base_ + f->offset;
  switch (f->type) {
    case ENIL_F_STR: {
      const char *s = *(const char *const *)p;
      return s ? [NSString stringWithUTF8String:s] : nil;
    }
    case ENIL_F_INT:
      return [NSNumber numberWithInt:*(const int *)p];
    case ENIL_F_BOOL:
      return [NSNumber numberWithBool:(*(const int *)p ? YES : NO)];
    case ENIL_F_I64:
      return [NSNumber numberWithLongLong:*(const long long *)p];
    case ENIL_F_JSON: {
      cJSON *j = *(cJSON *const *)p;
      char *s;
      NSString *out;
      if (!j) return nil;
      s = cJSON_PrintUnformatted(j);
      if (!s) return nil;
      out = [NSString stringWithUTF8String:s];
      free(s);
      return out;
    }
  }
  return nil;
}

- (NSUInteger)count;
{
  return fieldsCount_;
}

- (NSEnumerator *)keyEnumerator;
{
  NSMutableArray *keys = [NSMutableArray arrayWithCapacity:fieldsCount_];
  size_t i;
  for (i = 0; i < fieldsCount_; i++) {
    [keys addObject:[NSString stringWithUTF8String:fields_[i].key]];
  }
  return [keys objectEnumerator];
}

@end
