#import "ENILUserDefaults.h"

@implementation ENILUserDefaults

+ (NSString *)commonCSS;
{
  return @"*{margin:0;padding:0}"
          "html,body.enil-messages{background:transparent;color:#1a1a1a;"
          "padding:6px}"
          "body.enil-sticker-picker{background:#f5f5f5;color:#333;padding:0}"
          ".enil-image{display:block;-webkit-border-radius:8px}"
          ".enil-load-earlier{clear:both;margin:0 0 12px;padding:0 0 8px;"
          "text-align:center;border-bottom:1px solid #dfe3e8;"
          "font-size:11px}"
          ".enil-load-earlier a{display:inline-block;padding:5px 14px;"
          "color:#06934a;text-decoration:none;-webkit-border-radius:12px;"
          "border:1px solid #cdd3da;background:#fff;"
          "-webkit-box-shadow:0 1px 1px rgba(0,0,0,0.06)}"
          ".enil-load-earlier a:hover{background:#06C755;color:#fff;"
          "border-color:#06C755}";
}

+ (NSString *)pickerCSSWithItemSize:(NSInteger)itemSize;
{
  NSInteger thumbSize;
  NSInteger margin;
  if (itemSize < 9) return @"";
  thumbSize = itemSize - 8;
  margin = 4;
  return [[self commonCSS] stringByAppendingString:[NSString stringWithFormat:
          @"html,body.enil-sticker-picker,html,body.enil-sticon-picker{"
          "background:transparent}"
          "body.enil-sticker-picker,body.enil-sticon-picker{"
          "font-family:-apple-system,Helvetica,Arial,sans-serif;font-size:12px}"
          ".enil-picker-grid{overflow:hidden;padding:0}"
          ".enil-picker-pack{clear:both;overflow:hidden;margin:0 0 6px;"
          "padding:0 0 4px;background:transparent;"
          "border:2px solid #9aa3ad;"
          "-webkit-border-radius:8px;border-radius:8px}"
          ".enil-picker-pack-title{clear:both;cursor:pointer;"
          "padding:7px 10px;font-size:11px;font-weight:bold;"
          "color:#3a3f45;background:#fff;"
          "border-bottom:1px solid #9aa3ad;-webkit-user-select:none;"
          "-webkit-border-top-left-radius:6px;"
          "-webkit-border-top-right-radius:6px;"
          "border-top-left-radius:6px;border-top-right-radius:6px;"
          "white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
          ".enil-picker-pack-title .enil-chev{display:inline-block;"
          "width:12px;text-align:center;font-size:10px;"
          "color:#8a929c;margin-right:7px}"
          ".enil-picker-rest{display:none}"
          ".enil-picker-item{float:left;display:block;width:%ldpx;"
          "height:%ldpx;margin:%ldpx;text-align:center;vertical-align:top;"
          "text-decoration:none;color:#333;background:transparent;"
          "border:2px solid transparent;"
          "-webkit-border-radius:8px;border-radius:8px}"
          ".enil-picker-thumb{display:block;position:relative;width:%ldpx;"
          "height:%ldpx;overflow:hidden;margin:4px auto}"
          ".enil-picker-thumb img{position:absolute;left:50%%;top:50%%;"
          "vertical-align:middle}"
          ".enil-empty{padding:24px 16px;color:#777;text-align:center}",
          (long)itemSize, (long)itemSize, (long)margin,
          (long)thumbSize, (long)thumbSize]];
}

