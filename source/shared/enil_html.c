/* ============================================================================
 * HTML/JS generation for the WebView chat renderer — builds the chat document
 * and emits appendMessage/updateMessage JS for incremental updates.
 * ==========================================================================*/

#include "enil_html.h"
#include "enil_cocoa_date.h"
#include "enil_render_scale.h"
#include "enil_strbuf.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* --- Static page parts --- */
/* All CSS now comes from ENILUserDefaults (commonCSS prepended by each
 * getter) and is passed in as extra_css — the renderer bakes in none. */

/* Tiger-safe (no querySelectorAll / getElementsByClassName / closures):
   a pack header click promotes that pack's deferred <img data-src> to a
   live src and reveals the hidden overflow. Re-click collapses it. */
static const char kStickerPickerScript[] =
  "<script>"
  "function enilExpand(n){"
  "var r=document.getElementById('enil-rest-'+n);"
  "if(!r)return;"
  "var c=document.getElementById('enil-chev-'+n);"
  "if(r.style.display!='inline'){"
  "r.style.display='inline';"
  "if(c)c.innerHTML='&#xF078;';"
  "var g=r.getElementsByTagName('img');"
  "for(var i=0;i<g.length;i++){"
  "var d=g[i].getAttribute('data-src');"
  "if(d){g[i].src=d;g[i].removeAttribute('data-src');}"
  "}"
  "}else{r.style.display='none';if(c)c.innerHTML='&#xF054;';}"
  "}"
  "</script>";

/* Absolute path to the bundled Fonts/ dir; set once via
 * enil_html_set_font_dir before any render. NULL → @font-face omitted. */
static char *g_font_dir = NULL;

void enil_html_set_font_dir(const char *abs_dir) {
  free(g_font_dir);
  g_font_dir = NULL;
  if (abs_dir && *abs_dir) {
    size_t n = strlen(abs_dir);
    g_font_dir = (char *)malloc(n + 1);
    if (g_font_dir) memcpy(g_font_dir, abs_dir, n + 1);
  }
}

/* Emit the three Font Awesome @font-face blocks + base .fa rules. No-op when
 * the font dir is unset. Emitted before extra_css so user/common CSS can
 * still override sizing/color. */
static void append_font_face(ENILStrBuf *b) {
  static const struct { const char *family, *file; } faces[] = {
    { "FA7S", "FA7-Solid-900.otf"   },
    { "FA7R", "FA7-Regular-400.otf" },
    { "FA7B", "FA7-Brands-400.otf"  }
  };
  size_t i;
  if (!g_font_dir) return;
  for (i = 0; i < sizeof(faces) / sizeof(faces[0]); i++) {
    ESB_LIT(b, "@font-face{font-family:'");
    esb_append(b, faces[i].family, strlen(faces[i].family));
    ESB_LIT(b, "';src:url('file://");
    esb_url_path(b, g_font_dir);
    ESB_LIT(b, "/");
    esb_append(b, faces[i].file, strlen(faces[i].file));
    ESB_LIT(b, "') format('opentype');}");
  }
  ESB_LIT(b,
    ".fa{font-family:'FA7S';font-style:normal;font-weight:normal;"
    "display:inline-block;line-height:1;}"
    ".fa-regular{font-family:'FA7R';}"
    ".fa-brands{font-family:'FA7B';}");
}

/* Emit `<i class="fa[ fa-regular]">&#xNNNN;</i>` for a FA glyph.
 * style: 0=solid (default .fa → FA7S), 1=regular (adds .fa-regular → FA7R). */
static void append_icon(ENILStrBuf *b, unsigned codepoint, int style) {
  char ent[16];
  int n;
  ESB_LIT(b, "<i class=\"fa");
  if (style == 1) ESB_LIT(b, " fa-regular");
  ESB_LIT(b, "\">");
  n = snprintf(ent, sizeof(ent), "&#x%X;", codepoint);
  if (n > 0 && (size_t)n < sizeof(ent)) esb_append(b, ent, (size_t)n);
  ESB_LIT(b, "</i>");
}

unsigned enil_reaction_glyph(int predefined_type) {
  switch (predefined_type) {
    case ENIL_REACTION_NICE:    return ENIL_REACTION_GLYPH_NICE;
    case ENIL_REACTION_LOVE:    return ENIL_REACTION_GLYPH_LOVE;
    case ENIL_REACTION_FUN:     return ENIL_REACTION_GLYPH_FUN;
    case ENIL_REACTION_AMAZING: return ENIL_REACTION_GLYPH_AMAZING;
    case ENIL_REACTION_SAD:     return ENIL_REACTION_GLYPH_SAD;
    case ENIL_REACTION_OMG:     return ENIL_REACTION_GLYPH_OMG;
    default:                    return 0; /* paid / unknown → skip */
  }
}

/* base_href: emitted as <base href="..."> right after <meta charset>, before
 * <style>. NULL or empty means no <base> tag — pickers don't need one because
 * they live at enilDir root, where the document URL already matches the
 * relative paths their HTML uses. Chat pages live under enilDir/chats/, so
 * they pass "../" to push relative-path resolution back to enilDir. */
static void append_head(ENILStrBuf *b, const char *body_class,
                        const char *base_href, const char *extra_css) {
  ESB_LIT(b, "<!DOCTYPE html><html><head><meta charset=\"utf-8\">");
  ESB_LIT(b, ENIL_HTML_ZOOM_SCRIPT);
  if (base_href && base_href[0]) {
    ESB_LIT(b, "<base href=\"");
    esb_html(b, base_href);
    ESB_LIT(b, "\">");
  }
  ESB_LIT(b, "<style>");
  append_font_face(b);
  if (extra_css) esb_append(b, extra_css, strlen(extra_css));
  ESB_LIT(b, "</style></head><body class=\"");
  esb_html(b, body_class);
  ESB_LIT(b, "\">");
}

