#ifndef ENIL_HTML_H
#define ENIL_HTML_H

#include <sqlite3.h>

/* Inline <head> script: the shared CSS reads small on iOS, so bump the whole
 * page like desktop Cmd-+ BEFORE first paint (running in <head> means the
 * first layout is already scaled — no post-load reflow). UA detection keeps
 * the same markup serving both targets; macOS WebKit stays at 1.0. Emitted
 * by every <head>-producing site so empty-state and populated pages match. */
#define ENIL_HTML_ZOOM_SCRIPT \
  "<script>document.documentElement.style.zoom=" \
  "/iP(hone|ad|od)/.test(navigator.userAgent)?'1.2':'1.0';</script>"

typedef struct {
  const char *message_id;
  const char *sender_name;
  const char *text;
  const char *text_html;
  const char *media_path;
  const char *thumb_path;
  const char *sticker_path; /* relative path to sticker PNG, or NULL */
  const char *avatar_path; /* relative path to sender avatar JPG, or NULL */
  const char *reactions_json; /* messages_v2.reactions_json, or NULL */
  long long   created_at;
  int         content_type;
  int         is_outgoing;
  int         is_deleted; /* messages_v2.is_deleted — renders struck/faded */
  int         encrypted_unresolved; /* decrypt_status is PENDING or FAILED:
                               * we hold ciphertext but no plaintext (not yet
                               * attempted, or the worker couldn't decrypt it).
                               * Renders an italic gray "Encrypted Text"
                               * placeholder so a text bubble isn't empty.
                               * Media types keep their own placeholder. */
  int         sticker_width;
  int         sticker_height;
  int         orig_width;
  int         orig_height;
  int         thumb_width;
  int         thumb_height;
} ENILHTMLMessage;

/* One peer's read-receipt position for the group read-marker render path.
 * `last_read_message_id` points into the same id-space as
 * ENILHTMLMessage.message_id — the renderer locates the bucket by matching
 * the two. Mirrors enil_group_reader_t but kept HTML-local to avoid
 * pulling enil_db.h into the renderer header. */
typedef struct {
  const char *reader_mid;
  const char *display_name;
  const char *last_read_message_id;
} ENILHTMLReader;

typedef struct {
  const char *sticker_id;
  const char *package_id;
  const char *package_name;
  const char *image_path;
  const char *alt_text;
  int         image_width;
  int         image_height;
  int         is_animated;
} ENILHTMLSticker;

/* Absolute filesystem path to the bundled Font Awesome OTF directory
 * (…/ENIL.app/Contents/Resources/Fonts). Must be set once at startup, before
 * any page is rendered, so append_head can emit @font-face with an absolute
 * file:// URL — the WebView pages are rooted at the per-account enilDir, not
 * the bundle, so a relative font URL would not resolve. NULL/unset simply
 * omits the @font-face blocks. The string is copied. */
void enil_html_set_font_dir(const char *abs_dir);

/* LINE predefined reaction types — the integer stored in
 * messages_v2.reactions_json as "predefinedReactionType" (LINE TalkService
 * values). Paid reactions (productId/emojiId) are intentionally unsupported
 * and have no entry here. */
typedef enum {
  ENIL_REACTION_NICE    = 2,
  ENIL_REACTION_LOVE    = 3,
  ENIL_REACTION_FUN     = 4,
  ENIL_REACTION_AMAZING = 5,
  ENIL_REACTION_SAD     = 6,
  ENIL_REACTION_OMG     = 7
} enil_reaction_type_t;

/* FA7 Solid code points used to render each predefined reaction. These mirror
 * the matching AIFontAwesomeIcon enum values, redeclared here because
 * enil_html.c builds as plain C and cannot include AIFontAwesome.h. To change
 * the glyph used for a reaction, edit exactly one line below. */
typedef enum {
  ENIL_REACTION_GLYPH_NICE    = 0xF087, /* AIFAThumbsUp */
  ENIL_REACTION_GLYPH_LOVE    = 0xF004, /* AIFAHeart */
  ENIL_REACTION_GLYPH_FUN     = 0xF59A, /* AIFAFaceLaughBeam */
  ENIL_REACTION_GLYPH_AMAZING = 0xF5C2, /* AIFAFaceSurprise */
  ENIL_REACTION_GLYPH_SAD     = 0xF5B4, /* AIFAFaceSadTear */
  ENIL_REACTION_GLYPH_OMG     = 0xF579  /* AIFAFaceFlushed */
} enil_reaction_glyph_t;

/* Map a LINE predefined reaction type to its FA7 Solid code point. Returns 0
 * for paid/unknown/out-of-range types — callers skip those entries. */
unsigned enil_reaction_glyph(int predefined_type);