+ (NSString *)messageCSS;
{
  return [[self commonCSS] stringByAppendingString:
          @".enil-message{overflow:hidden;margin:6px 0}"
          ".enil-day{clear:both;text-align:center;margin:16px 0 12px}"
          ".enil-day span{display:inline-block;padding:3px 12px;"
          "font-size:11px;color:#fff;background:#9aa3ad;"
          "-webkit-border-radius:11px;border-radius:11px}"
          ".enil-read-marker{clear:both;position:relative;height:0;"
          "margin:10px 0 18px;border-top:2px solid #06934a;text-align:right}"
          ".enil-read-marker .enil-read-label{display:inline-block;"
          "position:relative;top:-8px;margin-right:12px;padding:0 6px;"
          "background:#fff;font-size:10px;line-height:14px;color:#000}"
          ".enil-seen-marker{clear:both;position:relative;height:0;"
          "margin:10px 0 18px;border-top:2px solid #9aa3ad;text-align:left}"
          ".enil-seen-marker .enil-seen-label{display:inline-block;"
          "position:relative;top:-8px;margin-left:12px;padding:0 6px;"
          "background:#fff;font-size:10px;line-height:14px;color:#000}"
          "body{font-family:-apple-system,Helvetica,Arial,sans-serif;"
          "font-size:14px;line-height:1.5}"
          ".enil-bubble{padding:0;-webkit-border-radius:8px;"
          "border-radius:8px;max-width:100%;word-wrap:break-word;"
          "background:#fff;color:#1a1a1a;border:2px solid #9aa3ad;"
          "-webkit-box-shadow:none}"
          ".enil-seg{padding:5px 9px}"
          ".enil-seg+.enil-seg{border-top:1px solid #9aa3ad}"
          ".enil-media-bubble{max-width:none;word-wrap:normal}"
          ".enil-message.incoming{position:relative;padding-left:36px}"
          ".enil-message.incoming .enil-bubble{float:left}"
          ".enil-avatar{position:absolute;left:0;top:0;width:28px;"
          "height:28px;-webkit-border-radius:50%;border-radius:50%;"
          "color:#fff;font-size:13px;font-weight:bold;text-align:center;"
          "line-height:28px;overflow:hidden}"
          "img.enil-avatar{display:block;object-fit:cover;"
          "border:1px solid #dfe3e8}"
          ".enil-message.outgoing .enil-bubble{float:right;"
          "background:#d8f5e3;color:#1a1a1a;border-color:#06934a}"
          ".enil-message.outgoing .enil-media-bubble,"
          ".enil-message.incoming .enil-media-bubble{"
          "background:transparent;border:none}"
          ".enil-media-bubble .enil-seg{padding:0}"
          ".enil-sender{font-size:12px;color:#06934a;font-weight:bold;"
          "margin-bottom:2px}"
          ".enil-time{font-size:10px;color:#454c54;margin-top:2px}"
          ".enil-message.outgoing .enil-bubble a{color:#06934a}"
          ".enil-message.outgoing .enil-time{text-align:right}"
          /* Short outgoing text in a date-wide bubble: shrink-wrap the
           * text block (inline-block) and right-align it so it hugs the
           * right edge under the date, while the words inside stay
           * left-aligned. Empty space falls to the left, which reads
           * naturally for a right-side bubble. */
          ".enil-message.outgoing .enil-seg{text-align:right}"
          ".enil-message.outgoing .enil-text{display:inline-block;"
          "text-align:left}"
          ".enil-text{line-height:1.5}"
          ".enil-text .enil-sticon-inline{display:inline-block;"
          "vertical-align:text-bottom;margin:0 1px}"
          ".enil-text .enil-sticon-missing{display:inline-block;"
          "vertical-align:text-bottom;font-size:11px;line-height:18px;"
          "height:18px;padding:0 4px;border:1px solid #ccc;"
          "-webkit-border-radius:10px;color:#777}"
          ".enil-media,.enil-sticker{line-height:0}"
          ".enil-media img,.enil-sticker img{display:block;"
          "-webkit-border-radius:8px}"
          ".enil-placeholder{font-style:italic;color:#888}"
          ".enil-send-pending,.enil-send-error{display:none}"
          ".enil-send-pending .fa,.enil-send-error .fa{font-size:9px}"
          ".enil-seg.pending .enil-text{color:#9bbfa9}"
          ".enil-seg.pending .enil-send-pending{display:inline;"
          "color:#9aa3ad}"
          ".enil-seg.pending .enil-sticker img,"
          ".enil-seg.pending .enil-media img{opacity:0.4}"
          ".enil-seg.error .enil-sticker img,"
          ".enil-seg.error .enil-media img{opacity:0.6}"
          ".enil-seg.error .enil-text{color:#a12020}"
          ".enil-seg.error .enil-send-error{display:inline;"
          "color:#c0392b}"
          ".enil-seg.deleted .enil-text{text-decoration:line-through;"
          "color:#9aa3ad}"
          ".enil-seg.deleted .enil-sticker img,"
          ".enil-seg.deleted .enil-media img{opacity:0.35}"
          ".enil-seg.deleted .enil-text:after,"
          ".enil-seg.deleted .enil-placeholder:after{"
          "content:' \\00b7 unsent';text-decoration:none;font-style:italic;"
          "font-size:10px;color:#a0a7b0}"
          ".enil-reactions{margin-top:3px;line-height:1}"
          ".enil-message.outgoing .enil-reactions{text-align:right}"
          ".rxn{display:inline-block;margin:4px 5px 0 0;"
          "vertical-align:middle}"
          ".rxn-badge{display:inline-block;width:18px;height:18px;"
          "line-height:18px;text-align:center;background:#fff;"
          "border:1px solid #dfe3e8;"
          "-webkit-border-radius:50%;border-radius:50%}"
          ".rxn-badge .fa{font-size:10px;vertical-align:1px;color:#8a929c}"
          ".rxn-count{margin-left:3px;font-size:11px;color:#5b636e;"
          "font-weight:bold;vertical-align:middle}"
          ".rxn-nice .fa{color:#f5b400}"
          ".rxn-love .fa{color:#e0245e}"
          ".rxn-fun .fa{color:#f5a623}"
          ".rxn-amazing .fa{color:#9b59b6}"
          ".rxn-sad .fa{color:#3498db}"
          ".rxn-omg .fa{color:#e67e22}"];
}

+ (NSString *)emptyMessageCSS;
{
  return @"body{font-family:-apple-system,Helvetica,Arial,sans-serif;color:#999;"
          "display:table;width:100%;height:100%;margin:0;text-align:center}"
          "p{display:table-cell;vertical-align:middle}";
}

@end
