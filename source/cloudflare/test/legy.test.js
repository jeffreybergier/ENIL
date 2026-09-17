import { describe,it,expect } from 'vitest';
import { createCipheriv,createDecipheriv,webcrypto } from 'node:crypto';
import { legyEncode,legyDecode,legyMac,encodeHeaders,xxhash32 } from '../src/legy.js';
globalThis.crypto ||= webcrypto;
const iv=Buffer.from([78,9,72,62,56,245,255,114,128,18,123,158,251,92,45,51]);
describe('LEGY native transport crypto',()=>{
  it('matches published xxHash32 vectors',()=>{
    expect(xxhash32(Buffer.from(''))).toBe(0x02cc5d05);
    expect(xxhash32(Buffer.from('a'))).toBe(0x550d7456);
    expect(xxhash32(Buffer.from('abc'))).toBe(0x32d153ff);
  });
  it('wraps an authenticated Thrift request with OAEP and AES-CBC',async()=>{
    const out=await legyEncode({path:'/S4',accessToken:'synthetic-token',body:Buffer.from([0x82,0x21,0]).toString('base64')});
    const key=Buffer.from(out.key,'base64'),wire=Buffer.from(out.body,'base64');
    expect(Buffer.from(out.xLcs.slice(4),'base64').length).toBe(256);
    expect(out.xLcs.slice(0,4)).toBe('0008');
    expect(wire.subarray(-4)).toEqual(Buffer.from(legyMac(key,wire.subarray(0,-4))));
    const decipher=createDecipheriv('aes-128-cbc',key,iv);
    const plain=Buffer.concat([decipher.update(wire.subarray(0,-4)),decipher.final()]);
    expect(plain).toEqual(Buffer.concat([Buffer.from([7]),encodeHeaders({'x-lpqs':'/S4','x-lt':'synthetic-token'}),Buffer.from([0x82,0x21,0])]));
  });
  it('decodes server status and binary payload and rejects damaged packets',async()=>{
    const key=Buffer.alloc(16,37),cipher=createCipheriv('aes-128-cbc',key,iv);
    const payload=Buffer.from([0x82,0x41,0,0,255]);
    const plain=Buffer.concat([Buffer.from([7]),encodeHeaders({'x-lc':'200'}),payload]);
    const ct=Buffer.concat([cipher.update(plain),cipher.final()]);
    const raw=Buffer.concat([ct,legyMac(key,ct)]);
    const decode=(bytes)=>legyDecode({key:key.toString('base64'),body:bytes.toString('base64')});
    expect(await decode(raw)).toMatchObject({status:200,body:payload.toString('base64')});
    raw[2]^=1;await expect(decode(raw)).rejects.toThrow('checksum');
    await expect(decode(raw.subarray(0,-1))).rejects.toThrow('length');
  });
});
