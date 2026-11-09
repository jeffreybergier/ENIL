/* Native implementation of source/cloudflare/src/legy.js. LEGY's legacy
 * xxHash-based checksum is protocol framing, not a modern authentication MAC.
 * These functions never contact LINE or expose output before full validation. */
#include "ltsm_internal.h"
#include <limits.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char kIV[16]={78,9,72,62,56,245,255,114,128,18,123,158,251,92,45,51};
static const char kPublicKey[]=
"-----BEGIN PUBLIC KEY-----\n"
"MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAsMC6HAYeMq4R59e2yRw6\n"
"W1OWT2t9aepiAp4fbSCXzRj7A29BOAFAvKlzAub4oxN13Nt8dbcB+ICAufyDnN5N\n"
"d3+vXgDxEXZ/sx2/wuFbC3B3evSNKR4hKcs80suRs8aL6EeWi+bAU2oYIc78Bbqh\n"
"Nzx0WCzZSJbMBFw1VlsU/HQ/XdiUufopl5QSa0S246XXmwJmmXRO0v7bNvrxaNV0\n"
"cbviGkOvTlBt1+RerIFHMTw3SwLDnCOolTz3CuE5V2OrPZCmC0nlmPRzwUfxoxxs\n"
"/6qFdpZNoORH/s5mQenSyqPkmH8TBOlHJWPH3eN1k6aZIlK5S54mcUb/oNRRq9wD\n"
"1wIDAQAB\n-----END PUBLIC KEY-----\n";
static U32 little32(const unsigned char *p) {
  return (U32)p[0]|(U32)p[1]<<8|(U32)p[2]<<16|(U32)p[3]<<24;
}
static U32 rotate(U32 n,unsigned shift) { return n<<shift|n>>(32-shift); }
static U32 round_hash(U32 v,U32 x) { return rotate(v+x*0x85ebca77U,13)*0x9e3779b1U; }
static U32 xxhash(const unsigned char *p,size_t n) {
  size_t i=0; U32 h;
  if(n>=16) {
    U32 a=0x9e3779b1U+0x85ebca77U,b=0x85ebca77U,d=0U-0x9e3779b1U,e=0;
    while(n-i>=16) {
      a=round_hash(a,little32(p+i)); b=round_hash(b,little32(p+i+4));
      e=round_hash(e,little32(p+i+8)); d=round_hash(d,little32(p+i+12)); i+=16;
    }
    h=rotate(a,1)+rotate(b,7)+rotate(e,12)+rotate(d,18);
  } else h=0x165667b1U;
  h+=(U32)n;
  while(n-i>=4) { h=rotate(h+little32(p+i)*0xc2b2ae3dU,17)*0x27d4eb2fU; i+=4; }
  while(i<n) h=rotate(h+p[i++]*0x165667b1U,11)*0x9e3779b1U;
  h^=h>>15; h*=0x85ebca77U; h^=h>>13; h*=0xc2b2ae3dU; return h^(h>>16);
}
static void big32(unsigned char *p,U32 n) {
  p[0]=(U8)(n>>24); p[1]=(U8)(n>>16); p[2]=(U8)(n>>8); p[3]=(U8)n;
}
static void mac(Context *c,const unsigned char *key,const unsigned char *data,size_t n,unsigned char out[4]) {
  unsigned char *inner=allocate(c,n+16),outer[20]; size_t i;
  for(i=0;i<16;++i) { inner[i]=key[i]^0x36; outer[i]=key[i]^0x5c; }
  memcpy(inner+16,data,n); big32(outer+16,xxhash(inner,n+16));
  big32(out,xxhash(outer,20)); wipe(outer,sizeof(outer));
}
static Value *aes(Context *c,int encrypt,Value *key,Value *data) {
  EVP_CIPHER_CTX *ctx; Value *out; int n=0,tail=0,ok;
  if(key->length!=16 || data->length>INT_MAX-32) fail(c,"invalid LEGY AES input");
  out=bytes(c,NULL,data->length+32); ctx=EVP_CIPHER_CTX_new();
  if(!ctx) fail(c,"AES allocation failed");
  ok=EVP_CipherInit_ex(ctx,EVP_aes_128_cbc(),NULL,key->data,kIV,encrypt)==1 &&
     EVP_CipherUpdate(ctx,out->data,&n,data->data,(int)data->length)==1 &&
     EVP_CipherFinal_ex(ctx,out->data+n,&tail)==1;
  EVP_CIPHER_CTX_free(ctx);
  if(!ok) fail(c,"invalid LEGY ciphertext or padding");
  out->length=(size_t)n+(size_t)tail; return out;
}
static void word(unsigned char **p,size_t n) {
  *(*p)++=(U8)(n>>8); *(*p)++=(U8)n;
}
static void text(unsigned char **p,const char *s) {
  size_t n=strlen(s); word(p,n); memcpy(*p,s,n); *p+=n;
}
static const char *access_token(const char *s) {
  const char *colon=strchr(s,':'),*p; unsigned dots=0,part=0;
  if(!colon || colon==s) return s;
  for(p=colon+1;*p;++p) {
    if(*p=='.') { if(!part) return s; part=0; ++dots; }
    else if((*p>='A'&&*p<='Z') || (*p>='a'&&*p<='z') ||
            (*p>='0'&&*p<='9') || *p=='_' || *p=='-') ++part;
    else return s;
  }
  return dots==2 && part?colon+1:s;
}
static void encode(Context *c) {
  const char *path=json_string(c,c->request,"path"),*token="",*s;
  const cJSON *t=cJSON_GetObjectItemCaseSensitive(c->request,"accessToken");
  const cJSON *body=cJSON_GetObjectItemCaseSensitive(c->request,"body");
  Value *raw,*key,*plain,*encrypted,*wrapped; size_t headers,length=256;
  unsigned char *p; char *x; BIO *bio; EVP_PKEY *rsa; EVP_PKEY_CTX *ctx; int ok;
  if(t) { if(!cJSON_IsString(t)) fail(c,"invalid LEGY token"); token=t->valuestring; }
  if(path[0]!='/' || !path[1] || strpbrk(token,"\r\n")) fail(c,"invalid LEGY request");
  for(s=path;*s;++s) if(!((*s>='A'&&*s<='Z')||(*s>='a'&&*s<='z')||(*s>='0'&&*s<='9')||*s=='/'))
    fail(c,"invalid LEGY path");
  token=access_token(token);
  if(!cJSON_IsString(body)) fail(c,"invalid LEGY body");
  raw=unbase64(c,body->valuestring); key=bytes(c,NULL,16);
  if(RAND_bytes(key->data,16)!=1) fail(c,"secure randomness unavailable");
  headers=2+4+6+strlen(path)+(*token?4+4+strlen(token):0);
  if(headers>65535 || raw->length>LTSM_MAX_INPUT-headers-40) fail(c,"LEGY request too large");
  plain=bytes(c,NULL,1+2+headers+raw->length); p=plain->data; *p++=7;
  word(&p,headers); word(&p,*token?2:1); text(&p,"x-lpqs"); text(&p,path);
  if(*token) { text(&p,"x-lt"); text(&p,token); }
  memcpy(p,raw->data,raw->length);
  encrypted=aes(c,1,key,plain);
  mac(c,key->data,encrypted->data,encrypted->length,encrypted->data+encrypted->length);
  encrypted->length+=4;
  wrapped=bytes(c,NULL,256);
  bio=BIO_new_mem_buf(kPublicKey,(int)strlen(kPublicKey));
  if(!bio) fail(c,"RSA allocation failed");
  rsa=PEM_read_bio_PUBKEY(bio,NULL,NULL,NULL); BIO_free(bio);
  if(!rsa) fail(c,"invalid LEGY public key");
  ctx=EVP_PKEY_CTX_new(rsa,NULL);
  ok=ctx && EVP_PKEY_encrypt_init(ctx)>0 &&
    EVP_PKEY_CTX_set_rsa_padding(ctx,RSA_PKCS1_OAEP_PADDING)>0 &&
    EVP_PKEY_CTX_set_rsa_oaep_md(ctx,EVP_sha1())>0 &&
    EVP_PKEY_CTX_set_rsa_mgf1_md(ctx,EVP_sha1())>0 &&
    EVP_PKEY_encrypt(ctx,wrapped->data,&length,key->data,key->length)>0;
  EVP_PKEY_CTX_free(ctx); EVP_PKEY_free(rsa);
  if(!ok || length!=256) fail(c,"LEGY RSA encryption failed");
  s=base64(c,wrapped); x=allocate(c,strlen(s)+5); memcpy(x,"0008",4); strcpy(x+4,s);
  json_add(c,c->response,"body",cJSON_CreateString(base64(c,encrypted)));
  json_add(c,c->response,"key",cJSON_CreateString(base64(c,key)));
  json_add(c,c->response,"xLcs",cJSON_CreateString(x));
}
static unsigned read_word(Context *c,const unsigned char **p,const unsigned char *end) {
  unsigned n;
  if(end-*p<2) fail(c,"truncated LEGY headers");
  n=(unsigned)(*p)[0]*256+(*p)[1]; *p+=2; return n;
}
static char *read_text(Context *c,const unsigned char **p,const unsigned char *end) {
  unsigned n=read_word(c,p,end); char *s;
  if((size_t)(end-*p)<n || !valid_utf8(*p,n)) fail(c,"invalid LEGY header");
  s=allocate(c,n+1); memcpy(s,*p,n); *p+=n; return s;
}
static void decode(Context *c) {
  Value *key=unbase64(c,json_string(c,c->request,"key"));
  Value *raw=unbase64(c,json_string(c,c->request,"body")),*plain;
  unsigned char sum[4]; unsigned diff=0,i,count; size_t length;
  const unsigned char *p,*end; cJSON *headers; const cJSON *status;
  char *name,*val,*tail; long code=200;
  if(key->length!=16 || raw->length<16) fail(c,"invalid LEGY response");
  if(raw->length%16==4) {
    raw->length-=4; mac(c,key->data,raw->data,raw->length,sum);
    for(i=0;i<4;++i) diff|=sum[i]^raw->data[raw->length+i];
    if(diff) fail(c,"invalid LEGY checksum");
  } else if(raw->length%16) fail(c,"invalid LEGY block length");
  plain=aes(c,0,key,raw);
  if(plain->length<5 || plain->data[0]!=7) fail(c,"unsupported LEGY framing");
  p=plain->data+1; end=plain->data+plain->length;
  length=read_word(c,&p,end);
  if(length<2 || (size_t)(end-p)<length) fail(c,"invalid LEGY header length");
  end=p+length; count=read_word(c,&p,end);
  headers=cJSON_CreateObject(); json_add(c,c->response,"headers",headers);
  for(i=0;i<count;++i) {
    name=read_text(c,&p,end); val=read_text(c,&p,end);
    cJSON_DeleteItemFromObjectCaseSensitive(headers,name);
    json_add(c,headers,name,cJSON_CreateString(val));
  }
  if(p!=end) fail(c,"invalid LEGY header count");
  status=cJSON_GetObjectItemCaseSensitive(headers,"x-lc");
  if(cJSON_IsString(status) && status->valuestring[0]) {
    code=strtol(status->valuestring,&tail,10);
    if(*tail || tail==status->valuestring) fail(c,"invalid LEGY status");
  }
  if(code<100 || code>599) fail(c,"invalid LEGY status");
  json_add(c,c->response,"status",cJSON_CreateNumber((double)code));
  json_add(c,c->response,"body",cJSON_CreateString(base64(c,bytes(c,p,(size_t)(plain->data+plain->length-p)))));
}
void legy_service(Context *c,const char *operation) {
  if(!strcmp(operation,"/transport/legy/encode")) encode(c);
  else if(!strcmp(operation,"/transport/legy/decode")) decode(c);
  else fail(c,"unsupported LEGY operation");
}
