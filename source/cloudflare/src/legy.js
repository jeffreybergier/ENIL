// LINE LEGY 7 framing. Protocol constants: LINEJS ef6c3d9.
// Pure crypto only; this module never performs network requests.
const IV = new Uint8Array([78,9,72,62,56,245,255,114,128,18,123,158,251,92,45,51]);
const PUBLIC_KEY = `-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAsMC6HAYeMq4R59e2yRw6
W1OWT2t9aepiAp4fbSCXzRj7A29BOAFAvKlzAub4oxN13Nt8dbcB+ICAufyDnN5N
d3+vXgDxEXZ/sx2/wuFbC3B3evSNKR4hKcs80suRs8aL6EeWi+bAU2oYIc78Bbqh
Nzx0WCzZSJbMBFw1VlsU/HQ/XdiUufopl5QSa0S246XXmwJmmXRO0v7bNvrxaNV0
cbviGkOvTlBt1+RerIFHMTw3SwLDnCOolTz3CuE5V2OrPZCmC0nlmPRzwUfxoxxs
/6qFdpZNoORH/s5mQenSyqPkmH8TBOlHJWPH3eN1k6aZIlK5S54mcUb/oNRRq9wD
1wIDAQAB
-----END PUBLIC KEY-----`;
const enc = new TextEncoder(), dec = new TextDecoder("utf-8", {fatal:true});
const concat = (...xs) => { const out=new Uint8Array(xs.reduce((n,x)=>n+x.length,0));let p=0;for(const x of xs){out.set(x,p);p+=x.length;}return out; };
function b64(s) { if(typeof s!=="string" || !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(s))throw new TypeError("invalid base64");return Uint8Array.from(atob(s),c=>c.charCodeAt(0)); }
function base64(a) { let s="";for(let i=0;i<a.length;i+=8192)s+=String.fromCharCode(...a.subarray(i,i+8192));return btoa(s); }
function u16(n) { if(n>65535)throw new TypeError("header too large");return new Uint8Array([n>>>8,n&255]); }
function u32(n) { return new Uint8Array([n>>>24,n>>>16&255,n>>>8&255,n&255]); }
const rot=(v,n)=>(v<<n|v>>>(32-n))>>>0;
export function xxhash32(a) {
  const p1=0x9e3779b1,p2=0x85ebca77,p3=0xc2b2ae3d,p4=0x27d4eb2f,p5=0x165667b1;
  const view=new DataView(a.buffer,a.byteOffset,a.byteLength);let i=0,h;
  const round=(v,x)=>Math.imul(rot((v+Math.imul(x,p2))>>>0,13),p1)>>>0;
  if(a.length>=16){let v1=(p1+p2)>>>0,v2=p2,v3=0,v4=(-p1)>>>0;
    while(i<=a.length-16){v1=round(v1,view.getUint32(i,true));v2=round(v2,view.getUint32(i+4,true));v3=round(v3,view.getUint32(i+8,true));v4=round(v4,view.getUint32(i+12,true));i+=16;}
    h=(rot(v1,1)+rot(v2,7)+rot(v3,12)+rot(v4,18))>>>0;
  }else h=p5;
  h=(h+a.length)>>>0;
  while(i<=a.length-4){h=Math.imul(rot((h+Math.imul(view.getUint32(i,true),p3))>>>0,17),p4)>>>0;i+=4;}
  while(i<a.length)h=Math.imul(rot((h+Math.imul(a[i++],p5))>>>0,11),p1)>>>0;
  h^=h>>>15;h=Math.imul(h,p2);h^=h>>>13;h=Math.imul(h,p3);return (h^h>>>16)>>>0;
}
export function legyMac(key,data) {
  const ip=key.map(v=>v^0x36),op=key.map(v=>v^0x5c);
  return u32(xxhash32(concat(op,u32(xxhash32(concat(ip,data))))));
}
export function encodeHeaders(headers) {
  const entries=Object.entries(headers),parts=[u16(entries.length)];
  for(const [k,v] of entries){const kb=enc.encode(k),vb=enc.encode(v);parts.push(u16(kb.length),kb,u16(vb.length),vb);}
  const data=concat(...parts);return concat(u16(data.length),data);
}
function decodeHeaders(a) {
  let pos=0; const word=()=>{if(pos+2>a.length)throw new TypeError("truncated LEGY headers");return a[pos++]*256+a[pos++];};
  const end=word()+2,count=word(),headers=Object.create(null);
  if(end>a.length)throw new TypeError("invalid LEGY header length");
  const string=()=>{const n=word();if(pos+n>end)throw new TypeError("invalid LEGY header");const s=dec.decode(a.subarray(pos,pos+n));pos+=n;return s;};
  for(let i=0;i<count;i++){const k=string();headers[k]=string();}
  if(pos!==end)throw new TypeError("invalid LEGY header count");return {headers,data:a.subarray(end)};
}
export async function legyEncode({path,body,accessToken=""}) {
  if(typeof path!=="string"||!/^\/[A-Za-z0-9/]+$/.test(path)||typeof accessToken!=="string"||/[\r\n]/.test(accessToken))throw new TypeError("invalid LEGY request");
  const raw=b64(body),key=crypto.getRandomValues(new Uint8Array(16));
  const headers={"x-lpqs":path};if(accessToken)headers["x-lt"]=/^[^:]+:[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+$/.test(accessToken)?accessToken.slice(accessToken.indexOf(":")+1):accessToken;
  const aes=await crypto.subtle.importKey("raw",key,"AES-CBC",false,["encrypt"]);
  const encrypted=new Uint8Array(await crypto.subtle.encrypt({name:"AES-CBC",iv:IV},aes,concat(new Uint8Array([7]),encodeHeaders(headers),raw)));
  const der=b64(PUBLIC_KEY.replace(/-----[^-]+-----|\s/g,""));
  const rsa=await crypto.subtle.importKey("spki",der,{name:"RSA-OAEP",hash:"SHA-1"},false,["encrypt"]);
  const wrapped=new Uint8Array(await crypto.subtle.encrypt("RSA-OAEP",rsa,key));
  return {body:base64(concat(encrypted,legyMac(key,encrypted))),key:base64(key),xLcs:"0008"+base64(wrapped)};
}
export async function legyDecode({key,body}) {
  const k=b64(key),raw=b64(body);if(k.length!==16||raw.length<16)throw new TypeError("invalid LEGY response");
  let cipher=raw;
  if(raw.length%16===4){cipher=raw.subarray(0,-4);const mac=legyMac(k,cipher);let diff=0;for(let i=0;i<4;i++)diff|=mac[i]^raw[raw.length-4+i];if(diff)throw new TypeError("invalid LEGY checksum");}
  else if(raw.length%16)throw new TypeError("invalid LEGY block length");
  const aes=await crypto.subtle.importKey("raw",k,"AES-CBC",false,["decrypt"]);
  const plain=new Uint8Array(await crypto.subtle.decrypt({name:"AES-CBC",iv:IV},aes,cipher));
  if(plain[0]!==7)throw new TypeError("unsupported LEGY version");
  const decoded=decodeHeaders(plain.subarray(1));const status=Number(decoded.headers["x-lc"]||200);
  if(!Number.isInteger(status)||status<100||status>599)throw new TypeError("invalid LEGY status");
  return {body:base64(decoded.data),headers:decoded.headers,status};
}
