/* Minimal host ABI for the pinned Emscripten LTSM module. This is not a
 * JavaScript interpreter or a general-purpose Emscripten runtime. Unsupported
 * imports/types/properties fail closed. All WASM pointers are 32-bit offsets;
 * accesses below are bounds-checked and explicitly little endian, including on
 * PPC. Objects and host values live only for one request, even after a trap. */
#include "enil_ltsm.h"
#include "ltsm_internal.h"
#include "identity.h"
#include <limits.h>
#include <errno.h>
#include <sys/select.h>
#include <openssl/rand.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Allocation { Allocation *next; size_t length; };
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
/* Protected by lock. w2c2's trap ABI has no instance argument. */
static Context *active;

void wipe(void *p, size_t n) {
  volatile unsigned char *v = (volatile unsigned char *)p;
  while (n--) *v++ = 0;
}
void fail(Context *c, const char *message) {
  c->error = message;
  longjmp(c->escape, 1);
}
void trap(Trap reason) {
  (void)reason;
  if (active) fail(active, "LTSM trapped; request discarded");
  abort();
}
void *allocate(Context *c, size_t n) {
  Allocation *p;
  if (n > LTSM_MAX_HOST || c->allocated > LTSM_MAX_HOST - n ||
      sizeof(*p) > LTSM_MAX_HOST - n - c->allocated)
    fail(c, "LTSM host allocation limit");
  p = (Allocation *)calloc(1, sizeof(*p) + n);
  if (!p) fail(c, "LTSM allocation failed");
  p->next = c->allocations; p->length = n;
  c->allocations = p; c->allocated += sizeof(*p) + n;
  return p + 1;
}
static unsigned char *memory(Context *c, U32 p, size_t n) {
  wasmMemory *m = c->wasm.m0;
  if (!m || p > m->size || n > m->size - p)
    fail(c, "invalid LTSM memory range");
  return m->data + p;
}
static U32 read32(Context *c, U32 p) {
  const unsigned char *b = memory(c, p, 4);
  return (U32)b[0] | (U32)b[1]<<8 | (U32)b[2]<<16 | (U32)b[3]<<24;
}
static void write32(Context *c, U32 p, U32 value) {
  unsigned char *b = memory(c, p, 4);
  b[0]=(U8)value; b[1]=(U8)(value>>8);
  b[2]=(U8)(value>>16); b[3]=(U8)(value>>24);
}
static const char *cstring(Context *c, U32 p) {
  const char *s = (const char *)memory(c, p, 1);
  if (!memchr(s, 0, c->wasm.m0->size - p)) fail(c, "unterminated LTSM string");
  return s;
}
static U32 wasm_alloc(Context *c, size_t n) {
  U32 p;
  if (n > LTSM_MAX_INPUT + 32U) fail(c, "LTSM input too large");
  p = m4_ltsmExport1_V(&c->wasm, (U32)n);
  if (!p) fail(c, "LTSM linear memory exhausted");
  memory(c, p, n);
  return p;
}
static Value *value(Context *c, ValueKind kind) {
  Value *v = (Value *)allocate(c, sizeof(*v)); v->kind = kind; return v;
}
Value *bytes(Context *c, const void *data, size_t n) {
  Value *v = value(c, V_BYTES);
  if (n > LTSM_MAX_INPUT) fail(c, "LTSM byte array too large");
  v->length = n; v->data = allocate(c, n + 1);
  if (n && data) memcpy(v->data, data, n);
  return v;
}
Value *string(Context *c, const char *s) {
  Value *v = bytes(c, s, strlen(s)); v->kind = V_STRING; return v;
}
Value *number(Context *c, U64 n) {
  Value *v = n < 256 ? &c->small_numbers[n] : value(c, V_NUMBER);
  v->kind=V_NUMBER; v->number=n; return v;
}
static Type *type(Context *c, U32 id) {
  size_t i;
  for (i=0; i<c->type_count; ++i) if (c->types[i].id == id) return &c->types[i];
  fail(c, "unregistered LTSM type");
}
static Type *register_type(Context *c, U32 id, TypeKind kind, U32 name) {
  Type *t; size_t i;
  for (i=0; i<c->type_count; ++i) if (c->types[i].id == id) return &c->types[i];
  if (c->type_count == LTSM_MAX_TYPES) fail(c, "too many LTSM types");
  t = &c->types[c->type_count++]; t->id=id; t->kind=kind;
  t->name = name ? cstring(c, name) : "";
  return t;
}
static U32 handle(Context *c, Value *v) {
  U32 i;
  for (i=5; i<LTSM_MAX_HANDLES; ++i) if (!c->handles[i].references) {
    c->handles[i].value=v; c->handles[i].references=1; return i;
  }
  fail(c, "LTSM handle limit");
}
static Value *get_handle(Context *c, U32 h) {
  if (h >= LTSM_MAX_HANDLES || !c->handles[h].references)
    fail(c, "invalid LTSM value handle");
  return c->handles[h].value;
}
static void drop_handle(Context *c, U32 h) {
  get_handle(c,h);
  if (h > 4 && --c->handles[h].references == 0) c->handles[h].value=NULL;
}
#include "invoke.inc"
static U32 cast_object(Context *c, Value *v, U32 owner) {
  Type *t; U32 p; Wire a;
  if (!v || v->kind != V_OBJECT) fail(c,"expected LTSM object");
  t=type(c,v->type); p=v->pointer;
  while (t->id != owner) {
    if (!t->base || !t->upcast) fail(c,"incompatible LTSM object");
    a.i=p; p=call_wasm(c,t->upcast,&a,1).i; t=type(c,t->base);
  }
  return p;
}
static Wire to_wire(Context *c, Type *t, Value *v) {
  Wire w; memset(&w,0,sizeof(w));
  if (!v) fail(c,"missing LTSM argument");
  switch (t->kind) {
  case T_VAL: w.i=handle(c,v); break;
  case T_CLASS: w.i=cast_object(c,v,t->owner); break;
  case T_INT: if(v->kind!=V_NUMBER) fail(c,"expected integer"); w.i=(U32)v->number; break;
  case T_BIGINT: if(v->kind!=V_NUMBER) fail(c,"expected uint64"); w.j=v->number; break;
  case T_STRING:
    if(v->kind!=V_STRING && v->kind!=V_BYTES) fail(c,"expected string");
    w.i=wasm_alloc(c,v->length+5); write32(c,w.i,(U32)v->length);
    memcpy(memory(c,w.i+4,v->length+1),v->data,v->length+1); break;
  default: fail(c,"unsupported LTSM argument type");
  }
  return w;
}
static Value *from_wire(Context *c, Type *t, Wire w) {
  Value *v;
  switch(t->kind) {
  case T_VOID: return value(c,V_UNDEFINED);
  case T_INT: return number(c,w.i);
  case T_BIGINT: return number(c,w.j);
  case T_VAL: v=get_handle(c,w.i); drop_handle(c,w.i); return v;
  case T_CLASS:
    if(!w.i) fail(c,"null LTSM object");
    memory(c,w.i,1); v=value(c,V_OBJECT); v->type=t->owner; v->pointer=w.i; return v;
  case T_STRING: {
    U32 n=read32(c,w.i); v=bytes(c,memory(c,w.i+4,n),n); v->kind=V_STRING;
    m4_ltsmExport1_R(&c->wasm,w.i); return v;
  }
  case T_VIEW:
    if(t->view!=1 && t->view!=0) fail(c,"unsupported LTSM memory view");
    return bytes(c,memory(c,read32(c,w.i+4),read32(c,w.i)),read32(c,w.i));
  default: fail(c,"unsupported LTSM result type");
  }
}
static Value *read_value(Context *c, U32 tid, U32 p) {
  Type *t=type(c,tid); Wire w; size_t i;
  memset(&w,0,sizeof(w));
  if(t->kind==T_VIEW) w.i=p;
  else if(t->kind==T_BIGINT) {
    w.j=(U64)read32(c,p)|((U64)read32(c,p+4)<<32);
  } else if(t->kind==T_INT) {
    const U8 *b=memory(c,p,t->size);
    for(i=0;i<t->size;++i) w.i|=(U32)b[i]<<(i*8);
  } else w.i=read32(c,p);
  if(t->kind==T_VAL) return get_handle(c,w.i); /* read does not consume */
  return from_wire(c,t,w);
}
Value *invoke(Context *c,const char *cls,const char *name,Value *self,size_t n,...) {
  Type *owner=NULL; Method *m=NULL; size_t i,j; Wire a[16],result;
  Value *arguments[16]; va_list ap;
  for(i=0;i<c->type_count;++i) if(c->types[i].kind==T_CLASS &&
      c->types[i].id==c->types[i].owner && !strcmp(c->types[i].name,cls)) owner=&c->types[i];
  if(!owner) fail(c,"unknown LTSM class");
  for(;;) {
    for(i=0;i<c->method_count;++i) if(c->methods[i].owner==owner->id &&
       !strcmp(c->methods[i].name,name)) { m=&c->methods[i]; break; }
    if(m || !owner->base) break;
    owner=type(c,owner->base);
  }
  if(!m || m->count != n+1+(self?1:0) || m->instance != (self!=NULL))
    fail(c,"unknown LTSM method or arity");
  memset(a,0,sizeof(a)); a[0].i=m->target; j=1;
  if(self) { a[j]=to_wire(c,type(c,m->types[j]),self); ++j; }
  va_start(ap,n);
  for(i=0;i<n;++i) arguments[i]=va_arg(ap,Value*);
  va_end(ap);
  for(i=0;i<n;++i) { a[j]=to_wire(c,type(c,m->types[j]),arguments[i]); ++j; }
  result=call_wasm(c,m->invoker,a,j);
  return from_wire(c,type(c,m->types[0]),result);
}
static void method(Context *c,U32 owner,U32 name,U32 count,U32 types,
                   U32 invoker,U32 target,int instance) {
  Method *m; size_t i;
  if(c->method_count==LTSM_MAX_METHODS || count>16 || !count)
    fail(c,"invalid LTSM method registration");
  m=&c->methods[c->method_count++]; m->owner=owner;
  m->name=name?cstring(c,name):"constructor"; m->count=count;
  m->invoker=invoker; m->target=target; m->instance=instance;
  for(i=0;i<count;++i) m->types[i]=read32(c,types+(U32)i*4);
}
#define CTX Context *c=(Context *)instance
/* Emscripten imports. The minified names are pinned by the WASM hash. */
void m4_ltsmImport1_a1_i(wasmModuleInstance *instance,U32 id,U32 ptr,U32 constant,U32 base,
            U32 sig,U32 gettype,U32 upsig,U32 up,U32 downsig,U32 down,
            U32 name,U32 dsig,U32 destructor) {
  Type *t; CTX;
  t=register_type(c,id,T_CLASS,name); t->owner=id; t->base=base; t->upcast=up;
  t=register_type(c,ptr,T_CLASS,name); t->owner=id;
  t=register_type(c,constant,T_CLASS,name); t->owner=id;
}
void m4_ltsmImport1_a1_b(wasmModuleInstance *instance,U32 owner,U32 name,U32 n,U32 types,U32 sig,U32 inv,U32 target,U32 pure) {
  CTX; method(c,owner,name,n,types,inv,target,1);
}
void m4_ltsmImport1_a1_g(wasmModuleInstance *instance,U32 owner,U32 name,U32 n,U32 types,U32 sig,U32 inv,U32 target) {
  CTX; method(c,owner,name,n,types,inv,target,0);
}
void m4_ltsmImport1_a1_k(wasmModuleInstance *instance,U32 owner,U32 n,U32 types,U32 sig,U32 inv,U32 target) {
  CTX; method(c,owner,0,n,types,inv,target,0);
}
void m4_ltsmImport1_a1_N(wasmModuleInstance *instance,U32 name,U32 n,U32 types,U32 sig,U32 inv,U32 target) {
  CTX; method(c,0,name,n,types,inv,target,0);
}
void m4_ltsmImport1_a1_c(wasmModuleInstance *instance,U32 name,U32 tid,F64 v) { /* named constants; unused by host */ }
void m4_ltsmImport1_a1_h(wasmModuleInstance *instance,U32 id,U32 name,U32 size,U32 min,U32 max) {
  CTX; if(size!=1 && size!=2 && size!=4) fail(c,"invalid integer type");
  register_type(c,id,T_INT,name)->size=size;
}
void m4_ltsmImport1_a1_J(wasmModuleInstance *instance,U32 id,U32 name,U32 size,U32 yes,U32 no) {
  CTX; if(size!=1 || yes!=1 || no!=0) fail(c,"unsupported boolean ABI");
  register_type(c,id,T_INT,name)->size=size;
}
void m4_ltsmImport1_a1_v(wasmModuleInstance *instance,U32 id,U32 name,U32 size,U64 min,U64 max) {
  CTX; register_type(c,id,T_BIGINT,name)->size=size;
}
void m4_ltsmImport1_a1_u(wasmModuleInstance *instance,U32 id,U32 name,U32 size) {
  CTX; register_type(c,id,T_FLOAT,name)->size=size;
}
void m4_ltsmImport1_a1_t(wasmModuleInstance *instance,U32 id,U32 name) {
  CTX; register_type(c,id,T_STRING,name);
}
void m4_ltsmImport1_a1_n(wasmModuleInstance *instance,U32 id,U32 size,U32 name) {
  CTX; register_type(c,id,T_WSTRING,name)->size=size;
}
void m4_ltsmImport1_a1_I(wasmModuleInstance *instance,U32 id,U32 name) {
  CTX; register_type(c,id,T_VAL,name);
}
void m4_ltsmImport1_a1_K(wasmModuleInstance *instance,U32 id,U32 name) {
  CTX; register_type(c,id,T_VOID,name);
}
void m4_ltsmImport1_a1_d(wasmModuleInstance *instance,U32 id,U32 kind,U32 name) {
  CTX; register_type(c,id,T_VIEW,name)->view=kind;
}
void m4_ltsmImport1_a1_a(wasmModuleInstance *instance,U32 h) { CTX; drop_handle(c,h); }
void m4_ltsmImport1_a1_j(wasmModuleInstance *instance,U32 h) {
  CTX; get_handle(c,h);
  if(h>4 && ++c->handles[h].references==0) fail(c,"LTSM reference overflow");
}
U32 m4_ltsmImport1_a1_f(wasmModuleInstance *instance,U32 name) {
  CTX; if(strcmp(cstring(c,name),"Uint8Array")) fail(c,"unsupported LTSM global");
  return handle(c,value(c,V_UINT8_CONSTRUCTOR));
}
U32 m4_ltsmImport1_a1_M(wasmModuleInstance *instance,U32 name) { CTX; return handle(c,string(c,cstring(c,name))); }
U32 m4_ltsmImport1_a1_A(wasmModuleInstance *instance,U32 object,U32 property) {
  Value *v,*p; CTX; v=get_handle(c,object); p=get_handle(c,property);
  if(p->kind==V_STRING && !strcmp((char *)p->data,"length") &&
     (v->kind==V_BYTES || v->kind==V_ARRAY || v->kind==V_STRING))
    return handle(c,number(c,v->length));
  if(p->kind!=V_NUMBER || p->number>=v->length) fail(c,"invalid LTSM property");
  if(v->kind==V_BYTES) return handle(c,number(c,v->data[p->number]));
  if(v->kind==V_ARRAY) return handle(c,v->items[p->number]);
  fail(c,"unsupported LTSM property access");
}
U32 m4_ltsmImport1_a1_e(wasmModuleInstance *instance,U32 ctor,U32 argc,U32 types,U32 argv) {
  Value *arg,*v; CTX;
  if(get_handle(c,ctor)->kind!=V_UINT8_CONSTRUCTOR || argc!=1)
    fail(c,"unsupported LTSM host constructor");
  arg=read_value(c,read32(c,types),argv);
  if(arg->kind==V_NUMBER) v=bytes(c,NULL,(size_t)arg->number);
  else if(arg->kind==V_BYTES) v=bytes(c,arg->data,arg->length);
  else fail(c,"invalid Uint8Array input");
  return handle(c,v);
}
U32 m4_ltsmImport1_a1_x(wasmModuleInstance *instance) { CTX; return handle(c,value(c,V_ARRAY)); }
void m4_ltsmImport1_a1_L(wasmModuleInstance *instance,U32 object,U32 key,U32 val) {
  Value *v,*p; size_t n; Value **items; CTX;
  v=get_handle(c,object); p=get_handle(c,key);
  if(p->kind!=V_NUMBER || p->number>=LTSM_MAX_HANDLES || v->kind!=V_ARRAY)
    fail(c,"unsupported LTSM property write");
  n=(size_t)p->number;
  if(n>=v->length) {
    items=allocate(c,(n+1)*sizeof(*items));
    if(v->length) memcpy(items,v->items,v->length*sizeof(*items));
    v->items=items; v->length=n+1;
  }
  v->items[n]=get_handle(c,val);
}
U32 m4_ltsmImport1_a1_l(wasmModuleInstance *instance,U32 tid,U32 p) { CTX; return handle(c,read_value(c,tid,p)); }
F64 m4_ltsmImport1_a1_z(wasmModuleInstance *instance,U32 h,U32 tid,U32 destructors) {
  Wire w; Type *t; CTX; t=type(c,tid);
  /* These conversions own no host-side destructors. Avoid allocating an
   * empty array for every byte crossing emval; the request owns WASM storage. */
  c->empty_destructors.kind=V_ARRAY;
  write32(c,destructors,handle(c,&c->empty_destructors));
  w=to_wire(c,t,get_handle(c,h));
  if(t->kind!=T_INT && t->kind!=T_VAL && t->kind!=T_CLASS && t->kind!=T_STRING)
    fail(c,"unsupported LTSM value conversion");
  return (F64)w.i;
}
void m4_ltsmImport1_a1_y(wasmModuleInstance *instance,U32 h) { CTX; drop_handle(c,h); }
U32 m4_ltsmImport1_a1_p(wasmModuleInstance *instance,U32 size) { CTX; return wasm_alloc(c,(size_t)size+24)+24; }
void m4_ltsmImport1_a1_B(wasmModuleInstance *instance,U32 p,U32 t,U32 destructor) {
  CTX; fail(c,"LTSM rejected input");
}
void m4_ltsmImport1_a1_m(wasmModuleInstance *instance) { CTX; fail(c,"LTSM aborted"); }
void m4_ltsmImport1_a1_H(wasmModuleInstance *instance,U32 dest,U32 src,U32 n) {
  CTX; memmove(memory(c,dest,n),memory(c,src,n),n);
}
U32 m4_ltsmImport1_a1_C(wasmModuleInstance *instance,U32 requested) { return 0; /* fixed memory */ }
U32 m4_ltsmImport1_a1_w(wasmModuleInstance *instance) {
  U32 p; size_t n=sizeof(ENIL_LTSM_ORIGIN); CTX;
  p=wasm_alloc(c,n); memcpy(memory(c,p,n),ENIL_LTSM_ORIGIN,n); return p;
}
static void random_bytes(Context *c,U32 p,U32 n) {
  if(n>INT_MAX || RAND_bytes(memory(c,p,n),(int)n)!=1) fail(c,"secure randomness unavailable");
}
U32 m4_ltsmImport1_a1_o(wasmModuleInstance *instance,U32 code,U32 signature,U32 args) {
  CTX;
  switch(code) {
  case 1456932: fail(c,"LTSM rejected input");
  case 1456985: return RAND_status()==1?0:1;
  case 1457725: random_bytes(c,read32(c,args),read32(c,args+4)); return 0;
  default: fail(c,"unsupported LTSM host expression");
  }
}
U32 m4_ltsmImport1_a1_G(wasmModuleInstance *instance,U32 dir,U32 path,U32 flags,U32 mode) {
  CTX; if(strcmp(cstring(c,path),"/dev/urandom")) fail(c,"LTSM filesystem access denied");
  return 3; /* Virtual entropy descriptor; never opens a host path. */
}
U32 m4_ltsmImport1_a1_E(wasmModuleInstance *instance,U32 fd,U32 iov,U32 count,U32 result) {
  U32 i,total=0,n,p; CTX;
  if(fd!=3 || count>1024) fail(c,"invalid LTSM read");
  memory(c,iov,(size_t)count*8);
  for(i=0;i<count;++i) {
    p=read32(c,iov+i*8); n=read32(c,iov+i*8+4);
    if(n>UINT_MAX-total) fail(c,"LTSM read overflow");
    random_bytes(c,p,n); total+=n;
  }
  write32(c,result,total); return 0;
}
U32 m4_ltsmImport1_a1_q(wasmModuleInstance *instance,U32 fd) { CTX; if(fd!=3) fail(c,"invalid LTSM close"); return 0; }
U32 m4_ltsmImport1_a1_s(wasmModuleInstance *instance,U32 fd,U32 cmd,U32 args) { return (U32)-28; }
U32 m4_ltsmImport1_a1_F(wasmModuleInstance *instance,U32 fd,U32 cmd,U32 args) { return (U32)-59; }
U32 m4_ltsmImport1_a1_D(wasmModuleInstance *instance,U32 fd,U64 offset,U32 whence,U32 result) { return 29; }
U32 m4_ltsmImport1_a1_r(wasmModuleInstance *instance,U32 fd,U32 iov,U32 count,U32 result) {
  CTX; fail(c,"LTSM output denied"); /* Do not leak exception data to logs. */
}
void enil_ltsm_checkpoint(wasmModuleInstance *instance) {
  Context *c=(Context *)instance;
  if((++c->calls & 255U)==0 && c->cancel && c->cancel(c->cancel_context))
    fail(c,"LTSM request cancelled");
}
void enil_ltsm_allocate_memory(m4_ltsmInstance *i) {
  Context *c=(Context *)i;
  i->m0=(wasmMemory *)calloc(1,sizeof(*i->m0));
  if(!i->m0) fail(c,"LTSM allocation failed");
  i->m0->size=16U*1024U*1024U;
  i->m0->pages=i->m0->maxPages=256;
  i->m0->data=(U8 *)calloc((size_t)i->m0->size,1);
  if(!i->m0->data) fail(c,"LTSM allocation failed");
}
void enil_ltsm_allocate_table(m4_ltsmInstance *i) {
  Context *c=(Context *)i;
  i->t0.data=(wasmTableEntry *)calloc(429,sizeof(*i->t0.data));
  if(!i->t0.data) fail(c,"LTSM allocation failed");
  i->t0.size=i->t0.maxSize=429;
}
void bridge_start(Context *c) {
  U32 i; int available=0;
  for(i=1;i<=4;++i) {
    c->handles[i].value = i<3 ? value(c,V_UNDEFINED) : number(c,i==3);
    c->handles[i].references=1;
  }
  m4_ltsmInstantiate(&c->wasm,NULL);
  if(!c->wasm.m0 || !c->wasm.m0->data || !c->wasm.t0.data)
    fail(c,"LTSM initialization failed");
  m4_ltsmExport1_P(&c->wasm);
  for(i=0;i<c->method_count;++i) {
    Method *m=&c->methods[i];
    if(!m->owner && !strcmp(m->name,"isAvailable") && m->count==1) {
      Wire argument; memset(&argument,0,sizeof(argument)); argument.i=m->target;
      available=call_wasm(c,m->invoker,&argument,1).i!=0;
    }
  }
  if(!available) fail(c,"LTSM unavailable");
}
void bridge_end(Context *c) {
  Allocation *p,*next;
  if(c->wasm.m0) {
    if(c->wasm.m0->data) wipe(c->wasm.m0->data,c->wasm.m0->size);
    m4_ltsmFreeInstance(&c->wasm);
  }
  for(p=c->allocations;p;p=next) {
    next=p->next; wipe(p+1,p->length); free(p);
  }
}
static void wipe_json(cJSON *v) {
  cJSON *child;
  if(!v) return;
  for(child=v->child;child;child=child->next) wipe_json(child);
  if(v->valuestring) wipe(v->valuestring,strlen(v->valuestring));
}
int enil_ltsm_request_ex(const char *operation,const char *json,char **output,
                        char *error,size_t error_size,
                        ENILLTSMCancel cancel,void *cancel_context) {
  Context *c; volatile int ok=0; int status;
  if(output) *output=NULL;
  if(error && error_size) error[0]=0;
  if(!output || !operation || !json || strlen(json)>LTSM_MAX_INPUT) {
    if(error && error_size) snprintf(error,error_size,"invalid LTSM request");
    return 0;
  }
  c=(Context *)calloc(1,sizeof(*c));
  if(!c) {
    if(error && error_size) snprintf(error,error_size,"LTSM allocation failed");
    return 0;
  }
  c->cancel=cancel; c->cancel_context=cancel_context;
  for(;;) {
    struct timeval wait;
    if(cancel && cancel(cancel_context)) {
      if(error && error_size) snprintf(error,error_size,"LTSM request cancelled");
      free(c); return 0;
    }
    status=pthread_mutex_trylock(&lock);
    if(!status) break;
    if(status!=EBUSY) { free(c); return 0; }
    wait.tv_sec=0; wait.tv_usec=10000; select(0,NULL,NULL,NULL,&wait);
  }
  active=c;
  if(!setjmp(c->escape)) {
    const char *end=NULL;
    json_check_text(c,json);
    c->request=cJSON_ParseWithOpts(json,&end,1);
    if(!cJSON_IsObject(c->request)) fail(c,"expected JSON object");
    c->response=cJSON_CreateObject();
    if(!c->response) fail(c,"JSON allocation failed");
    if(!strncmp(operation,"/transport/legy/",16)) legy_service(c,operation);
    else { bridge_start(c); service(c,operation); }
    if(cancel && cancel(cancel_context)) fail(c,"LTSM request cancelled");
    *output=cJSON_PrintUnformatted(c->response);
    if(!*output) fail(c,"JSON allocation failed");
    ok=1;
  }
  if(!ok && error && error_size) snprintf(error,error_size,"%s",c->error?c->error:"LTSM failed");
  bridge_end(c); wipe_json(c->request); wipe_json(c->response);
  cJSON_Delete(c->request); cJSON_Delete(c->response);
  active=NULL; pthread_mutex_unlock(&lock);
  wipe(c,sizeof(*c)); free(c); return ok;
}
int enil_ltsm_request(const char *operation,const char *json,char **output,
                     char *error,size_t error_size) {
  return enil_ltsm_request_ex(operation,json,output,error,error_size,NULL,NULL);
}
void enil_ltsm_free(char *s) { if(s) { wipe(s,strlen(s)); free(s); } }
const char *enil_ltsm_version(void) { return "LINE " ENIL_LTSM_VERSION " / w2c2 8b05ef3"; }
