"""Compact Thrift checked against Apache Thrift, plus loopback native routing."""
import base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import shlex
import subprocess
import tempfile
import threading
import unittest
from thrift.Thrift import TType, TMessageType
from thrift.protocol.TCompactProtocol import TCompactProtocol
from thrift.transport.TTransport import TMemoryBuffer
from test_windows_login_probe import reply
REPO=Path(__file__).resolve().parents[2]

class NativeTransportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();cls.addClassCleanup(cls.tmp.cleanup)
        cls.bin=str(Path(cls.tmp.name)/'native')
        flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','libcjson','libcurl','openssl'],text=True))
        shared=REPO/'source/shared'
        subprocess.run(['cc','-std=gnu99','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-ffunction-sections','-fdata-sections','-I'+str(shared),str(REPO/'source/tests/native_transport.c'),*[str(shared/x) for x in ['enil_native.c','enil_thrift.c','enil_identity.c','enil_line.c','enil_http.c','enil_b64.c','enil_session.c','enil_api_json.c','enil_sse.c','enil_talkserv.c','enil_api_call.c']],'-Wl,--gc-sections','-Wl,--wrap=curl_easy_perform','-pthread',*flags,'-o',cls.bin],check=True)
    def run_native(self,*args,**kw):
        return subprocess.run([self.bin,'http://127.0.0.1:1',*args],capture_output=True,check=True,**kw).stdout
    def decode(self,method,wire):return json.loads(self.run_native('decode',method,input=wire))
    def test_exact_binary_message_and_i64_encoding(self):
        chunks=[b'\0\xff\x80test',bytes(range(256))]
        message={'from':'self','to':'peer','createdTime':'9007199254740993','contentType':0,'chunks':[base64.b64encode(x).decode() for x in chunks]}
        actual=self.run_native('encode','sendMessage',json.dumps([37,message]))
        buf=TMemoryBuffer();p=TCompactProtocol(buf);p.writeMessageBegin('sendMessage',TMessageType.CALL,0);p.writeStructBegin('args')
        p.writeFieldBegin('seq',TType.I32,1);p.writeI32(37);p.writeFieldEnd();p.writeFieldBegin('message',TType.STRUCT,2);p.writeStructBegin('message')
        for name,fid in [('from',1),('to',2)]:p.writeFieldBegin(name,TType.STRING,fid);p.writeString(message[name]);p.writeFieldEnd()
        p.writeFieldBegin('time',TType.I64,5);p.writeI64(9007199254740993);p.writeFieldEnd()
        p.writeFieldBegin('content',TType.I32,15);p.writeI32(0);p.writeFieldEnd()
        p.writeFieldBegin('chunks',TType.LIST,20);p.writeListBegin(TType.STRING,2)
        for chunk in chunks:p.writeBinary(chunk)
        p.writeListEnd();p.writeFieldEnd();p.writeFieldStop();p.writeStructEnd();p.writeFieldEnd();p.writeFieldStop();p.writeStructEnd();p.writeMessageEnd()
        self.assertEqual(actual,buf.getvalue())
    def test_revision_and_malformed_reply(self):
        wire=reply('getLastOpRevision',[(0,TType.I64,9007199254740993)])
        self.assertEqual(self.decode('getLastOpRevision',wire)['success'],'9007199254740993')
        for i in range(len(wire)):
            r=subprocess.run([self.bin,'http://127.0.0.1:1','decode','getLastOpRevision'],input=wire[:i],capture_output=True)
            self.assertEqual(r.returncode,2,i)
    def test_native_routing_refresh_and_exceptions(self):
        records=[]
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*args):pass
            def do_POST(self):
                raw=self.rfile.read(int(self.headers['Content-Length']));p=TCompactProtocol(TMemoryBuffer(raw));method,kind,seq=p.readMessageBegin()
                records.append((self.path,method,dict(self.headers),raw));fields=[(0,TType.STRUCT,[(1,TType.STRING,'synthetic-mid'),(20,TType.STRING,'Synthetic')])]
                if method=='refresh':fields=[(0,TType.STRUCT,[(1,TType.STRING,'rotated'),(2,TType.I64,3600),(4,TType.I64,1800000000),(5,TType.STRING,'rotated-refresh')])]
                if method=='getE2EEPublicKey':fields=[(1,TType.STRUCT,[(1,TType.I32,5),(2,TType.STRING,'missing')])]
                wire=reply(method,fields);self.send_response(200);self.send_header('Content-Length',str(len(wire)));self.end_headers();self.wfile.write(wire)
        server=ThreadingHTTPServer(('127.0.0.1',0),Handler);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        try:
            origin=f'http://127.0.0.1:{server.server_port}'
            for path,body in [('/api/talk/thrift/Talk/TalkService/getProfile','[0]'),('/api/auth/tokenRefresh','{"refreshToken":"synthetic-refresh"}'),('/api/talk/thrift/Talk/TalkService/getE2EEPublicKey','["peer",1,1]')]:
                out=subprocess.check_output([self.bin,origin,path,body],text=True);status,data=out.split('\n',1);data=json.loads(data)
                if path.endswith('getE2EEPublicKey'):self.assertEqual(status,'400');self.assertEqual(data['code'],5)
                else:self.assertEqual(status,'200');self.assertEqual(data['message'],'OK')
            self.assertEqual([r[0] for r in records],['/enc','/EXT/auth/tokenrefresh/v1','/enc'])
            for path,method,headers,raw in records:
                lower={k.lower():v for k,v in headers.items()}
                self.assertTrue(lower['x-line-application'].startswith('DESKTOPWIN\t'))
                self.assertEqual(lower['user-agent'],'Line/9.7.0.3556')
                for absent in ['origin','cookie','x-hmac','x-line-chrome-version']:self.assertNotIn(absent,lower)
                if path=='/enc':self.assertEqual(lower['x-lcs'],'0008synthetic');self.assertNotIn('x-line-access',lower)
        finally:server.shutdown();server.server_close();thread.join()
    def test_stale_sync_preserves_rotated_tokens_and_raw_login(self):
        self.run_native('save',str(Path(self.tmp.name)/'session.json'))
        self.assertEqual((Path(self.tmp.name)/'session.json').stat().st_mode&0o777,0o600)

    def test_polling_persists_exact_cursors_only_after_success(self):
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*args):pass
            def do_POST(self):
                self.rfile.read(int(self.headers['Content-Length']))
                buf=TMemoryBuffer();p=TCompactProtocol(buf);p.writeMessageBegin('sync',TMessageType.REPLY,0);p.writeStructBegin('result')
                p.writeFieldBegin('success',TType.STRUCT,0);p.writeStructBegin('success');p.writeFieldBegin('operationResponse',TType.STRUCT,1);p.writeStructBegin('opResponse')
                p.writeFieldBegin('operations',TType.LIST,1);p.writeListBegin(TType.STRUCT,1);p.writeStructBegin('op');p.writeFieldBegin('revision',TType.I64,1);p.writeI64(9007199254740993);p.writeFieldEnd();p.writeFieldBegin('type',TType.I32,3);p.writeI32(0);p.writeFieldEnd();p.writeFieldStop();p.writeStructEnd();p.writeListEnd();p.writeFieldEnd()
                p.writeFieldBegin('hasMoreOps',TType.BOOL,2);p.writeBool(False);p.writeFieldEnd()
                for fid,value in [(3,9007199254740994),(4,9007199254740995)]:
                    p.writeFieldBegin('events',TType.STRUCT,fid);p.writeStructBegin('events');p.writeFieldBegin('revision',TType.I64,2);p.writeI64(value);p.writeFieldEnd();p.writeFieldStop();p.writeStructEnd();p.writeFieldEnd()
                p.writeFieldStop();p.writeStructEnd();p.writeFieldEnd();p.writeFieldStop();p.writeStructEnd();p.writeFieldEnd();p.writeFieldStop();p.writeStructEnd();p.writeMessageEnd()
                data=buf.getvalue();self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
        server=ThreadingHTTPServer(('127.0.0.1',0),Handler);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        try:
            for mode in ['poll','poll-fail']:
                subprocess.run([self.bin,f'http://127.0.0.1:{server.server_port}',mode,str(Path(self.tmp.name)/(mode+'.json'))],check=True,timeout=10)
        finally:server.shutdown();server.server_close();thread.join()

    def test_interrupted_refresh_recovers_without_network(self):
        self.run_native('refresh-recovery',str(Path(self.tmp.name)/'refresh-session.json'))

    def test_binary_message_reply(self):
        buf=TMemoryBuffer();p=TCompactProtocol(buf);p.writeMessageBegin('sendMessage',TMessageType.REPLY,0);p.writeStructBegin('result')
        p.writeFieldBegin('success',TType.STRUCT,0);p.writeStructBegin('message');p.writeFieldBegin('id',TType.STRING,4);p.writeString('synthetic-message');p.writeFieldEnd()
        p.writeFieldBegin('chunks',TType.LIST,20);p.writeListBegin(TType.STRING,1);p.writeBinary(b'\0\xff\x80ciphertext');p.writeListEnd();p.writeFieldEnd()
        p.writeFieldStop();p.writeStructEnd();p.writeFieldEnd();p.writeFieldStop();p.writeStructEnd();p.writeMessageEnd()
        decoded=self.decode('sendMessage',buf.getvalue())['success']
        self.assertEqual(decoded['chunks'],[base64.b64encode(b'\0\xff\x80ciphertext').decode()])

    def test_superseded_and_committed_refresh_journals(self):
        calls = []
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args): pass
            def do_POST(self):
                wire = self.rfile.read(int(self.headers['Content-Length']))
                protocol = TCompactProtocol(TMemoryBuffer(wire))
                method, _, _ = protocol.readMessageBegin()
                calls.append(method)
                body = reply('refresh', [(0, TType.STRUCT, [
                    (1, TType.STRING, 'fresh-from-fixture'),
                    (5, TType.STRING, 'fresh-refresh-token')])])
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            for mode in ['journal-stale', 'journal-superseded', 'journal-committed', 'journal-legacy-committed']:
                with self.subTest(mode=mode):
                    subprocess.run([self.bin, f'http://127.0.0.1:{server.server_port}', mode,
                                    str(Path(self.tmp.name) / (mode + '.json'))], check=True, timeout=10)
            self.assertEqual(calls, ['refresh', 'refresh'])
        finally:
            server.shutdown(); server.server_close(); thread.join()