static const char kFoot[] =
  "<script>"
  /* "Stick to bottom" test: is the viewport already at (or within ~120px of)
     the bottom? The page is body-scrolled, so measure off document.body — the
     same scrollHeight/pageYOffset the append and prepend paths already use. A
     short conversation (content shorter than the viewport) yields <=0 and so
     counts as at-bottom, which means it always sticks. Tiger WebKit predates
     scrollingElement, so read window.innerHeight with a
     documentElement.clientHeight fallback. */
  "function enilNearBottom(){"
    "var b=document.body;"
    "var vh=(window.innerHeight||document.documentElement.clientHeight||0);"
    "var st=(window.pageYOffset||b.scrollTop||0);"
    "return (b.scrollHeight-st-vh)<=120"
  "}"
  /* Tiger WebKit (Safari 2.0) only builds the child tree from innerHTML when
     the scratch element is already in the document — set it on an attached
     div, then reparent the parsed node into place. `force` (1 for our own
     outgoing sends) always pins to the bottom; otherwise we only auto-scroll
     when the reader is already near the bottom, so an incoming push never
     yanks someone who has scrolled up to read history. Sample that decision
     BEFORE the insert grows scrollHeight. */
  "function appendMessage(h,force){"
    "var b=document.body;"
    "var stick=force||enilNearBottom();"
    "var d=document.createElement('div');"
    "b.appendChild(d);"
    "d.innerHTML=h;"
    "var el=d.firstChild;"
    "if(el)b.appendChild(el);"
    "b.removeChild(d);"
    "if(stick)window.scrollTo(0,b.scrollHeight)"
  "}"
  "function updateMessage(id,h,force){"
    "var el=document.getElementById('msg-'+id);"
    "if(!el)return;"
    "var b=document.body;"
    "var stick=force||enilNearBottom();"
    "var d=document.createElement('div');"
    "b.appendChild(d);"
    "d.innerHTML=h;"
    "var nw=d.firstChild;"
    "if(nw)el.parentNode.replaceChild(nw,el);"
    "b.removeChild(d);"
    "if(stick)window.scrollTo(0,b.scrollHeight)"
  "}"
  "function failMessage(id){"
    "var el=document.getElementById('msg-'+id);"
    "if(!el)return;"
    "el.className=el.className.replace(/\\bpending\\b/,'');"
    "el.className+=' error'"
  "}"
  /* Replace (or clear) the reaction pill row on an existing bubble without a
     full reload — used when an SSE reaction op updates messages_v2. h is the
     new .enil-reactions block, or "" to just remove it. */
  "function updateReactions(id,h){"
    "var el=document.getElementById('msg-'+id);"
    "if(!el)return;"
    "var i,c=el.childNodes;"
    "for(i=0;i<c.length;i++){"
      "if(c[i].className&&c[i].className.indexOf('enil-reactions')>=0){"
        "el.removeChild(c[i]);break"
      "}"
    "}"
    "if(!h)return;"
    "var b=document.body;"
    "var d=document.createElement('div');"
    "b.appendChild(d);"
    "d.innerHTML=h;"
    "var nw=d.firstChild;"
    "if(nw)el.appendChild(nw);"
    "b.removeChild(d)"
  "}"
  /* Move (or clear) the "Read up to here" marker without a full reload.
     id="" clears the marker; an id places it immediately after the row that
     contains msg-<id>. If the target segment has later siblings inside the
     same bubble (the bubble groups several messages together and the peer
     has only read the first few), split the bubble in place: leftover
     segments move into a freshly-cloned sibling row, and the marker sits
     between the two. Walks parentNodes by class-substring lookup (no
     classList in Tiger WebKit) and uses the same scratch-div pattern as
     appendMessage so innerHTML parses against an attached element. */
  "function setReadMarker(id){"
    /* Same stick-to-bottom rule as appendMessage: placing the marker grows
       scrollHeight, so pin to the bottom only when the reader was already
       near it. Sample BEFORE the remove/insert churn below. */
    "var stick=enilNearBottom();"
    "var old=document.getElementById('enil-read-marker');"
    "if(old&&old.parentNode)old.parentNode.removeChild(old);"
    "if(!id)return;"
    "var seg=document.getElementById('msg-'+id);"
    "if(!seg)return;"
    "if(seg.nextSibling){"
      "var bub=seg.parentNode;"
      "var oRow=bub.parentNode;"
      "var nRow=document.createElement('div');"
      "nRow.className=oRow.className;"
      "var nBub=document.createElement('div');"
      "nBub.className=bub.className;"
      "nRow.appendChild(nBub);"
      "var nx;"
      "while((nx=seg.nextSibling))nBub.appendChild(nx);"
      "if((' '+seg.className+' ').indexOf(' seg-last ')<0)"
        "seg.className+=' seg-last';"
      "oRow.parentNode.insertBefore(nRow,oRow.nextSibling);"
    "}"
    "var row=seg.parentNode;"
    "while(row&&(!row.className||row.className.indexOf('enil-message')<0))"
      "row=row.parentNode;"
    "if(!row||!row.parentNode)return;"
    "var b=document.body;"
    "var d=document.createElement('div');"
    "b.appendChild(d);"
    "d.innerHTML='<div id=\"enil-read-marker\" class=\"enil-read-marker\">"
                 "<span class=\"enil-read-label\">Read</span></div>';"
    "var nw=d.firstChild;"
    "if(nw)row.parentNode.insertBefore(nw,row.nextSibling);"
    "b.removeChild(d);"
    "if(stick)window.scrollTo(0,b.scrollHeight)"
  "}"
  /* Self read-position marker — the gray "Seen" sibling of setReadMarker.
     Byte-for-byte the same split-and-insert logic, only the element id,
     class and label differ, so it coexists with the green Read marker and
     the Mark-as-Seen push can move it without touching Read. */
  "function setSeenMarker(id){"
    /* Stick-to-bottom mirror of setReadMarker for the gray self marker. */
    "var stick=enilNearBottom();"
    "var old=document.getElementById('enil-seen-marker');"
    "if(old&&old.parentNode)old.parentNode.removeChild(old);"
    "if(!id)return;"
    "var seg=document.getElementById('msg-'+id);"
    "if(!seg)return;"
    "if(seg.nextSibling){"
      "var bub=seg.parentNode;"
      "var oRow=bub.parentNode;"
      "var nRow=document.createElement('div');"
      "nRow.className=oRow.className;"
      "var nBub=document.createElement('div');"
      "nBub.className=bub.className;"
      "nRow.appendChild(nBub);"
      "var nx;"
      "while((nx=seg.nextSibling))nBub.appendChild(nx);"
      "if((' '+seg.className+' ').indexOf(' seg-last ')<0)"
        "seg.className+=' seg-last';"
      "oRow.parentNode.insertBefore(nRow,oRow.nextSibling);"
    "}"
    "var row=seg.parentNode;"
    "while(row&&(!row.className||row.className.indexOf('enil-message')<0))"
      "row=row.parentNode;"
    "if(!row||!row.parentNode)return;"
    "var b=document.body;"
    "var d=document.createElement('div');"
    "b.appendChild(d);"
    "d.innerHTML='<div id=\"enil-seen-marker\" class=\"enil-seen-marker\">"
                 "<span class=\"enil-seen-label\">Seen</span></div>';"
    "var nw=d.firstChild;"
    "if(nw)row.parentNode.insertBefore(nw,row.nextSibling);"
    "b.removeChild(d);"
    "if(stick)window.scrollTo(0,b.scrollHeight)"
  "}"
  /* Group variant of setReadMarker — keyed by reader_mid so multiple
     readers can be tracked independently. Labels are appended directly
     as children of .enil-read-marker; each one is an .enil-read-label
     identical to the 1:1 marker's "Read" label. Removes any existing
     label for this reader (it may live on another marker if the reader
     has advanced); if the departing marker becomes empty its row is
     removed. Empty msgId clears the label. Otherwise finds or creates
     the marker at msg-<msgId> and appends a fresh label. Avoids
     classList/dataset/CSS3 selectors — Tiger WebKit predates all three. */
  "function setReadMarkerForReader(rmid,name,msgId){"
    /* Stick-to-bottom rule for the group read-receipt pills. Sample up front;
       `b` is only in scope inside the create-marker branch, so the final scroll
       reads document.body directly. */
    "var stick=enilNearBottom();"
    "var i,el,old=null,markers=[];"
    "var divs=document.getElementsByTagName('div');"
    "for(i=0;i<divs.length;i++){"
      "el=divs[i];"
      "if(el.className&&el.className.indexOf('enil-read-marker')>=0)"
        "markers[markers.length]=el;"
    "}"
    "var spans=document.getElementsByTagName('span');"
    "for(i=0;i<spans.length;i++){"
      "if(spans[i].getAttribute('data-reader-mid')===rmid){"
        "old=spans[i];break;"
      "}"
    "}"
    "if(old){"
      "var mk=old.parentNode;"
      "mk.removeChild(old);"
      "if(!mk.firstChild&&mk.parentNode)mk.parentNode.removeChild(mk);"
    "}"
    "if(!msgId)return;"
    /* Try to coalesce into an existing marker for this msgId. */
    "var target=null;"
    "for(i=0;i<markers.length;i++){"
      "if(markers[i].getAttribute('data-msg-id')===msgId&&"
         "markers[i].parentNode){target=markers[i];break;}"
    "}"
    "if(!target){"
      /* Anchor under the row holding msg-<id>. Split the bubble if the
         segment isn't already last in its parent — same logic as
         setReadMarker so the marker sits between read and unread. */
      "var seg=document.getElementById('msg-'+msgId);"
      "if(!seg)return;"
      "if(seg.nextSibling){"
        "var bub=seg.parentNode;"
        "var oRow=bub.parentNode;"
        "var nRow=document.createElement('div');"
        "nRow.className=oRow.className;"
        "var nBub=document.createElement('div');"
        "nBub.className=bub.className;"
        "nRow.appendChild(nBub);"
        "var nx;"
        "while((nx=seg.nextSibling))nBub.appendChild(nx);"
        "if((' '+seg.className+' ').indexOf(' seg-last ')<0)"
          "seg.className+=' seg-last';"
        "oRow.parentNode.insertBefore(nRow,oRow.nextSibling);"
      "}"
      "var row=seg.parentNode;"
      "while(row&&(!row.className||row.className.indexOf('enil-message')<0))"
        "row=row.parentNode;"
      "if(!row||!row.parentNode)return;"
      "var b=document.body;"
      "var d=document.createElement('div');"
      "b.appendChild(d);"
      "d.innerHTML='<div class=\"enil-read-marker\" data-msg-id=\"'+msgId+"
                   "'\"></div>';"
      "target=d.firstChild;"
      "row.parentNode.insertBefore(target,row.nextSibling);"
      "b.removeChild(d);"
    "}"
    /* Append the named label directly to the marker. */
    "var lbl=document.createElement('span');"
    "lbl.className='enil-read-label';"
    "lbl.setAttribute('data-reader-mid',rmid);"
    "lbl.appendChild(document.createTextNode(name));"
    "target.appendChild(lbl);"
    "if(stick)window.scrollTo(0,document.body.scrollHeight)"
  "}"
  /* Mark an existing bubble as unsent in place (SSE op 64/65) — adds the
     `deleted` class so CSS applies the strike/fade; text stays visible. */
  "function markDeleted(id){"
    "var el=document.getElementById('msg-'+id);"
    "if(el&&(' '+el.className+' ').indexOf(' deleted ')<0)"
      "el.className+=' deleted'"
  "}"
  /* Insert older messages above the existing ones while keeping the user's
     current view fixed: re-anchor scrollTop by the height we just added. */
  "function prependMessages(a,more){"
    "var b=document.body;"
    "var btn=document.getElementById('load-earlier');"
    "var ref=btn?btn.nextSibling:b.firstChild;"
    "var h0=b.scrollHeight;"
    "var y0=(window.pageYOffset||b.scrollTop||0);"
    "var d=document.createElement('div');"
    "b.appendChild(d);"
    "for(var i=0;i<a.length;i++){"
      "d.innerHTML=a[i];"
      "var el=d.firstChild;"
      "if(el)b.insertBefore(el,ref)"
    "}"
    "b.removeChild(d);"
    "if(!more&&btn&&btn.parentNode)btn.parentNode.removeChild(btn);"
    "window.scrollTo(0,y0+(b.scrollHeight-h0))"
  "}"
  "window.scrollTo(0,document.body.scrollHeight)"
  "</script></body></html>";

