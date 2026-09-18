/* Feed real libcurl callback chunks into the production Chrome SSE parser.
 * Including the implementation exposes only its private parser to this test;
 * no network or thread timing is needed to test arbitrary chunk boundaries. */
#include "../shared/enil_sse.c"
#include <assert.h>

static int failed, delivered, reject_event;
static long long saved_revision;
static const char *expected_text;

void enil_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
int enil_health_any_failed(void) { return failed; }
void enil_health_set_failure(enil_err_source_t source, const char *message) {
  assert(source == ENIL_ERR_LINE && message && *message);
  failed = 1;
}
int enil_db_set_local_rev(sqlite3 *db, long long revision) {
  (void)db;
  saved_revision = revision;
  return SQLITE_OK;
}

static int receive_event(const ENILSSEEvent *event, void *ctx) {
  cJSON *root = cJSON_Parse(event->data), *text;
  (void)ctx;
  assert(root && !strcmp(event->type, "message"));
  if (reject_event) { cJSON_Delete(root); return 0; }
  text = cJSON_GetObjectItemCaseSensitive(root, "text");
  assert(cJSON_IsString(text));
  assert(!strcmp(text->valuestring, delivered == 0 ? expected_text : "next"));
  delivered++;
  cJSON_Delete(root);
  return 1;
}

static int feed(ENILSSEState *state, const char *wire, size_t size, size_t chunk) {
  size_t offset;
  for (offset = 0; offset < size; offset += chunk) {
    size_t n = size - offset < chunk ? size - offset : chunk;
    if (write_cb((char *)wire + offset, 1, n, state) != n) return 0;
  }
  return 1;
}

int main(int argc, char **argv) {
  ENILSSEClient client = {0};
  ENILSSEState state = {0};
  const char *next = "data: {\"revision\":\"21\",\"text\":\"next\"}\n\n";
  size_t length, chunk;
  char *text, *wire;
  int multiline, oversize, interrupted, retry;
  assert(argc == 3);
  multiline = !strcmp(argv[1], "multiline");
  oversize = !strncmp(argv[1], "oversize", 8);
  interrupted = !strcmp(argv[1], "interrupted");
  retry = !strcmp(argv[1], "retry");
  chunk = (size_t)strtoul(argv[2], NULL, 10);
  assert(chunk > 0);
  length = !strcmp(argv[1], "large") || multiline ? 128 * 1024 : 3000;
  if (oversize) length = SSE_MAX_EVENT_SIZE;
  if (!strcmp(argv[1], "limit"))
    length = SSE_MAX_EVENT_SIZE - strlen("{\"revision\":\"20\",\"text\":\"\"}");
  text = malloc(length + 1);
  wire = malloc(length + 256);
  assert(text && wire);
  memset(text, 'x', length);
  text[length] = 0;
  expected_text = text;
  client.local_rev = saved_revision = 10;
  client.db = (sqlite3 *)1;
  client.fn = receive_event;
  state.client = &client;
  /* Valid multiline JSON, CRLF, an empty data field, and no explicit event
   * type on the following event exercise both framing and state reset. */
  sprintf(wire, multiline
    ? "event: message\r\ndata: {\r\ndata:\r\ndata: \"revision\":\"20\",\"text\":\"%s\"}\r\n\r\n"
    : "event: message\r\ndata: {\"revision\":\"20\",\"text\":\"%s\"}\r\n\r\n", text);
  if (!strcmp(argv[1], "oversize-event")) {
    /* Each line fits; their combined data exceeds the event bound. */
    size_t half = length / 2;
    memmove(wire + half + 7, wire + half, strlen(wire + half) + 1);
    memcpy(wire + half, "\ndata: ", 7);
  }
  if (interrupted) {
    assert(feed(&state, wire, strlen(wire) / 2, chunk));
    assert(delivered == 0 && saved_revision == 10);
    free_sse_state(&state);
    memset(&state, 0, sizeof(state));
    state.client = &client;
  }
  if (retry) {
    reject_event = 1;
    assert(!feed(&state, wire, strlen(wire), chunk));
    assert(client.delivery_failed && saved_revision == 10 && !failed);
    free_sse_state(&state);
    memset(&state, 0, sizeof(state));
    state.client = &client;
    client.delivery_failed = reject_event = 0;
  }
  if (oversize) {
    assert(!feed(&state, wire, strlen(wire), chunk));
    assert(failed && client.delivery_failed);
    assert(delivered == 0 && saved_revision == 10 && client.local_rev == 10);
  } else {
    if (!strcmp(argv[1], "coalesced")) {
      strcat(wire, next);
      assert(feed(&state, wire, strlen(wire), chunk));
      assert(delivered == 2 && saved_revision == 21 && !client.delivery_failed);
      goto done;
    }
    assert(feed(&state, wire, strlen(wire), chunk));
    assert(delivered == 1 && saved_revision == 20 && !failed);
    assert(feed(&state, next, strlen(next), chunk));
    assert(delivered == 2 && saved_revision == 21 && !client.delivery_failed);
  }
done:
  free_sse_state(&state);
  free(wire);
  free(text);
  return 0;
}
