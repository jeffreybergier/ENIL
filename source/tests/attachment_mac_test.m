/* Offline production attachment recovery on disposable fixtures only.
 * No account startup, keychain lookup, login, or real curl transfer is used.
 * --ui exercises the production message controller and page renderer through
 * a model subclass owning only the disposable database. Main-thread only;
 * the controller is destroyed before its borrowed resource delegate.
 */
#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>
#import "../macOS/MessageListViewController.h"
#import "../shared/ENILUserDefaults.h"
#include "enil_account.h"
#include "enil_db.h"
#include "enil_session.h"
#include "enil_sync.h"
#include "enil_cocoa_image.h"
#include "enil_crypto.h"
#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); \
} } while (0)

static int refused;
static pthread_mutex_t refusalLock = PTHREAD_MUTEX_INITIALIZER;

/* All shared sources are compiled with curl_easy_perform renamed to this.
 * Never inspect/log URLs or request bodies, which can contain private data. */
CURLcode enil_test_refuse_network(CURL *handle) {
  (void)handle;
  pthread_mutex_lock(&refusalLock);
  refused++;
  pthread_mutex_unlock(&refusalLock);
  return CURLE_COULDNT_CONNECT;
}

static int scalar(sqlite3 *db, const char *sql) {
  sqlite3_stmt *stmt = NULL;
  int value;
  CHECK(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
  CHECK(sqlite3_step(stmt) == SQLITE_ROW);
  value = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return value;
}

@interface OfflineAccount : ENILAccount {
  sqlite3 *fixtureDB_;
}
- (id)initWithFixture:(NSString *)path;
@end
@implementation OfflineAccount
- (id)initWithFixture:(NSString *)path;
{
  self = [super initWithAccountPath:path];
  if (self) {
    fixtureDB_ = enil_db_open([[path stringByAppendingPathComponent:
        @"enil.sqlite"] fileSystemRepresentation]);
    CHECK(fixtureDB_);
  }
  return self;
}
- (NSString *)htmlPageForChatId:(NSString *)chat
                   oldestTime:(long long *)oldest
                      hasMore:(BOOL *)more;
{
  int hasMore = 0;
  char *html = enil_account_chat_page_html(fixtureDB_, [chat UTF8String],
      "offline-self", [[ENILUserDefaults messageCSS] UTF8String],
      [[ENILUserDefaults emptyMessageCSS] UTF8String], 50, oldest, &hasMore);
  CHECK(html);
  NSString *result = [NSString stringWithUTF8String:html];
  enil_account_free(html);
  if (more) *more = hasMore != 0;
  return result;
}
- (void)dealloc;
{
  enil_db_close(fixtureDB_);
  [super dealloc];
}
@end

@interface OfflineResources : NSObject {
  NSString *root_;
}
- (id)initWithRoot:(NSString *)root;
@end
@implementation OfflineResources
- (id)initWithRoot:(NSString *)root;
{
  self = [super init];
  if (self) root_ = [[root stringByAppendingString:@"/"] copy];
  return self;
}
- (NSURLRequest *)webView:(WebView *)view resource:(id)identifier
         willSendRequest:(NSURLRequest *)request
        redirectResponse:(NSURLResponse *)response
          fromDataSource:(WebDataSource *)source;
{
  (void)view; (void)identifier; (void)response; (void)source;
  NSURL *url = [request URL];
  CHECK([url isFileURL]);
  CHECK([[[url path] stringByResolvingSymlinksInPath] hasPrefix:root_]);
  return request;
}
- (void)dealloc;
{
  [root_ release];
  [super dealloc];
}
@end

static void testUI(NSString *directory, NSString *chat) {
  [NSApplication sharedApplication];
  OfflineAccount *account = [[OfflineAccount alloc] initWithFixture:directory];
  MessageListViewController *controller =
      [[MessageListViewController alloc] initWithEngine:account];
  NSWindow *window = [[NSWindow alloc] initWithContentRect:
      NSMakeRect(80, 80, 900, 650) styleMask:NSTitledWindowMask
      backing:NSBackingStoreBuffered defer:NO];
  [window setReleasedWhenClosed:NO];
  [window setTitle:@"ENIL — OFFLINE disposable recovery test"];
  [window setContentView:[controller view]];
  WebView *web = (WebView *)[controller webView];
  CHECK([web isKindOfClass:[WebView class]]);
  OfflineResources *resources =
      [[OfflineResources alloc] initWithRoot:directory];
  [web setResourceLoadDelegate:resources];
  [controller reloadWithChatId:chat displayName:@"Offline fixture"];
  [window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
  NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:45];
  NSString *result = nil;
  do {
    [[NSRunLoop currentRunLoop] runUntilDate:
        [NSDate dateWithTimeIntervalSinceNow:0.25]];
    result = [web stringByEvaluatingJavaScriptFromString:
      @"(function(){var a=document.getElementsByTagName('a'),n=0,b=0;"
       "for(var i=0;i<a.length;i++){if(a[i].href.indexOf('enil-media:')!=0)"
       "continue;var im=a[i].getElementsByTagName('img')[0];"
       "if(im){n++;if(!im.complete||!im.naturalWidth)b++;}}"
       "return n+','+b;})()"];
    if (![web isLoading] && [result length] &&
        ![result hasPrefix:@"0,"] && [result hasSuffix:@",0"]) break;
  } while ([deadline timeIntervalSinceNow] > 0);
  fprintf(stderr, "UI attachment images (total,broken): %s\n",
      result ? [result UTF8String] : "(no result)");
  CHECK([result length] && ![result hasPrefix:@"0,"] &&
        [result hasSuffix:@",0"]);
  printf("PASS UI: loaded attachment thumbnails (total,broken)=%s\n",
      [result UTF8String]);
  /* Verify the original target advertised by the actual rendered link. */
  NSString *relative = [web stringByEvaluatingJavaScriptFromString:
    @"(function(){var a=document.getElementsByTagName('a');"
     "for(var i=0;i<a.length;i++)if(a[i].href.indexOf('enil-media:')==0)"
     "return a[i].href.substring(13);return '';})()"];
  CHECK([relative length]);
  NSString *original = [directory stringByAppendingPathComponent:relative];
  NSImage *image = [[NSImage alloc] initWithContentsOfFile:original];
  CHECK(image && [image isValid]);
  [image release];
  [web stringByEvaluatingJavaScriptFromString:
    @"(function(){var a=document.getElementsByTagName('a');"
     "for(var i=0;i<a.length;i++)if(a[i].href.indexOf('enil-media:')==0)"
     "{a[i].scrollIntoView();break;}})()"];
  [[NSRunLoop currentRunLoop] runUntilDate:
      [NSDate dateWithTimeIntervalSinceNow:0.5]];
  [web lockFocus];
  NSBitmapImageRep *shot = [[NSBitmapImageRep alloc]
      initWithFocusedViewRect:[web bounds]];
  [web unlockFocus];
  CHECK([[shot representationUsingType:NSPNGFileType properties:
      [NSDictionary dictionary]] writeToFile:
      [directory stringByAppendingPathComponent:@"ui.png"] atomically:YES]);
  [shot release];
  printf("PASS UI: original image link resolves and decodes; ui.png saved\n");
  [web setResourceLoadDelegate:nil];
  [window orderOut:nil];
  [window setContentView:nil];
  [controller release];
  [resources release];
  [account release];
  [window close];
  [window release];
}

int main(int argc, char **argv) {
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
  int ui = 0, regenerate = 0, removed = 0, arg, expectedComplete = -1;
  CHECK(argc >= 2 && argc <= 5);
  for (arg = 2; arg < argc; arg++) {
    if (!strcmp(argv[arg], "--ui")) ui = 1;
    else if (!strcmp(argv[arg], "--regenerate")) regenerate = 1;
    else if (!strncmp(argv[arg], "--expect-complete=", 18)) {
      char *end = NULL;
      long value = strtol(argv[arg] + 18, &end, 10);
      CHECK(*end == 0 && value >= 0 && value <= 1000000);
      expectedComplete = (int)value;
    }
    else CHECK(0);
  }
  NSString *directory = [[NSString stringWithUTF8String:argv[1]]
      stringByResolvingSymlinksInPath];
  CHECK([directory hasPrefix:[NSHomeDirectory()
      stringByAppendingString:@"/Desktop/"]]);
  CHECK([[NSFileManager defaultManager] fileExistsAtPath:
      [directory stringByAppendingPathComponent:@".enil-offline-fixture"]]);
  NSString *session = [directory stringByAppendingPathComponent:@"session.json"];
  CHECK(![[NSFileManager defaultManager] fileExistsAtPath:session]);
  enil_identity_t identity;
  CHECK(enil_identity_default("chrome", &identity));
  cJSON *synthetic = cJSON_Parse(
      "{\"accessToken\":\"offline-fixture\",\"mid\":\"offline-self\"}");
  cJSON_AddItemToObject(synthetic, "clientIdentity",
      enil_identity_to_json(&identity));
  CHECK(enil_session_write([session fileSystemRepresentation], synthetic));
  cJSON_Delete(synthetic);
  sqlite3 *db = enil_db_open([[directory stringByAppendingPathComponent:
      @"enil.sqlite"] fileSystemRepresentation]);
  CHECK(db);
  long long revision = enil_db_get_local_rev(db);
  int messages = scalar(db, "SELECT count(*) FROM messages_v2");
  int before = scalar(db, "SELECT count(*) FROM messages_v2 WHERE "
      "contentType=1 AND media_path IS NOT NULL AND thumb_path IS NOT NULL");
  cJSON *entries = enil_db_get_messages_needing_media(db);
  CHECK(entries);
  int i, available = 0, unavailable = 0;
  for (i = 0; i < cJSON_GetArraySize(entries); i++) {
    cJSON *row = cJSON_GetArrayItem(entries, i);
    const char *mid = cJSON_GetStringValue(cJSON_GetObjectItem(row,"message_id"));
    const char *chat = cJSON_GetStringValue(cJSON_GetObjectItem(row,"chat_id"));
    CHECK(mid && chat && !strchr(mid, '/') && !strchr(chat, '/'));
    NSString *path = [directory stringByAppendingPathComponent:
        [NSString stringWithFormat:@"media/%s/%s.jpg", chat, mid]];
    struct stat st;
    double size = cJSON_GetNumberValue(cJSON_GetObjectItem(row, "plain_size"));
    const char *key = cJSON_GetStringValue(cJSON_GetObjectItem(row, "enc_km"));
    int valid = enil_image_validate([path fileSystemRepresentation], NULL, NULL)
        == 0 && (size <= 0 || (stat([path fileSystemRepresentation], &st) == 0
        && ((double)st.st_size == size ||
        (key && key[0] && (double)st.st_size + ENIL_CRYPTO_FILE_TAG_LEN == size))));
    cJSON_AddBoolToObject(row, "expected", valid);
    if (valid) available++; else unavailable++;
    if (valid && regenerate && !removed) {
      NSString *thumb = [directory stringByAppendingPathComponent:
          [NSString stringWithFormat:@"media/%s/%s_t.jpg", chat, mid]];
      CHECK(unlink([thumb fileSystemRepresentation]) == 0);
      removed = 1;
    }
  }
  CHECK(!regenerate || removed);
  CHECK(expectedComplete < 0 || available == expectedComplete);
  int result = enil_sync_attachments(db, "offline-fixture", "offline-self",
      [session fileSystemRepresentation]);
  CHECK(result > 0);
  for (i = 0; i < cJSON_GetArraySize(entries); i++) {
    cJSON *row = cJSON_GetArrayItem(entries, i);
    const char *mid = cJSON_GetStringValue(cJSON_GetObjectItem(row,"message_id"));
    const char *chat = cJSON_GetStringValue(cJSON_GetObjectItem(row,"chat_id"));
    int expected = cJSON_IsTrue(cJSON_GetObjectItem(row, "expected"));
    sqlite3_stmt *stmt = NULL;
    CHECK(sqlite3_prepare_v2(db, "SELECT media_path,thumb_path,orig_width,"
        "orig_height,thumb_width,thumb_height FROM messages_v2 WHERE id=?",
        -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, mid, -1, SQLITE_TRANSIENT);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    int j;
    for (j = 0; j < 2; j++) {
      if (!expected) {
        CHECK(sqlite3_column_type(stmt, j) == SQLITE_NULL);
        CHECK(sqlite3_column_int(stmt, 2 + j * 2) == 0);
        CHECK(sqlite3_column_int(stmt, 3 + j * 2) == 0);
        continue;
      }
      const char *relative = (const char *)sqlite3_column_text(stmt, j);
      NSString *wanted = [NSString stringWithFormat:@"media/%s/%s%s.jpg",
          chat, mid, j ? "_t" : ""];
      CHECK(relative && !strcmp(relative, [wanted UTF8String]));
      NSString *path = [directory stringByAppendingPathComponent:wanted];
      int width = 0, height = 0;
      CHECK(enil_image_validate([path fileSystemRepresentation],
          &width, &height) == 0);
      CHECK(width == sqlite3_column_int(stmt, 2 + j * 2));
      CHECK(height == sqlite3_column_int(stmt, 3 + j * 2));
    }
    sqlite3_finalize(stmt);
  }
  CHECK(enil_db_get_local_rev(db) == revision);
  CHECK(scalar(db, "SELECT count(*) FROM messages_v2") == messages);
  CHECK(result - 1 >= unavailable);
  printf("PASS recovery: %d images; %d complete before; %d recovered/complete; "
      "%d unavailable offline; %d total attachment failures; "
      "%d curl attempts refused\n", cJSON_GetArraySize(entries), before,
      available, unavailable, result - 1, refused);
  if (regenerate) printf("PASS: deliberately removed thumbnail regenerated\n");
  cJSON_Delete(entries);
  NSString *chat = nil;
  if (ui) {
    sqlite3_stmt *stmt = NULL;
    /* Select an image in the actual first page, not a chat whose images
     * require pagination beyond the production 50-message page size. */
    CHECK(sqlite3_prepare_v2(db, "SELECT m.chat_id FROM messages_v2 m WHERE "
        "m.contentType=1 AND m.media_path IS NOT NULL AND "
        "(SELECT count(*) FROM messages_v2 n WHERE n.chat_id=m.chat_id "
        "AND n.createdTime>=m.createdTime)<=50 "
        "ORDER BY m.createdTime DESC LIMIT 1", -1, &stmt, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(stmt) == SQLITE_ROW);
    chat = [NSString stringWithUTF8String:
        (const char *)sqlite3_column_text(stmt, 0)];
    sqlite3_finalize(stmt);
  }
  enil_db_close(db);
  CHECK(unlink([session fileSystemRepresentation]) == 0);
  if (ui) testUI(directory, chat);
  [pool drain];
  return 0;
}