static const char *content_placeholder(int ct) {
  switch (ct) {
    case 1:  return "[Image]";
    case 2:  return "[Video]";
    case 3:  return "[Audio]";
    case 7:  return "[Sticker]";
    case 8:  return "[Location]";
    case 14: return "[File]";
    default: return NULL;
  }
}

/* Emit the reaction pill row from messages_v2.reactions_json. Groups by
 * predefinedReactionType (2..7), counts duplicates, renders each as an
 * FA7 Solid glyph via append_icon. Paid reactions
 * (productId/emojiId, no predefinedReactionType) are skipped entirely. */
/* Per-reaction CSS class so each reaction glyph gets its semantic color. */
static const char *rxn_class(int t) {
  switch (t) {
    case ENIL_REACTION_NICE:    return "rxn-nice";
    case ENIL_REACTION_LOVE:    return "rxn-love";
    case ENIL_REACTION_FUN:     return "rxn-fun";
    case ENIL_REACTION_AMAZING: return "rxn-amazing";
    case ENIL_REACTION_SAD:     return "rxn-sad";
    case ENIL_REACTION_OMG:     return "rxn-omg";
    default:                    return "";
  }
}

static void append_reactions(ENILStrBuf *b, const char *reactions_json) {
  cJSON *arr, *it;
  int counts[ENIL_REACTION_OMG + 1];
  int t, any = 0;

  if (!reactions_json || !reactions_json[0]) return;
  arr = cJSON_Parse(reactions_json);
  if (!cJSON_IsArray(arr)) { cJSON_Delete(arr); return; }

  memset(counts, 0, sizeof(counts));
  for (it = arr->child; it; it = it->next) {
    cJSON *pt = cJSON_GetObjectItem(it, "predefinedReactionType");
    if (!cJSON_IsNumber(pt)) continue; /* paid / unknown → skip */
    t = (int)pt->valuedouble;
    if (t >= ENIL_REACTION_NICE && t <= ENIL_REACTION_OMG) {
      counts[t]++;
      any = 1;
    }
  }

  if (any) {
    ESB_LIT(b, "<div class=\"enil-reactions\">");
    for (t = ENIL_REACTION_NICE; t <= ENIL_REACTION_OMG; t++) {
      unsigned g;
      const char *cls;
      if (counts[t] <= 0) continue;
      g = enil_reaction_glyph(t);
      if (!g) continue;
      cls = rxn_class(t);
      ESB_LIT(b, "<span class=\"rxn ");
      esb_append(b, cls, strlen(cls));
      ESB_LIT(b, "\"><span class=\"rxn-badge\">");
      append_icon(b, g, 0);
      ESB_LIT(b, "</span>");
      if (counts[t] > 1) {
        char num[32];
        int n = snprintf(num, sizeof(num),
                         "<span class=\"rxn-count\">%d</span>", counts[t]);
        if (n > 0 && (size_t)n < sizeof(num)) esb_append(b, num, (size_t)n);
      }
      ESB_LIT(b, "</span>");
    }
    ESB_LIT(b, "</div>");
  }
  cJSON_Delete(arr);
}

/* True when two adjacent messages belong to the same visual run: same
 * direction, same sender, same calendar day, within a 5-minute window.
 * Drives sender-name / timestamp / tail collapsing. */
static int same_calendar_day(long long a_ms, long long b_ms) {
  time_t ta = (time_t)(a_ms / 1000), tb = (time_t)(b_ms / 1000);
  struct tm la, lb, *p;
  p = localtime(&ta); if (!p) return 0; la = *p;
  p = localtime(&tb); if (!p) return 0; lb = *p;
  return la.tm_year == lb.tm_year && la.tm_yday == lb.tm_yday;
}

static int msg_same_run(const ENILHTMLMessage *a, const ENILHTMLMessage *b) {
  long long d;
  const char *na, *nb;
  if (!a || !b || a->is_outgoing != b->is_outgoing) return 0;
  na = a->sender_name ? a->sender_name : "";
  nb = b->sender_name ? b->sender_name : "";
  if (strcmp(na, nb) != 0) return 0;
  d = b->created_at - a->created_at;
  if (d < 0) d = -d;
  if (d > 300000LL) return 0;
  return same_calendar_day(a->created_at, b->created_at);
}

/* "Today" / "Yesterday" / "Mon DD, YYYY" for the day-divider chip. */
static void day_label(long long ms, char *buf, size_t n) {
  time_t t = (time_t)(ms / 1000), now = time(NULL), yest;
  struct tm lt, cmp, *p;
  if (!n) return;
  yest = now - 86400;
  p = localtime(&t); if (!p) { buf[0] = '\0'; return; }
  lt = *p;
  p = localtime(&now);
  if (p) { cmp = *p;
    if (cmp.tm_year == lt.tm_year && cmp.tm_yday == lt.tm_yday) {
      strncpy(buf, "Today", n); buf[n - 1] = '\0'; return; } }
  p = localtime(&yest);
  if (p) { cmp = *p;
    if (cmp.tm_year == lt.tm_year && cmp.tm_yday == lt.tm_yday) {
      strncpy(buf, "Yesterday", n); buf[n - 1] = '\0'; return; } }
  if (!strftime(buf, n, "%b %e, %Y", &lt)) buf[0] = '\0';
}

/* Centered translucent day-divider chip — emitted by the page loop on each
 * calendar-day boundary. The canvas is transparent (NSWindow shows through),
 * so a dark translucent pill reads on any backdrop. */
static void append_day_divider(ENILStrBuf *b, long long created_at) {
  char lbl[64];
  day_label(created_at, lbl, sizeof(lbl));
  if (!lbl[0]) return;
  ESB_LIT(b, "<div class=\"enil-day\"><span>");
  esb_html(b, lbl);
  ESB_LIT(b, "</span></div>");
}

/* FNV-1a over an arbitrary string → stable palette index. Used both for
 * avatar fallback color (hashed on the sender's display name) and for the
 * read-receipt pills in groups (hashed on the reader's mid, so a member's
 * color stays put even when they change their display name). */
