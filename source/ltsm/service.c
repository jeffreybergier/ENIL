/* Worker-compatible, stateless orchestration around the native WASM library.
 * Restore-state serialization remains version 1 so sessions can move between
 * the native library and the Worker without replacing their encryption keys. */
#include "ltsm_internal.h"
#include "identity.h"
#include <math.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <string.h>

static const cJSON *field(const cJSON *o,const char *key) {
  return cJSON_GetObjectItemCaseSensitive(o,key);
}
const char *json_string(Context *c,const cJSON *o,const char *key) {
  const cJSON *v=field(o,key);
  if(!cJSON_IsString(v) || !v->valuestring[0]) fail(c,"missing or invalid string");
  return v->valuestring;
}
static const char *optional_string(Context *c,const cJSON *o,const char *key) {
  const cJSON *v=field(o,key);
  if(!v) return "";
  if(!cJSON_IsString(v)) fail(c,"invalid string");
  return v->valuestring;
}
U64 json_uint(Context *c,const cJSON *o,const char *key) {
  const cJSON *v=field(o,key); double n;
  if(!cJSON_IsNumber(v)) fail(c,"missing integer");
  n=v->valuedouble;
  if(!isfinite(n) || n<0 || n>9007199254740991.0 || floor(n)!=n)
    fail(c,"invalid integer");
  return (U64)n;
}
static U32 json_u32(Context *c,const cJSON *o,const char *key) {
  U64 n=json_uint(c,o,key);
  if(n>0xffffffffU) fail(c,"integer out of range");
  return (U32)n;
}
void json_add(Context *c,cJSON *o,const char *key,cJSON *v) {
  if(!v) fail(c,"JSON allocation failed");
  if(!cJSON_AddItemToObject(o,key,v)) { cJSON_Delete(v); fail(c,"JSON allocation failed"); }
}
static void append(Context *c,cJSON *a,cJSON *v) {
  if(!v) fail(c,"JSON allocation failed");
  if(!cJSON_AddItemToArray(a,v)) { cJSON_Delete(v); fail(c,"JSON allocation failed"); }
}
Value *unbase64(Context *c,const char *s) {
  size_t n=strlen(s),i,padding=0; int decoded; Value *v;
  if(n>LTSM_MAX_INPUT || n%4) fail(c,"invalid base64 length");
  if(n && s[n-1]=='=') ++padding;
  if(n>1 && s[n-2]=='=') ++padding;
  for(i=0;i<n-padding;++i) if(!((s[i]>='A'&&s[i]<='Z') ||
     (s[i]>='a'&&s[i]<='z') || (s[i]>='0'&&s[i]<='9') || s[i]=='+' || s[i]=='/'))
    fail(c,"invalid base64 data");
  v=bytes(c,NULL,n/4*3);
  if(n) {
    decoded=EVP_DecodeBlock(v->data,(const unsigned char *)s,(int)n);
    if(decoded<0 || (size_t)decoded<padding) fail(c,"invalid base64 data");
    v->length=(size_t)decoded-padding;
  }
  return v;
}
char *base64(Context *c,const Value *v) {
  char *s;
  if(!v || v->kind!=V_BYTES || v->length>LTSM_MAX_INPUT) fail(c,"expected bytes");
  s=allocate(c,4*((v->length+2)/3)+1);
  EVP_EncodeBlock((unsigned char *)s,v->data,(int)v->length); return s;
}
static void add_string(Context *c,cJSON *o,const char *key,const char *s) {
  json_add(c,o,key,cJSON_CreateString(s));
}
static void add_number(Context *c,cJSON *o,const char *key,U64 n) {
  json_add(c,o,key,cJSON_CreateNumber((double)n));
}
static Value *digest(Context *c,const char *s) {
  unsigned char d[SHA256_DIGEST_LENGTH]; Value *v;
  if(!SHA256((const unsigned char *)s,strlen(s),d)) fail(c,"SHA-256 failed");
  v=bytes(c,d,sizeof(d)); wipe(d,sizeof(d)); return v;
}
static void sign_request(Context *c) {
  const char *path=json_string(c,c->request,"path");
  const char *body=optional_string(c,c->request,"body");
  const char *token=optional_string(c,c->request,"accessToken");
  size_t n=strlen(path)+strlen(body); char *data=allocate(c,n+1);
  Value *key,*derived,*hmac,*result;
  if(!field(c->request,"body")) fail(c,"missing signing body");
  memcpy(data,path,strlen(path)); memcpy(data+strlen(path),body,strlen(body));
  key=invoke(c,"SecureKey","loadToken",NULL,1,string(c,ENIL_LTSM_TOKEN));
  derived=invoke(c,"SecureKey","deriveKey",key,2,digest(c,ENIL_LTSM_VERSION),digest(c,token));
  hmac=invoke(c,"Hmac","constructor",NULL,1,derived);
  result=invoke(c,"Hmac","digest",hmac,1,bytes(c,data,n));
  add_string(c,c->response,"hmac",base64(c,result));
}
static cJSON *restore_state(Context *c) {
  const char *arrays[]={"keys","qrKeys","channels"};
  const char *objects[]={"peerPublicKeys","groupSharedKeys"};
  const cJSON *old=field(c->request,"workerRestoreState"); cJSON *state,*v; size_t i;
  if(old && !cJSON_IsNull(old) && !cJSON_IsObject(old)) fail(c,"invalid restore state");
  state=cJSON_IsObject(old)?cJSON_Duplicate(old,1):cJSON_CreateObject();
  json_add(c,c->response,"workerRestoreState",state);
  v=cJSON_GetObjectItemCaseSensitive(state,"version");
  if(v && (!cJSON_IsNumber(v) || v->valuedouble!=1)) fail(c,"unsupported restore state version");
  if(!v) add_number(c,state,"version",1);
  for(i=0;i<3;++i) {
    v=cJSON_GetObjectItemCaseSensitive(state,arrays[i]);
    if(v && !cJSON_IsArray(v)) fail(c,"invalid restore state array");
    if(!v) json_add(c,state,arrays[i],cJSON_CreateArray());
  }
  for(i=0;i<2;++i) {
    v=cJSON_GetObjectItemCaseSensitive(state,objects[i]);
    if(v && !cJSON_IsObject(v)) fail(c,"invalid restore state map");
    if(!v) json_add(c,state,objects[i],cJSON_CreateObject());
  }
  return state;
}
static void store_key(Context *c,cJSON *state,U32 id,const char *exported,int qr,const char *public_key) {
  cJSON *a=cJSON_GetObjectItemCaseSensitive(state,qr?"qrKeys":"keys"),*v;
  char logical[64];
  snprintf(logical,sizeof(logical),qr?"qr-login-%u":"my-e2ee-%u",(unsigned)id);
  cJSON_ArrayForEach(v,a) if(cJSON_IsObject(v) && cJSON_IsString(field(v,"logicalId")) &&
      !strcmp(field(v,"logicalId")->valuestring,logical)) return;
  v=cJSON_CreateObject(); append(c,a,v);
  add_string(c,v,"logicalId",logical); add_string(c,v,"kind",qr?"qr-e2ee":"e2ee");
  add_number(c,v,"keyId",id); add_string(c,v,"exportedKey",exported);
  if(qr) add_string(c,v,"publicKey",public_key);
}
static void keygen(Context *c,cJSON *state) {
  Value *key,*pub,*exported; cJSON *v; U32 id=1;
  cJSON_ArrayForEach(v,field(state,"qrKeys")) {
    U32 current=json_u32(c,v,"keyId");
    if(current>=0x7ffffffeU) fail(c,"QR key identifier exhausted");
    if(current>=id) id=current+1;
  }
  key=invoke(c,"Curve25519Key","generate",NULL,1,number(c,1));
  pub=invoke(c,"Curve25519Key","getPublicKey",key,0);
  exported=invoke(c,"Curve25519Key","exportKey",key,0);
  add_number(c,c->response,"keyId",id);
  add_string(c,c->response,"publicKey",base64(c,pub));
  store_key(c,state,id,base64(c,exported),1,base64(c,pub));
}
static Value *load_key(Context *c,const char *s) {
  return invoke(c,"E2EEKey","loadKey",NULL,1,unbase64(c,s));
}
static Value *channel(Context *c,Value *key,Value *pub) {
  if(pub->length!=32) fail(c,"invalid Curve25519 public key length");
  return invoke(c,"Curve25519Key","createChannel",key,1,pub);
}
static void unwrap(Context *c,cJSON *state) {
  U32 id=json_u32(c,c->request,"qrKeyId"),keyid;
  cJSON *a=cJSON_GetObjectItemCaseSensitive(state,"qrKeys"),*v,*out,*item;
  const char *exported=NULL; Value *key,*ch,*chain,*size,*entry,*blob; U64 i;
  cJSON_ArrayForEach(v,a) if(json_u32(c,v,"keyId")==id) { exported=json_string(c,v,"exportedKey"); break; }
  if(!exported) fail(c,"QR key missing from restore state");
  key=invoke(c,"Curve25519Key","loadKey",NULL,1,unbase64(c,exported));
  ch=channel(c,key,unbase64(c,json_string(c,c->request,"peerPublicKey")));
  chain=invoke(c,"E2EEChannel","unwrapE2EEKeyChain",ch,1,
               unbase64(c,json_string(c,c->request,"encryptedKeyChain")));
  size=invoke(c,"E2EEKeychain","size",chain,0);
  if(size->kind!=V_NUMBER || size->number>1024) fail(c,"invalid keychain size");
  out=cJSON_CreateArray(); json_add(c,c->response,"keys",out);
  for(i=0;i<size->number;++i) {
    entry=invoke(c,"E2EEKeychain","get",chain,1,number(c,i));
    keyid=(U32)invoke(c,"E2EEKey","getKeyId",entry,0)->number;
    blob=invoke(c,"E2EEKey","exportKey",entry,0);
    item=cJSON_CreateObject(); append(c,out,item);
    add_number(c,item,"keyId",keyid); add_string(c,item,"exportedKey",base64(c,blob));
    store_key(c,state,keyid,base64(c,blob),0,NULL);
  }
  cJSON_Delete(cJSON_DetachItemViaPointer(a,v));
}
static void peer(Context *c,cJSON *state,const char *mid,U32 id,const char *pub) {
  cJSON *map=cJSON_GetObjectItemCaseSensitive(state,"peerPublicKeys"),*v;
  char fallback[40];
  if(!*mid) { snprintf(fallback,sizeof(fallback),"key:%u",(unsigned)id); mid=fallback; }
  cJSON_DeleteItemFromObjectCaseSensitive(map,mid);
  v=cJSON_CreateObject(); json_add(c,map,mid,v);
  add_number(c,v,"keyId",id); add_string(c,v,"publicKey",pub);
}
static Value *user_channel(Context *c,cJSON *state) {
  const char *private_key=json_string(c,c->request,"privateKey");
  const char *public_key=json_string(c,c->request,"peerPublicKey");
  const char *mid=optional_string(c,c->request,"peerMid");
  U32 id=json_u32(c,c->request,"privateKeyId"),pid=json_u32(c,c->request,"peerKeyId");
  store_key(c,state,id,private_key,0,NULL); peer(c,state,mid,pid,public_key);
  return channel(c,load_key(c,private_key),unbase64(c,public_key));
}
static Value *group_channel(Context *c,cJSON *state) {
  const cJSON *group=field(c->request,"groupKey");
  const char *private_key=json_string(c,c->request,"myPrivateKey");
  const char *creator=json_string(c,c->request,"creatorPublicKey");
  const char *sender=json_string(c,c->request,"senderPublicKey");
  const char *mid=json_string(c,c->request,"groupMid");
  U32 id=json_u32(c,c->request,"myPrivateKeyId"),gid=json_u32(c,c->request,"groupKeyId");
  cJSON *map=cJSON_GetObjectItemCaseSensitive(state,"groupSharedKeys"),*groups;
  char identifier[32]; Value *ch,*key;
  if(!cJSON_IsObject(group)) fail(c,"missing group key");
  store_key(c,state,id,private_key,0,NULL);
  peer(c,state,json_string(c,group,"creator"),json_u32(c,group,"creatorKeyId"),creator);
  peer(c,state,json_string(c,c->request,"senderMid"),json_u32(c,c->request,"senderKeyId"),sender);
  groups=cJSON_GetObjectItemCaseSensitive(map,mid);
  if(!groups) { groups=cJSON_CreateObject(); json_add(c,map,mid,groups); }
  if(!cJSON_IsObject(groups)) fail(c,"invalid group state");
  snprintf(identifier,sizeof(identifier),"%u",(unsigned)gid);
  cJSON_DeleteItemFromObjectCaseSensitive(groups,identifier);
  json_add(c,groups,identifier,cJSON_Duplicate(group,1));
  ch=channel(c,load_key(c,private_key),unbase64(c,creator));
  key=invoke(c,"E2EEChannel","unwrapGroupSharedKey",ch,1,
             unbase64(c,json_string(c,group,"encryptedSharedKey")));
  return channel(c,key,unbase64(c,sender));
}
/* cJSON's string representation cannot retain U+0000. Check JSON escapes
 * before decoding, including object keys; escaped backslashes stay literal. */
