"""Generate ENIL's reachable wire schema from LINEJS's pinned protocol IDL.
Usage: python generate.py /path/to/line.thrift
Reference: evex-dev/linejs ef6c3d9f70dd41fa51053615d47f071f58cf8db3.
The IDL is research input, not a runtime/build dependency.
"""
import json, re, sys
from pathlib import Path
text = Path(sys.argv[1]).read_text()
enums = set(re.findall(r'enum\s+(\w+)', text))
def typ(s):
    s = s.replace(' ', '')
    m = re.fullmatch(r'(list|set|map)<(.+)>', s)
    if m:
        parts = re.split(r',(?![^<]*>)', m[2])
        return [m[1]] + [typ(p) for p in parts]
    return 'i32' if s in enums else s
schemas = {}
for name, body in re.findall(r'(?:struct|exception)\s+(\w+)\s*\{([^}]*)\}', text):
    fields = []
    for fid, ts, fname in re.findall(r'(\d+)\s*:\s*(?:optional\s+|required\s+)?([\w<> ,]+?)\s+(\w+)\s*[;,\n]', body):
        fields.append([int(fid), fname, typ(ts)])
    schemas[name] = sorted(fields)
schemas['getContacts_args'] = [[2,'mids',['list','string']]]
schemas['getContacts_result'] = [[0,'success',['list','Contact']],[1,'e','TalkException']]
schemas['getRecentMessagesV2_args'] = [[2,'messageBoxId','string'],[3,'messagesCount','i32']]
schemas['getRecentMessagesV2_result'] = [[0,'success',['list','Message']],[1,'e','TalkException']]
schemas['refresh_args'] = [[1,'request','ENILRefreshRequest']]
schemas['ENILRefreshRequest'] = [[1,'refreshToken','string']]
schemas['refresh_result'] = [[0,'success','ENILRefreshResponse'],[1,'e','TalkException']]
schemas['ENILRefreshResponse'] = [[1,'accessToken','string'],[2,'durationUntilRefreshInSec','i64'],[3,'refreshApiRetryPolicy','RefreshApiRetryPolicy'],[4,'tokenIssueTimeEpochSec','i64'],[5,'refreshToken','string']]
for struct, field, new in [('Message','contentPreview','binary'),('Message','chunks',['list','binary']),('Pb1_C13097n4','keyData','binary'),('Pb1_U3','encryptedSharedKey','binary'),('registerE2EEGroupKey_args','encryptedSharedKeys',['list','binary'])]:
    for f in schemas[struct]:
        if f[1] == field: f[2] = new
schemas['YN0_Ob1_N0'] = schemas['ProductSummaryList']
schemas['getLastOpRevision_args'] = []
schemas['getLastOpRevision_result'] = [[0,'success','i64'],[1,'e','TalkException']]
schemas['getServerTime_args'] = []
schemas['getServerTime_result'] = [[0,'success','i64'],[1,'e','TalkException']]
schemas['TMessageReadRange'][1][2] = ['map','string',['list','TMessageReadRangeEntry']]
methods = '''getProfile getAllChatMids getChats getAllContactIds getContacts getRecentMessagesV2 getMessageBoxes determineMediaMessageFlow getE2EEPublicKey getLastE2EEPublicKeys getLastE2EEGroupSharedKey getE2EEGroupSharedKey negotiateE2EEPublicKey registerE2EEGroupKey acquireEncryptedAccessToken getOwnedProductSummaries sendChatChecked sendChatRemoved sendMessage getLastOpRevision getServerTime getSettings getConfigurations getMessageReadRange sync refresh'''.split()
result = {}
primitives = {'string','binary','bool','byte','i16','i32','i64','double','void','_any'}
def visit(t):
    if isinstance(t,list):
        for x in t[1:]: visit(x)
    elif t not in primitives and t not in result:
        if t not in schemas: raise ValueError('Unknown type: '+t)
        result[t] = schemas[t]
        for f in result[t]: visit(f[2])
for m in methods:
    visit(m+'_args'); visit(m+'_result')
out = Path(__file__).resolve().parents[2]/'shared/enil_thrift_schema.inc'
out.write_text('/* Generated protocol field definitions; see tools/native-schema/generate.py. */\n' + '\n'.join(json.dumps(json.dumps({k:v},separators=(',',':'))[1:-1]+(',' if i<len(result)-1 else '')) for i,(k,v) in enumerate(sorted(result.items())))+'\n')
print(len(result), 'reachable structs')
