#import "../shared/ENILAccount.h"
#include <assert.h>
/* Synthetic native fixture: no account startup, credentials or network. */
static void TestRosterRows(NSString *path) {
  sqlite3 *writer=enil_db_open([path fileSystemRepresentation]);
  assert(writer && enil_db_create_tables(writer)==0);
  assert(sqlite3_exec(writer,"DELETE FROM message_boxes_v2; DELETE FROM contacts_v2;"
      "WITH RECURSIVE n(x) AS (SELECT 0 UNION ALL SELECT x+1 FROM n WHERE x<99999) "
      "INSERT INTO contacts_v2(mid,displayName,registered) SELECT printf('u%06d',x),printf('Person %06d',x),1 FROM n;"
      "INSERT INTO message_boxes_v2(id,lastDeliveredTime,unreadCount) SELECT mid,42,1 FROM contacts_v2;",NULL,NULL,NULL)==SQLITE_OK);
  ENILRosterRows *chats=[[ENILRosterRows alloc] initWithPath:path contacts:NO];
  ENILRosterRows *friends=[[ENILRosterRows alloc] initWithPath:path contacts:YES];
  assert([chats count]==100000 && [friends count]==100000);
  assert(![chats cachedObjectAtIndex:0] && ![friends cachedObjectAtIndex:0]);
  assert([chats collectionIdentifiers]==nil && [friends collectionIdentifiers]==nil);
  NSUInteger jumps[]={99999,0,512,1024,2048,4096,99999};
  NSUInteger i;
  NSTimeInterval started=[NSDate timeIntervalSinceReferenceDate];
  for(i=0;i<7;++i) {
    NSUInteger index=jumps[i];
    NSString *mid=[NSString stringWithFormat:@"u%06lu",(unsigned long)index];
    assert([[[chats objectAtIndex:index] objectForKey:@"chatMid"] isEqual:mid]);
    NSString *name=[NSString stringWithFormat:@"Person %06lu",(unsigned long)index];
    assert([[[friends objectAtIndex:index] objectForKey:@"displayName"] isEqual:name]);
    assert([chats readError]==nil && [friends readError]==nil);
    if(i==5) assert(![chats cachedObjectAtIndex:99999] && ![friends cachedObjectAtIndex:99999]);
  }
  assert([chats indexForMid:@"u099999"]==99999);
  assert(sqlite3_exec(writer,"UPDATE contacts_v2 SET displayName='Changed' WHERE mid='u099999'; DELETE FROM message_boxes_v2 WHERE id='u000000';",NULL,NULL,NULL)==SQLITE_OK);
  [friends invalidatePages]; [chats invalidatePages];
  assert([[[friends objectAtIndex:99999] objectForKey:@"displayName"] isEqual:@"Person 099999"]);
  assert([[[chats objectAtIndex:0] objectForKey:@"chatMid"] isEqual:@"u000000"]);
  ENILRosterRows *next=[[ENILRosterRows alloc] initWithPath:path contacts:NO];
  assert([next count]==99999 && [next indexForMid:@"u000000"]==NSNotFound);
  NSLog(@"PASS: 100000-row roster counts, bounded eviction, complete synchronous text, stable ties and replacement snapshots (%.3fs)",[NSDate timeIntervalSinceReferenceDate]-started);
  /* This header is shared by ARC UIKit and MRC AppKit harnesses. */
#if !__has_feature(objc_arc)
  [next release]; [chats release]; [friends release];
#endif
  enil_db_close(writer);
}