void json_check_text(Context *c,const char *text) {
  const char *p;
  for(p=text;*p;++p) if(*p=='\\') {
    ++p;
    if(!*p) break; /* The JSON parser will reject the incomplete escape. */
    if(*p=='u' && !strncmp(p+1,"0000",4))
      fail(c,"JSON NUL string is not representable");
  }
}
/* Reject invalid UTF-8 instead of silently returning corrupted authenticated
 * JSON. cJSON does not validate UTF-8 itself. Embedded NUL is rejected because
 * its string API cannot faithfully represent it. */
int valid_utf8(const unsigned char *s,size_t n) {
  size_t i=0; unsigned b,cp,min; int count,j;
  while(i<n) {
    b=s[i++]; if(!b) return 0; if(b<128) continue;
    if(b>=0xc2 && b<=0xdf) { count=1; cp=b&31; min=0x80; }
    else if(b>=0xe0 && b<=0xef) { count=2; cp=b&15; min=0x800; }
    else if(b>=0xf0 && b<=0xf4) { count=3; cp=b&7; min=0x10000; }
    else return 0;
    if((size_t)count>n-i) return 0;
    for(j=0;j<count;++j) { b=s[i++]; if((b&0xc0)!=0x80) return 0; cp=(cp<<6)|(b&63); }
    if(cp<min || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff) || cp==0xfffd) return 0;
  }
  return 1;
}
static void message(Context *c,cJSON *state,const char *op) {
  int group=strstr(op,"-group-")!=NULL, encrypt=strstr(op,"/encrypt-")!=NULL;
  int v1=strstr(op,"-v1")!=NULL;
  Value *ch=group?group_channel(c,state):user_channel(c,state),*result;
  Value *input=unbase64(c,json_string(c,c->request,encrypt?"plaintext":"ciphertext"));
  if(v1) result=invoke(c,"E2EEChannel","decryptV1",ch,1,input);
  else {
    Value *to=string(c,json_string(c,c->request,"to"));
    Value *from=string(c,json_string(c,c->request,"from"));
    Value *sid=number(c,json_u32(c,c->request,"senderKeyId"));
    Value *rid=number(c,json_u32(c,c->request,group?"groupKeyId":"receiverKeyId"));
    Value *kind=number(c,json_u32(c,c->request,"contentType"));
    if(encrypt) result=invoke(c,"E2EEChannel","encryptV2",ch,7,to,from,sid,rid,kind,
                            number(c,json_uint(c,c->request,"sequenceNumber")),input);
    else result=invoke(c,"E2EEChannel","decryptV2",ch,6,to,from,sid,rid,kind,input);
  }
  if(encrypt) add_string(c,c->response,"ciphertext",base64(c,result));
  else {
    cJSON *payload;
    if(result->kind!=V_BYTES || !valid_utf8(result->data,result->length))
      fail(c,"decrypted payload is not valid UTF-8");
    json_check_text(c,(const char *)result->data);
    payload=cJSON_ParseWithOpts((const char *)result->data,NULL,1);
    if(!payload) fail(c,"decrypted payload is not JSON");
    json_add(c,c->response,"payload",payload);
  }
}
static void create_group(Context *c,cJSON *state) {
  const cJSON *members=field(c->request,"members"),*member;
  const char *private_key=json_string(c,c->request,"myPrivateKey");
  Value *key,*shared,*ch,*encrypted; cJSON *out,*entry;
  U32 id=json_u32(c,c->request,"myPrivateKeyId");
  if(!cJSON_IsArray(members) || !cJSON_GetArraySize(members) || cJSON_GetArraySize(members)>1024)
    fail(c,"invalid group members");
  store_key(c,state,id,private_key,0,NULL);
  shared=invoke(c,"Curve25519Key","generate",NULL,1,number(c,1));
  key=load_key(c,private_key);
  out=cJSON_CreateArray(); json_add(c,c->response,"encryptedKeys",out);
  cJSON_ArrayForEach(member,members) {
    ch=channel(c,key,unbase64(c,json_string(c,member,"publicKey")));
    encrypted=invoke(c,"E2EEChannel","wrapGroupSharedKey",ch,1,shared);
    entry=cJSON_CreateObject(); append(c,out,entry);
    add_string(c,entry,"mid",json_string(c,member,"mid"));
    add_number(c,entry,"keyId",json_u32(c,member,"keyId"));
    add_string(c,entry,"encryptedKey",base64(c,encrypted));
  }
}
void service(Context *c,const char *op) {
  static const char *message_ops[]={"/e2ee/decrypt-user-v1","/e2ee/decrypt-user-v2",
    "/e2ee/decrypt-group-v1","/e2ee/decrypt-group-v2",
    "/e2ee/encrypt-user-v2","/e2ee/encrypt-group-v2"};
  cJSON *state; size_t i;
  if(!strcmp(op,"/sign")) { sign_request(c); return; }
  state=restore_state(c);
  if(!strcmp(op,"/keygen")) { keygen(c,state); return; }
  if(!strcmp(op,"/e2ee/unwrap-keychain")) { unwrap(c,state); return; }
  if(!strcmp(op,"/e2ee/create-group-key")) { create_group(c,state); return; }
  for(i=0;i<sizeof(message_ops)/sizeof(message_ops[0]);++i)
    if(!strcmp(op,message_ops[i])) { message(c,state,op); return; }
  fail(c,"unsupported LTSM operation");
}