static unsigned name_hash(const char *s) {
  unsigned h = 2166136261u;
  for (; s && *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
  return h;
}

/* Shared palette: 10 entries, designed to read with white text on top. The
 * .enil-read-pill-N CSS classes in messageCSS pick the matching background.
 * Keep the count in sync with kPaletteCount. */
static const char * const kAvatarPalette[] = {
  "#e57373", "#f06292", "#ba68c8", "#7986cb", "#4fc3f7",
  "#4db6ac", "#81c784", "#ffb74d", "#a1887f", "#90a4ae"
};
#define kPaletteCount ((int)(sizeof(kAvatarPalette) / sizeof(kAvatarPalette[0])))

static int palette_index_for(const char *key) {
  return (int)(name_hash(key ? key : "") % (unsigned)kPaletteCount);
}

/* Colored initial circle for the first incoming bubble of a run. Uses the
 * leading UTF-8 character of the sender name (handles multibyte JP names). */
static void append_avatar(ENILStrBuf *b, const char *name, const char *avatar) {
  const char *p = (name && name[0]) ? name : "?";
  const char *c = kAvatarPalette[palette_index_for(name ? name : "")];
  unsigned char f = (unsigned char)p[0];
  int len = 1, i;
  char ch[5];
  if (avatar && avatar[0]) {
    ESB_LIT(b, "<img class=\"enil-avatar\" src=\"");
    esb_html(b, avatar);
    ESB_LIT(b, "\" width=\"28\" height=\"28\">");
    return;
  }
  if (f >= 0xF0) len = 4; else if (f >= 0xE0) len = 3;
  else if (f >= 0xC0) len = 2;
  for (i = 0; i < len && p[i]; i++) ch[i] = p[i];
  ch[i] = '\0';
  ESB_LIT(b, "<div class=\"enil-avatar\" style=\"background:");
  esb_append(b, c, strlen(c));
  ESB_LIT(b, "\">");
  esb_html(b, ch);
  ESB_LIT(b, "</div>");
}

static int is_media_msg(const ENILHTMLMessage *m) {
  if (!m) return 0;
  if (m->content_type == 7) return 1;
  if (m->thumb_path && m->thumb_path[0]) return 1;
  if (m->media_path && m->media_path[0]) return 1;
  return 0;
}

/* A LINE system/notification event (e.g. contentType 18 "C_PI"): a non-text,
 * non-media, non-sticker content type carrying no body and no known
 * placeholder. Without this it falls through to an empty .enil-text div and,
 * when grouped with a neighbour, shows as a blank segment above the
 * .enil-seg+.enil-seg hairline. Rendered instead as a standalone
 * [System message] chip and kept out of any neighbour's bubble. */
static int is_system_msg(const ENILHTMLMessage *m) {
  if (!m) return 0;
  if (m->content_type == 0) return 0;             /* plain text */
  if (is_media_msg(m)) return 0;                  /* sticker / image / video */
  if (content_placeholder(m->content_type)) return 0; /* [Image], [File], … */
  if (m->encrypted_unresolved) return 0;          /* Encrypted Text placeholder */
  if (m->text_html && m->text_html[0]) return 0;
  if (m->text && m->text[0]) return 0;
  return 1;
}

/* cur starts a fresh bubble on sender/day/time change, or whenever either
 * side is media/sticker — media never shares a bubble with text. System
 * events stand alone for the same reason: they must not occupy a segment
 * inside a real message's bubble. */
static int run_breaks(const ENILHTMLMessage *prev, const ENILHTMLMessage *cur) {
  if (!prev) return 1;
  if (is_media_msg(prev) || is_media_msg(cur)) return 1;
  if (is_system_msg(prev) || is_system_msg(cur)) return 1;
  return !msg_same_run(prev, cur);
}

/* One message inside a (possibly multi-message) shared bubble. The bubble
 * frame is drawn by append_run; consecutive segments are separated by a
 * hairline (CSS .enil-seg + .enil-seg border-top). */
static void append_segment(ENILStrBuf *b, const ENILHTMLMessage *m,
                           int seg_first, int seg_last,
                           int show_sender, int show_time) {
  char ts[128];
  int has_time = 0;
  (void)seg_first;
  ESB_LIT(b, "<div class=\"enil-seg");
  if (seg_last) ESB_LIT(b, " seg-last");
  if (m->message_id && strncmp(m->message_id, "local-", 6) == 0)
    ESB_LIT(b, " pending");
  if (m->is_deleted) ESB_LIT(b, " deleted");
  ESB_LIT(b, "\" id=\"msg-");
  esb_html(b, m->message_id);
  ESB_LIT(b, "\">");

  if (show_sender && m->sender_name && m->sender_name[0]) {
    ESB_LIT(b, "<div class=\"enil-sender\">");
    esb_html(b, m->sender_name);
    ESB_LIT(b, "</div>");
  }

  if (m->content_type == 7) {
    if (m->sticker_path && m->sticker_path[0]) {
      char dim[96];
      ESB_LIT(b, "<div class=\"enil-sticker\"><img src=\"");
      esb_html(b, m->sticker_path);
      ESB_LIT(b, "\" class=\"enil-image enil-sticker-image\"");
      if (m->sticker_width > 0 && m->sticker_height > 0) {
        /* Render at 1/ENIL_STICKER_SCALE_DIV of the bitmap size so a 2x
         * (Retina) screen maps one source pixel to one device pixel — crisp.
         * Matches the media thumbnail branch below. The source PNG is
         * untouched; only the display box shrinks, so 1x screens downsample
         * cleanly. */
        int w = (int)(m->sticker_width  / (double)ENIL_STICKER_SCALE_DIV + 0.5);
        int h = (int)(m->sticker_height / (double)ENIL_STICKER_SCALE_DIV + 0.5);
        snprintf(dim, sizeof(dim),
                 " width=\"%d\" height=\"%d\" style=\"width:%dpx;height:%dpx\"",
                 w, h, w, h);
        esb_append(b, dim, strlen(dim));
      } else {
        /* Dimensions unknown: let the intrinsic bitmap size stand, but scale
         * it down for the same Retina crispness. WebKit `zoom` scales the
         * rendered box (layout included) by intrinsic size — CSS percentages
         * can't, they resolve against the containing block, not the image. */
        snprintf(dim, sizeof(dim), " style=\"zoom:%g\"",
                 1.0 / (double)ENIL_STICKER_SCALE_DIV);
        esb_append(b, dim, strlen(dim));
      }
      ESB_LIT(b, "></div>");
    } else {
      ESB_LIT(b, "<div class=\"enil-placeholder\">[Sticker]</div>");
    }
  } else if (m->thumb_path && m->thumb_path[0]) {
    char dim[96];
    int w = (m->thumb_width  + 1) / 2;
    int h = (m->thumb_height + 1) / 2;
    ESB_LIT(b, "<div class=\"enil-media\">");
    if (m->media_path && m->media_path[0]) {
      ESB_LIT(b, "<a href=\"enil-media://");
      esb_html(b, m->media_path);
      ESB_LIT(b, "\">");
    }
    ESB_LIT(b, "<img src=\"");
    esb_html(b, m->thumb_path);
    ESB_LIT(b, "\"");
    if (w > 0 && h > 0) {
      snprintf(dim, sizeof(dim),
               " width=\"%d\" height=\"%d\" style=\"width:%dpx;height:%dpx\"",
               w, h, w, h);
      esb_append(b, dim, strlen(dim));
    }
    ESB_LIT(b, ">");
    if (m->media_path && m->media_path[0]) ESB_LIT(b, "</a>");
    ESB_LIT(b, "</div>");
  } else if (m->media_path && m->media_path[0]) {
    /* thumb not yet generated — show placeholder, full image available */
    ESB_LIT(b, "<div class=\"enil-placeholder\"><a href=\"enil-media://");
    esb_html(b, m->media_path);
    ESB_LIT(b, "\">[Image]</a></div>");
  } else {
    const char *ph = content_placeholder(m->content_type);
    if (ph) {
      ESB_LIT(b, "<div class=\"enil-placeholder\">");
      esb_append(b, ph, strlen(ph));
      ESB_LIT(b, "</div>");
    } else if (m->encrypted_unresolved) {
      /* A text message we have ciphertext but no plaintext for (decrypt
       * pending or failed). The bubble would otherwise be empty. Use a
       * short fixed label so it's easy to localise; it also flags that
       * something is wrong, since this should be rare. */
      ESB_LIT(b, "<div class=\"enil-placeholder\">Encrypted Text</div>");
    } else if (is_system_msg(m)) {
      /* LINE system/notification event with no displayable body — a generic
       * chip so it never collapses to an empty segment (see is_system_msg). */
      ESB_LIT(b, "<div class=\"enil-placeholder\">[System message]</div>");
    } else {
      ESB_LIT(b, "<div class=\"enil-text\">");
      if (m->text_html && m->text_html[0]) esb_append(b, m->text_html, strlen(m->text_html));
      else esb_html(b, m->text ? m->text : "");
      ESB_LIT(b, "</div>");
    }
  }

  if (show_time) {
    time_t t = (time_t)(m->created_at / 1000);
    struct tm *lt = localtime(&t);
    has_time = (enil_format_date_string(m->created_at, ts, sizeof(ts)) == 0);
    if (!has_time && lt) strftime(ts, sizeof(ts), "%b %d %H:%M", lt);
    else if (!has_time) ts[0] = '\0';
    ESB_LIT(b, "<div class=\"enil-time\">");
    esb_html(b, ts);
    if (m->is_outgoing)
      ESB_LIT(b, "<span class=\"enil-send-pending\"> "
                "<i class=\"fa\">&#xF017;</i></span>"
                "<span class=\"enil-send-error\"> "
                "<i class=\"fa\">&#xF071;</i></span>");
    ESB_LIT(b, "</div>"); /* close .enil-time */
  }
  append_reactions(b, m->reactions_json);
  ESB_LIT(b, "</div>"); /* close .enil-seg */
}

/* Emit one run (msgs[s..e], all same sender/day, no media) as a single
 * shared bubble: wrapper + (incoming) avatar + one .enil-bubble holding
 * the per-message segments. */
static void append_run(ENILStrBuf *b, const ENILHTMLMessage *msgs, int s, int e) {
  const ENILHTMLMessage *first = &msgs[s];
  const char *direction = first->is_outgoing ? "outgoing" : "incoming";
  int outgoing = first->is_outgoing;
  int single = (s == e);
  int media = single && is_media_msg(first);
  int j;
  ESB_LIT(b, "<div class=\"enil-message ");
  esb_append(b, direction, strlen(direction));
  if (single) ESB_LIT(b, " run-solo");
  ESB_LIT(b, "\">");
  if (!outgoing)
    append_avatar(b, first->sender_name, first->avatar_path);
  ESB_LIT(b, "<div class=\"enil-bubble");
  if (media) ESB_LIT(b, " enil-media-bubble");
  ESB_LIT(b, "\">");
  for (j = s; j <= e; j++) {
    int show_sender = (!outgoing && j == s &&
                       first->sender_name && first->sender_name[0]);
    append_segment(b, &msgs[j], j == s, j == e, show_sender, j == e);
  }
  ESB_LIT(b, "</div></div>"); /* close .enil-bubble, .enil-message */
}

/* Bare segment (no run wrapper) for in-place updateMessage swaps. */
static char *build_seg_node(const ENILHTMLMessage *m) {
  ENILStrBuf b;
  int show_sender;
  if (!esb_init(&b)) return NULL;
  show_sender = (!m->is_outgoing && m->sender_name && m->sender_name[0]);
  append_segment(&b, m, 1, 1, show_sender, 1);
  return b.buf;
}

/* --- Public API --- */

/* ============================================================================
 * Render a full chat HTML document for the WebView using a caller-supplied
 * CSS string. Caller must free with enil_html_free.
 * ==========================================================================*/
/* Read-marker bucket: one entry per outgoing message that has at least one
 * reader resting on it. `reader_indices == NULL` is the 1:1 sentinel — the
 * marker renders a single "Read" label rather than a list of pills. */
typedef struct {
  int  msg_idx;
  int *reader_indices;
  int  n_readers;
} read_marker_bucket_t;

static void append_read_marker(ENILStrBuf *b, const read_marker_bucket_t *bucket,
                                const ENILHTMLMessage *msgs,
                                const ENILHTMLReader *readers) {
  int k;
  const char *mid = (bucket->msg_idx >= 0 &&
                     msgs[bucket->msg_idx].message_id)
                    ? msgs[bucket->msg_idx].message_id : NULL;
  /* 1:1 sentinel (NULL reader list) keeps the unique id and the
   * .enil-read-label wrapper: the wrapper paints a white box over the
   * green hairline so the word "Read" reads cleanly against the line.
   * Group markers don't want that white-box punch-out — each chip is
   * meant to look like one colored square sitting on the hairline —
   * so they emit chips directly as the marker's children, with no
   * wrapper at all. setReadMarker(id) (1:1) and setReadMarkerForReader
   * (group) target the two layouts independently. */
  /* Both 1:1 and group markers render their text in .enil-read-label
   * spans — a white box that punches through the hairline. The 1:1
   * sentinel emits one "Read" label; group markers emit one named
   * label per reader, distinguished only by data-reader-mid so the
   * live update path can find and move them. The 1:1 case keeps its
   * unique id so the original setReadMarker(id) JS still works. */
  if (!bucket->reader_indices) {
    ESB_LIT(b, "<div id=\"enil-read-marker\" class=\"enil-read-marker\"");
    if (mid && mid[0]) {
      ESB_LIT(b, " data-msg-id=\"");
      esb_html(b, mid);
      ESB_LIT(b, "\"");
    }
    ESB_LIT(b, "><span class=\"enil-read-label\">Read</span></div>");
    return;
  }
  ESB_LIT(b, "<div class=\"enil-read-marker\"");
  if (mid && mid[0]) {
    ESB_LIT(b, " data-msg-id=\"");
    esb_html(b, mid);
    ESB_LIT(b, "\"");
  }
  ESB_LIT(b, ">");
  for (k = 0; k < bucket->n_readers; k++) {
    const ENILHTMLReader *r = &readers[bucket->reader_indices[k]];
    ESB_LIT(b, "<span class=\"enil-read-label\" data-reader-mid=\"");
    if (r->reader_mid) esb_html(b, r->reader_mid);
    ESB_LIT(b, "\">");
    esb_html(b, (r->display_name && r->display_name[0])
                ? r->display_name
                : (r->reader_mid ? r->reader_mid : "?"));
    ESB_LIT(b, "</span>");
  }
  ESB_LIT(b, "</div>");
}

static int bucket_at(const read_marker_bucket_t *bk, int n, int msg_idx) {
  int i;
  for (i = 0; i < n; i++) if (bk[i].msg_idx == msg_idx) return i;
  return -1;
}

/* ============================================================================
 * The gray "Seen" marker — the account's OWN read position (lastSeenMessageId),
 * the self-position sibling of the green peer "Read" marker. Single line, never
 * per-reader (a group box still has only one lastSeenMessageId — ours), so it
 * carries no reader chips. Its own id/class so it coexists with enil-read-marker
 * on the same page and setSeenMarker(id) can target it without clobbering Read.
 * ==========================================================================*/
static void append_seen_marker(ENILStrBuf *b, const char *msg_id) {
  ESB_LIT(b, "<div id=\"enil-seen-marker\" class=\"enil-seen-marker\"");
  if (msg_id && msg_id[0]) {
    ESB_LIT(b, " data-msg-id=\"");
    esb_html(b, msg_id);
    ESB_LIT(b, "\"");
  }
  ESB_LIT(b, "><span class=\"enil-seen-label\">Seen</span></div>");
}

char *enil_html_page_with_css_more(const ENILHTMLMessage *msgs, int count,
                                          const char *message_css,
                                          const char *base_href, int has_more,
                                          long long peer_last_read_time,
                                          const ENILHTMLReader *group_readers,
                                          int n_group_readers,
                                          const char *seen_message_id) {
  ENILStrBuf b;
  int i, e, k;
  read_marker_bucket_t *buckets = NULL;
  int n_buckets = 0;
  int seen_idx = -1;
  if (!esb_init(&b)) return NULL;
  append_head(&b, "enil-messages", base_href, message_css);
  /* Top-of-page "Load earlier messages" button, rendered only when older
   * history exists. prependMessages() removes it once exhausted; a full
   * page reload re-emits it based on the fresh has_more. */
  if (has_more)
    ESB_LIT(&b, "<div id=\"load-earlier\" class=\"enil-load-earlier\">"
               "<a href=\"enil-action://load-more\">Load earlier messages</a></div>");
  /* Build buckets. Group path wins when both inputs are present — chips
   * always carry strictly more information than the bare "Read" label. */
  if (group_readers && n_group_readers > 0) {
    int r;
    for (r = 0; r < n_group_readers; r++) {
      const char *mid = group_readers[r].last_read_message_id;
      int idx = -1, j, bi;
      int *nri;
      if (!mid || !mid[0]) continue;
      for (j = count - 1; j >= 0; j--) {
        if (msgs[j].is_outgoing && !msgs[j].is_deleted &&
            msgs[j].message_id && strcmp(msgs[j].message_id, mid) == 0) {
          idx = j; break;
        }
      }
      if (idx < 0) continue; /* reader's last-read msg is off the page */
      bi = bucket_at(buckets, n_buckets, idx);
      if (bi < 0) {
        read_marker_bucket_t *nb = (read_marker_bucket_t *)realloc(buckets,
              (size_t)(n_buckets + 1) * sizeof(*buckets));
        if (!nb) goto cleanup;
        buckets = nb;
        buckets[n_buckets].msg_idx = idx;
        buckets[n_buckets].reader_indices = NULL;
        buckets[n_buckets].n_readers = 0;
        bi = n_buckets++;
      }
      nri = (int *)realloc(buckets[bi].reader_indices,
              (size_t)(buckets[bi].n_readers + 1) * sizeof(int));
      if (!nri) goto cleanup;
      buckets[bi].reader_indices = nri;
      buckets[bi].reader_indices[buckets[bi].n_readers++] = r;
    }
  } else if (peer_last_read_time > 0) {
    int j;
    for (j = count - 1; j >= 0; j--) {
      if (msgs[j].is_outgoing && !msgs[j].is_deleted &&
          msgs[j].created_at <= peer_last_read_time) {
        buckets = (read_marker_bucket_t *)malloc(sizeof(*buckets));
        if (!buckets) break;
        buckets[0].msg_idx = j;
        buckets[0].reader_indices = NULL; /* 1:1 sentinel → "Read" label */
        buckets[0].n_readers = 0;
        n_buckets = 1;
        break;
      }
    }
  }
  /* Resolve the account's own read position to a row index. Unlike the peer
   * "Read" path this matches by id (lastSeenMessageId is already a message id,
   * not a timestamp) and is NOT restricted to outgoing rows — our last-seen
   * message is usually an incoming one. Scan from the newest backward; the id
   * is unique so the first hit is the row. A deleted target yields no marker. */
  if (seen_message_id && seen_message_id[0]) {
    int j;
    for (j = count - 1; j >= 0; j--) {
      if (!msgs[j].is_deleted && msgs[j].message_id &&
          strcmp(msgs[j].message_id, seen_message_id) == 0) {
        seen_idx = j; break;
      }
    }
  }
  i = 0;
  while (i < count) {
    e = i;
    while (e + 1 < count && !run_breaks(&msgs[e], &msgs[e + 1])) {
      /* Stop extending the run AT any marker boundary so the bubble breaks
       * here — the marker line then sits between the last-read segment and
       * the first-unread one rather than below them both. The "Seen" anchor
       * breaks the run for the same reason. */
      if (bucket_at(buckets, n_buckets, e) >= 0 || e == seen_idx) break;
      e++;
    }
    if (i == 0 ||
        !same_calendar_day(msgs[i - 1].created_at, msgs[i].created_at))
      append_day_divider(&b, msgs[i].created_at);
    append_run(&b, msgs, i, e);
    for (k = 0; k < n_buckets; k++) {
      if (buckets[k].msg_idx >= i && buckets[k].msg_idx <= e)
        append_read_marker(&b, &buckets[k], msgs, group_readers);
    }
    if (seen_idx >= i && seen_idx <= e)
      append_seen_marker(&b, msgs[seen_idx].message_id);
    i = e + 1;
  }
cleanup:
  for (i = 0; i < n_buckets; i++) free(buckets[i].reader_indices);
  free(buckets);
  /* "Mark as Seen" now lives on the window toolbar (not an in-page footer).
   * The controller looks up `message_boxes_v2.lastDeliveredMessageId` at
   * click time — the server's authoritative high-water-mark. */
  esb_append(&b, kFoot, sizeof(kFoot) - 1);
  return b.buf;
}

/* ============================================================================
 * Render a single message bubble fragment (no <html>/<body> wrapper).
 * ==========================================================================*/
static char *enil_html_node(const ENILHTMLMessage *msg) {
  ENILStrBuf b;
  if (!esb_init(&b)) return NULL;
  append_run(&b, msg, 0, 0);
  return b.buf;
}

/* Build a JS call `name("a0","a1",…)` with each non-NULL arg JS-escaped and
 * double-quoted; a NULL arg is emitted as the bare literal `null` (no quotes),
 * which is how the optional-marker calls clear themselves. Returns a malloc'd
 * string the caller frees with enil_html_free, or NULL on OOM. Folds the
 * esb_init -> name"(" -> esb_js(arg)... -> ")" shape the one-shot enil_html_js_*
 * wrappers below all repeat. */
static char *js_call(const char *name, const char *const *args, int n) {
  ENILStrBuf b;
  int i;
  if (!esb_init(&b)) return NULL;
  esb_append(&b, name, strlen(name));
  ESB_LIT(&b, "(");
  for (i = 0; i < n; i++) {
    if (i) ESB_LIT(&b, ",");
    if (args[i]) {
      ESB_LIT(&b, "\"");
      esb_js(&b, args[i]);
      ESB_LIT(&b, "\"");
    } else {
      ESB_LIT(&b, "null");
    }
  }
  ESB_LIT(&b, ")");
  return b.buf;
}

/* ============================================================================
 * Build the appendMessage("...") JS the WebView evaluates to insert a bubble.
 * ==========================================================================*/
char *enil_html_js_append(const ENILHTMLMessage *msg) {
  ENILStrBuf b;
  char *node = enil_html_node(msg);
  if (!node) return NULL;
  if (!esb_init(&b)) { free(node); return NULL; }
  ESB_LIT(&b, "appendMessage(\"");
  esb_js(&b, node);
  /* Trailing force flag: our own outgoing sends pin to the bottom
     unconditionally; incoming bubbles defer to enilNearBottom(). ESB_LIT
     needs a literal token, so branch rather than a ternary. */
  if (msg->is_outgoing) ESB_LIT(&b, "\",1)");
  else                  ESB_LIT(&b, "\",0)");
  free(node);
  return b.buf;
}

/* ============================================================================
 * Build prependMessages([...],more) JS that inserts a page of older bubbles
 * above the existing ones (ascending order) while keeping scroll stable.
 * ==========================================================================*/
char *enil_html_js_prepend(const ENILHTMLMessage *msgs, int count,
                           int has_more) {
  ENILStrBuf b;
  int i, e, first_out = 1;
  if (count <= 0 || !msgs) return NULL;
  if (!esb_init(&b)) return NULL;
  ESB_LIT(&b, "prependMessages([");
  i = 0;
  while (i < count) {
    ENILStrBuf nb;
    e = i;
    while (e + 1 < count && !run_breaks(&msgs[e], &msgs[e + 1])) e++;
    if (!first_out) ESB_LIT(&b, ",");
    first_out = 0;
    ESB_LIT(&b, "\"");
    if (esb_init(&nb)) {
      append_run(&nb, msgs, i, e);
      if (nb.buf) { esb_js(&b, nb.buf); free(nb.buf); }
    }
    ESB_LIT(&b, "\"");
    i = e + 1;
  }
  if (has_more) ESB_LIT(&b, "],1)");
  else          ESB_LIT(&b, "],0)");
  return b.buf;
}

/* ============================================================================
 * Build the failMessage("id") JS used when a sent message fails to deliver.
 * ==========================================================================*/
char *enil_html_js_fail(const char *msg_id) {
  if (!msg_id) return NULL;
  return js_call("failMessage", &msg_id, 1);
}

/* ============================================================================
 * Build updateReactions(id, html) JS — refreshes just the reaction pill row
 * of an existing bubble. reactions_json may be NULL/empty (clears the row).
 * ==========================================================================*/
char *enil_html_js_reactions(const char *msg_id, const char *reactions_json) {
  ENILStrBuf rb;
  const char *args[2];
  char *out;
  if (!msg_id || !msg_id[0]) return NULL;
  if (!esb_init(&rb)) return NULL;
  append_reactions(&rb, reactions_json);
  args[0] = msg_id;
  args[1] = rb.buf ? rb.buf : "";
  out = js_call("updateReactions", args, 2);
  free(rb.buf);
  return out;
}

/* ============================================================================
 * Build setReadMarker(id) JS — moves the "Read up to here" hairline below
 * the row containing msg-<id>. Pass NULL/"" to clear instead of place.
 * ==========================================================================*/
char *enil_html_js_set_read_marker(const char *msg_id) {
  const char *a = (msg_id && msg_id[0]) ? msg_id : NULL;
  return js_call("setReadMarker", &a, 1);
}

/* ============================================================================
 * Build setSeenMarker(id) JS — moves (or clears, when msg_id is NULL/"") the
 * gray self "Seen" marker without a reload. Pushed by the optimistic
 * Mark-as-Seen path after enil_db_message_box_mark_seen advances the box's
 * lastSeenMessageId. Caller frees with enil_html_free.
 * ==========================================================================*/
char *enil_html_js_set_seen_marker(const char *msg_id) {
  const char *a = (msg_id && msg_id[0]) ? msg_id : NULL;
  return js_call("setSeenMarker", &a, 1);
}

/* ============================================================================
 * Build setReadMarkerForReader(rmid,name,msgId) JS — places (or moves) a
 * single named label below the row containing msg-<msg_id>, coalescing
 * into an existing marker for that message if one already exists. Empty
 * msg_id clears the label. Used by the group SSE op-55 push path.
 * ==========================================================================*/
char *enil_html_js_set_read_marker_for_reader(const char *reader_mid,
                                              const char *display_name,
                                              const char *msg_id) {
  const char *args[3];
  if (!reader_mid || !reader_mid[0]) return NULL;
  args[0] = reader_mid;
  args[1] = (display_name && display_name[0]) ? display_name : reader_mid;
  args[2] = (msg_id && msg_id[0]) ? msg_id : "";
  return js_call("setReadMarkerForReader", args, 3);
}

/* ============================================================================
 * Build markDeleted(id) JS — flags an existing bubble as unsent in place
 * (SSE op 64/65). Text stays; CSS applies the strike/fade.
 * ==========================================================================*/
char *enil_html_js_deleted(const char *msg_id) {
  if (!msg_id || !msg_id[0]) return NULL;
  return js_call("markDeleted", &msg_id, 1);
}

/* ============================================================================
 * Build the updateMessage(old_id, ...) JS used to swap a temp bubble for
 * its confirmed server-id counterpart.
 * ==========================================================================*/
char *enil_html_js_update(const char *msg_id, const ENILHTMLMessage *msg) {
  ENILStrBuf b;
  char *node = build_seg_node(msg);
  if (!node) return NULL;
  if (!esb_init(&b)) { free(node); return NULL; }
  ESB_LIT(&b, "updateMessage(\"");
  esb_js(&b, msg_id ? msg_id : "");
  ESB_LIT(&b, "\",\"");
  esb_js(&b, node);
  /* Same force flag as appendMessage: an outgoing confirm-swap pins to the
     bottom, an incoming swap defers to enilNearBottom(). */
  if (msg->is_outgoing) ESB_LIT(&b, "\",1)");
  else                  ESB_LIT(&b, "\",0)");
  free(node);
  return b.buf;
}

static int clamp_picker_image_size(int image_size, int fallback) {
  return image_size > 0 ? image_size : fallback;
}

/* Compute the on-screen w/h for a picker thumbnail: clamp the longest side to
 * image_size (or `fallback` when the caller passed 0), preserving aspect ratio,
 * with a 1px floor. Shared by the sticker and sticon link emitters. */
static void picker_thumb_dims(const ENILHTMLSticker *s, int image_size,
                              int fallback, int *out_w, int *out_h) {
  int w, h;
  image_size = clamp_picker_image_size(image_size, fallback);
  w = s->image_width;
  h = s->image_height;
  if (w <= 0 || h <= 0) { w = image_size; h = image_size; }
  else if (w > h && w > image_size) { h = (h * image_size) / w; w = image_size; }
  else if (h > image_size) { w = (w * image_size) / h; h = image_size; }
  if (w <= 0) w = 1;
  if (h <= 0) h = 1;
  *out_w = w; *out_h = h;
}

static void append_sticker_link(ENILStrBuf *b, const ENILHTMLSticker *s,
                                int image_size, int deferred) {
  char dim[160];
  int w, h;

  if (!s || !s->image_path || !s->image_path[0]) return;
  picker_thumb_dims(s, image_size, 64, &w, &h);

  /* No per-sticker tooltip — LINE has no useful per-sticker label
     (`productInfo.meta` has only id/width/height). The sticker_id
     number was confusing, so the title attribute is omitted entirely. */
  ESB_LIT(b, "<a class=\"enil-picker-item\" href=\"enil-sticker://select?package_id=");
  esb_url(b, s->package_id);
  ESB_LIT(b, "&amp;sticker_id=");
  esb_url(b, s->sticker_id);
  ESB_LIT(b, "\"><span class=\"enil-picker-thumb\"><img class=\"enil-image enil-picker-image");
  if (s->is_animated) ESB_LIT(b, " enil-animated-image");
  if (deferred) {
    ESB_LIT(b, " enil-lazy\" data-src=\"");
  } else {
    ESB_LIT(b, "\" src=\"");
  }
  esb_html(b, s->image_path);
  snprintf(dim, sizeof(dim),
           "\" width=\"%d\" height=\"%d\" style=\"width:%dpx;height:%dpx;margin-left:-%dpx;margin-top:-%dpx\" alt=\"\"",
           w, h, w, h, w / 2, h / 2);
  esb_append(b, dim, strlen(dim));
  ESB_LIT(b, "></span></a>");
}

static void append_sticon_link(ENILStrBuf *b, const ENILHTMLSticker *s,
                               int image_size, int deferred) {
  const char *title;
  char dim[160];
  int w, h;

  if (!s || !s->image_path || !s->image_path[0]) return;
  title = (s->alt_text && s->alt_text[0]) ? s->alt_text : s->sticker_id;
  picker_thumb_dims(s, image_size, 32, &w, &h);

  ESB_LIT(b, "<a class=\"enil-picker-item\" href=\"enil-sticon://select?package_id=");
  esb_url(b, s->package_id);
  ESB_LIT(b, "&amp;sticker_id=");
  esb_url(b, s->sticker_id);
  ESB_LIT(b, "&amp;alt_text=");
  esb_url(b, title);
  ESB_LIT(b, "\" title=\"");
  esb_html(b, title);
  if (deferred) {
    ESB_LIT(b, "\"><span class=\"enil-picker-thumb\"><img class=\"enil-image enil-picker-image enil-lazy\" data-src=\"");
  } else {
    ESB_LIT(b, "\"><span class=\"enil-picker-thumb\"><img class=\"enil-image enil-picker-image\" src=\"");
  }
  esb_html(b, s->image_path);
  snprintf(dim, sizeof(dim),
           "\" width=\"%d\" height=\"%d\""
           " style=\"width:%dpx;height:%dpx;margin-left:-%dpx;margin-top:-%dpx\""
           " alt=\"",
           w, h, w, h, w / 2, h / 2);
  esb_append(b, dim, strlen(dim));
  esb_html(b, title);
  ESB_LIT(b, "\"></span></a>");
}

/* ============================================================================
 * Render the sticon (inline emoji) picker page with caller-supplied CSS.
 * ==========================================================================*/
/* Open a pack: close any open rest-block and the previous pack, then emit
   the clickable header. Returns the bumped pack number. */
static int picker_open_pack(ENILStrBuf *b, const ENILHTMLSticker *s,
                            const char *pkg, int pack_no, int *rest_open,
                            int have_prev) {
  if (*rest_open) { ESB_LIT(b, "</span>"); *rest_open = 0; }
  if (have_prev) ESB_LIT(b, "</div>");
  pack_no++;
  ESB_LIT(b, "<div class=\"enil-picker-pack\">"
            "<div class=\"enil-picker-pack-title\" onclick=\"enilExpand(");
  esb_int(b, pack_no);
  /* FAChevronRight (0xF054) — collapsed; enilExpand swaps to
     FAChevronDown (0xF078) on expand. */
  ESB_LIT(b, ")\"><i class=\"fa enil-chev\" id=\"enil-chev-");
  esb_int(b, pack_no);
  ESB_LIT(b, "\">&#xF054;</i><span>");
  if (s->package_name && s->package_name[0]) esb_html(b, s->package_name);
  else esb_html(b, pkg);
  ESB_LIT(b, "</span></div>");
  return pack_no;
}

/* Open the hidden overflow span for the current pack (items past the
   preview window). An inline span keeps its floated items in the pack's
   float flow so previews + expanded items wrap as one group. */
static void picker_open_rest(ENILStrBuf *b, int pack_no, int *rest_open) {
  ESB_LIT(b, "<span class=\"enil-picker-rest\" id=\"enil-rest-");
  esb_int(b, pack_no);
  ESB_LIT(b, "\" style=\"display:none\">");
  *rest_open = 1;
}

typedef void (*PickerLinkFn)(ENILStrBuf *b, const ENILHTMLSticker *s,
                             int image_size, int deferred);

static char *picker_page_with_css(const ENILHTMLSticker *items,
                                  int count,
                                  const char *picker_css,
                                  int image_size,
                                  int preview_count,
                                  const char *body_class,
                                  const char *empty_text,
                                  PickerLinkFn append_link) {
  ENILStrBuf b;
  const char *last_package = NULL;
  int i, idx_in_pack = 0, pack_no = 0, rest_open = 0;

  if (!esb_init(&b)) return NULL;
  append_head(&b, body_class, NULL, picker_css);
  if (!items || count <= 0) {
    ESB_LIT(&b, "<div class=\"enil-empty\">");
    esb_html(&b, empty_text);
    ESB_LIT(&b, "</div></body></html>");
    return b.buf;
  }

  ESB_LIT(&b, "<div class=\"enil-picker-grid\">");
  for (i = 0; i < count; i++) {
    const ENILHTMLSticker *s = &items[i];
    const char *pkg;
    if (!s->image_path || !s->image_path[0]) continue;
    pkg = s->package_id ? s->package_id : "";
    if (!last_package || strcmp(last_package, pkg) != 0) {
      pack_no = picker_open_pack(&b, s, pkg, pack_no, &rest_open,
                                 last_package != NULL);
      idx_in_pack = 0;
      last_package = pkg;
    }
    if (idx_in_pack == preview_count) picker_open_rest(&b, pack_no, &rest_open);
    append_link(&b, s, image_size, idx_in_pack >= preview_count);
    idx_in_pack++;
  }
  if (rest_open) ESB_LIT(&b, "</span>");
  if (last_package) ESB_LIT(&b, "</div>");
  ESB_LIT(&b, "</div>");
  if (kStickerPickerScript[0])
    esb_append(&b, kStickerPickerScript, sizeof(kStickerPickerScript) - 1);
  ESB_LIT(&b, "</body></html>");
  return b.buf;
}

char *enil_html_sticon_picker_page_with_css(const ENILHTMLSticker *sticons,
                                                   int count,
                                                   const char *picker_css,
                                                   int image_size,
                                                   int preview_count) {
  return picker_page_with_css(sticons, count, picker_css, image_size,
                              preview_count, "enil-sticon-picker",
                              "No purchased emoji", append_sticon_link);
}

/* ============================================================================
 * Render the sticker picker page (grouped by package) with caller CSS.
 * ==========================================================================*/
char *enil_html_sticker_picker_page_with_css(const ENILHTMLSticker *stickers,
                                                    int count,
                                                    const char *picker_css,
                                                    int image_size,
                                                    int preview_count) {
  return picker_page_with_css(stickers, count, picker_css, image_size,
                              preview_count, "enil-sticker-picker",
                              "No purchased stickers", append_sticker_link);
}

/* ============================================================================
 * Free a string returned by any enil_html_* renderer. Safe on NULL.
 * ==========================================================================*/
void enil_html_free(char *p) {
  free(p);
}

/* ============================================================================
 * Cocoa-data convenience wrappers
 * ==========================================================================*/

#include "enil_message_format.h"
#include "enil_db.h"
#include "enil_cocoa_log.h"

#define LOG(fn, msg) enil_log("Html." fn, "%s", msg)


/* ---- JS string builders ---- */


/* ============================================================================
 * Build the appendMessage JS for an outgoing plain-text bubble identified by
 * temp_id, formatting REPLACE markers via the sqlite-resolved sticon table.
 * ==========================================================================*/
char *enil_html_js_append_text(sqlite3 *db, const char *temp_id,
                               const char *sender_name, const char *text,
                               long long created_at) {
  ENILHTMLMessage m;
  char *text_html, *js;

  if (!db || !temp_id || !text) {
    LOG("js_append_text", "NULL argument");
    return NULL;
  }
  text_html = enil_message_text_html(db, NULL, text);
  memset(&m, 0, sizeof(m));
  m.message_id   = temp_id;
  m.sender_name  = sender_name ? sender_name : "";
  m.text         = text;
  m.text_html    = text_html ? text_html : "";
  m.created_at   = created_at;
  m.content_type = 0;
  m.is_outgoing  = 1;
  js = enil_html_js_append(&m);
  enil_message_format_free(text_html);
  return js;
}

/* ============================================================================
 * Build the appendMessage JS for an outgoing inline-sticon bubble using
 * explicit (package_id, sticon_id, alt) arrays.
 * ==========================================================================*/
char *enil_html_js_append_inline_sticon(sqlite3 *db, const char *temp_id,
                                               const char *sender_name,
                                               const char *text,
                                               long long created_at,
                                               int count,
                                               const char * const *package_ids,
                                               const char * const *sticon_ids,
                                               const char * const *alt_texts) {
  ENILHTMLMessage m;
  char *text_html, *js;

  if (!db || !temp_id || !text || count <= 0 ||
      !package_ids || !sticon_ids || !alt_texts) {
    LOG("js_append_inline_sticon", "NULL argument");
    return NULL;
  }
  (void)alt_texts;
  text_html = enil_message_text_html_for_sticons(db, text, count,
                                                 package_ids, sticon_ids);
  memset(&m, 0, sizeof(m));
  m.message_id   = temp_id;
  m.sender_name  = sender_name ? sender_name : "";
  m.text         = text;
  m.text_html    = text_html ? text_html : "";
  m.created_at   = created_at;
  m.content_type = 0;
  m.is_outgoing  = 1;
  js = enil_html_js_append(&m);
  enil_message_format_free(text_html);
  return js;
}


/* ============================================================================
 * Build the appendMessage JS for an outgoing sticker bubble.
 * ==========================================================================*/
char *enil_html_js_append_sticker(sqlite3 *db, const char *temp_id,
                                  const char *package_id,
                                  const char *sticker_id,
                                  long long created_at) {
  ENILHTMLMessage m;
  char path[512];
  int sticker_w = 0, sticker_h = 0;

  if (!db || !temp_id || !package_id || !sticker_id) {
    LOG("js_append_sticker", "NULL argument");
    return NULL;
  }
  memset(&m, 0, sizeof(m));
  memset(path, 0, sizeof(path));
  m.message_id   = temp_id;
  m.sender_name  = "";
  m.created_at   = created_at;
  m.content_type = 7;
  m.is_outgoing  = 1;
  if (enil_db_get_sticker_image_info_for_package(db, package_id, sticker_id,
                                                 path, sizeof(path),
                                                 &sticker_w, &sticker_h)) {
    m.sticker_path   = path;
    m.sticker_width  = sticker_w;
    m.sticker_height = sticker_h;
  }
  return enil_html_js_append(&m);
}

/* ============================================================================
 * Build the appendMessage JS for an outgoing image bubble (optimistic insert
 * before the server returns the real message id). media_path / thumb_path are
 * relative to Application Support/ENIL/.
 * ==========================================================================*/
char *enil_html_js_append_image(const char *temp_id,
                                const char *sender_name,
                                const char *media_path,
                                const char *thumb_path,
                                int orig_width, int orig_height,
                                int thumb_width, int thumb_height,
                                long long created_at) {
  ENILHTMLMessage m;
  if (!temp_id) { LOG("js_append_image", "NULL argument"); return NULL; }
  memset(&m, 0, sizeof(m));
  m.message_id   = temp_id;
  m.sender_name  = sender_name ? sender_name : "";
  m.media_path   = media_path  ? media_path  : "";
  m.thumb_path   = thumb_path  ? thumb_path  : "";
  m.orig_width   = orig_width;
  m.orig_height  = orig_height;
  m.thumb_width  = thumb_width;
  m.thumb_height = thumb_height;
  m.created_at   = created_at;
  m.content_type = 1;
  m.is_outgoing  = 1;
  return enil_html_js_append(&m);
}