/* All return malloc'd C strings; caller must pass to enil_html_free(). */
char *enil_html_js_append(const ENILHTMLMessage *msg);
/* prependMessages([...],more) for paginated older-history loads. */
char *enil_html_js_prepend(const ENILHTMLMessage *msgs, int count,
                           int has_more);
char *enil_html_js_update(const char *msg_id, const ENILHTMLMessage *msg);
char *enil_html_js_fail(const char *msg_id);
/* updateReactions(id,…) — refresh just the reaction pill row of a bubble
 * already in the DOM. reactions_json NULL/empty clears the row. */
char *enil_html_js_reactions(const char *msg_id, const char *reactions_json);
/* markDeleted(id) — add the `deleted` class to an existing bubble in place
 * (unsend op 64/65) so the strike/fade appears without a chat reload. */
char *enil_html_js_deleted(const char *msg_id);
/* setReadMarker(id) — move (or clear, when msg_id is NULL/"") the "Read up
 * to here" marker without a full reload. Used by the op-55 push path. */
char *enil_html_js_set_read_marker(const char *msg_id);
/* setSeenMarker(id) — move (or clear, when msg_id is NULL/"") the gray self
 * "Seen" marker without a full reload. Used by the Mark-as-Seen push path. */
char *enil_html_js_set_seen_marker(const char *msg_id);
/* setReadMarkerForReader(reader_mid, name, msg_id) — group variant of
 * setReadMarker. Places (or moves) a single named label below the row
 * containing msg-<msg_id>, creating a fresh marker if none yet exists
 * at that message, and coalescing into an existing marker if another
 * reader already sits there. Pass an empty msg_id to remove the
 * reader's label. Used by the group op-55 push path. */
char *enil_html_js_set_read_marker_for_reader(const char *reader_mid,
                                              const char *display_name,
                                              const char *msg_id);
void  enil_html_free(char *p);

/* JS string builders used to append/replace a single message bubble in the
 * WebView via stringByEvaluatingJavaScriptFromString:. All return malloc'd
 * strings — pass to enil_html_free when done. */
char *enil_html_js_append_text(sqlite3 *db, const char *temp_id,
                               const char *sender_name, const char *text,
                               long long created_at);
char *enil_html_js_append_sticker(sqlite3 *db, const char *temp_id,
                                  const char *package_id,
                                  const char *sticker_id,
                                  long long created_at);
char *enil_html_js_append_image(const char *temp_id,
                                const char *sender_name,
                                const char *media_path,
                                const char *thumb_path,
                                int orig_width, int orig_height,
                                int thumb_width, int thumb_height,
                                long long created_at);

/* Full-page builders + inline-sticon append. Same pure-C string-builder family
 * as the enil_html_js_* calls above; the platform UI layer (ENILAccount.m)
 * marshals its native rows into these structs and calls these directly. All
 * return malloc'd strings — pass to enil_html_free. */
/* base_href is emitted as <base href="..."> inside <head>; pass NULL when
 * the document is being written somewhere that the HTML's relative paths
 * (sticker / media / avatar paths stored relative to enilDir) already
 * resolve correctly. Chat HTML lives under <enilDir>/chats/<chatId>.html
 * and so needs base_href="../" to push relative resolution back up to
 * enilDir. */
/* peer_last_read_time: ms watermark for the 1:1 read marker (0 disables).
 * group_readers / n_group_readers: per-member read positions for groups
 * (NULL/0 disables). The two are mutually exclusive in practice — a chat
 * is either 1:1 or group — but when both are supplied the group path
 * wins because chips carry strictly more information than the bare
 * "Read" label.
 * seen_message_id: the account's OWN lastSeenMessageId (NULL/"" disables) —
 * renders the gray "Seen" marker below that row. Independent of the peer
 * read marker (both can appear) and identical for 1:1 and group, since the
 * box carries a single self read-position regardless of chat type. */
char *enil_html_page_with_css_more(const ENILHTMLMessage *msgs, int count,
                                   const char *message_css,
                                   const char *base_href, int has_more,
                                   long long peer_last_read_time,
                                   const ENILHTMLReader *group_readers,
                                   int n_group_readers,
                                   const char *seen_message_id);

char *enil_html_sticon_picker_page_with_css(const ENILHTMLSticker *sticons,
                                            int count, const char *picker_css,
                                            int image_size, int preview_count);

char *enil_html_sticker_picker_page_with_css(const ENILHTMLSticker *stickers,
                                             int count, const char *picker_css,
                                             int image_size, int preview_count);

char *enil_html_js_append_inline_sticon(sqlite3 *db, const char *temp_id,
                                        const char *sender_name,
                                        const char *text, long long created_at,
                                        int count,
                                        const char * const *package_ids,
                                        const char * const *sticon_ids,
                                        const char * const *alt_texts);

#endif /* ENIL_HTML_H */
