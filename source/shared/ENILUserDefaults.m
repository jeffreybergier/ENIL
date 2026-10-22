#import "ENILUserDefaults.h"

@implementation ENILUserDefaults

+ (NSString *)commonCSS;
{
  return @"*{margin:0;padding:0}"
          "html{background:transparent}"
          "body.enil-messages{background:#ffffff;color:#252a31;padding:0 0 8px;overflow-anchor:none}"
          "body.enil-sticker-picker{background:#f5f5f5;color:#333;padding:0}"
          ".enil-image{display:block;border:0}"
          ".enil-load-earlier{clear:both;margin:0 0 12px;padding:0 0 8px;"
          "text-align:center;border-bottom:1px solid #dfe3e8;"
          "font-size:11px}"
          ".enil-load-earlier a{display:inline-block;padding:5px 14px;"
          "color:#46515f;text-decoration:none;-webkit-border-radius:3px;"
          "border:1px solid #cdd3da;background:#fff;"
          "-webkit-box-shadow:none}"
          ".enil-load-earlier a:hover{background:#46515f;color:#fff;"
          "border-color:#46515f}";
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
          @".enil-message{overflow:hidden;margin:18px 12px 0}"
          ".enil-day{clear:both;text-align:center;margin:26px 12px 22px;"
          "height:8px;border-bottom:1px solid #dce1e6}"
          ".enil-day span{display:inline-block;padding:0 12px;background:#fff;"
          "font-size:10px;line-height:16px;font-weight:bold;"
          "color:#6b7683;letter-spacing:0.06em;text-transform:uppercase}"
          ".enil-read-marker{clear:both;position:relative;height:0;"
          "margin:18px 0 0;border-top:1px solid #b8bfc7;text-align:right}"
          ".enil-read-marker .enil-read-label{display:inline-block;"
          "position:relative;top:-8px;margin-right:12px;padding:0 6px;"
          "background:#ffffff;font-size:10px;line-height:14px;color:#68717d}"
          ".enil-seen-marker{clear:both;position:relative;height:0;"
          "margin:18px 0 0;border-top:1px solid #d9dde2;text-align:left}"
          ".enil-seen-marker .enil-seen-label{display:inline-block;"
          "position:relative;top:-8px;margin-left:12px;padding:0 6px;"
          "background:#ffffff;font-size:10px;line-height:14px;color:#68717d}"
          "body{font-family:-apple-system,Helvetica,Arial,sans-serif;"
          "font-size:15px;line-height:1.45}"
          /* The wrapper positions the message; only its content is a card.
           * Names, timestamps, and reactions sit outside the card. */
          ".enil-bubble{padding:0;max-width:85%;word-wrap:break-word;"
          "-webkit-box-sizing:border-box;box-sizing:border-box;"
          "background:transparent;color:#252a31;border:0}"
          ".enil-seg{padding:0}"
          ".enil-media-bubble{max-width:100%;word-wrap:break-word}"
          ".enil-message.incoming{position:relative;padding-left:32px}"
          ".enil-message.incoming .enil-bubble{float:left}"
          ".enil-avatar{position:absolute;left:0;top:0;width:24px;"
          "height:24px;-webkit-border-radius:4px;border-radius:4px;"
          "color:#fff;font-size:11px;font-weight:bold;text-align:center;"
          "line-height:24px;overflow:hidden}"
          "div.enil-avatar{background:#546170!important}"
          "img.enil-avatar{display:block;object-fit:cover;"
          "border:1px solid #dfe3e8}"
          ".enil-message.outgoing .enil-bubble{float:right}"
          ".enil-message.outgoing .enil-media-bubble,"
          ".enil-message.incoming .enil-media-bubble{"
          "background:transparent;border:none}"
          ".enil-media-bubble .enil-seg{padding:0}"
          ".enil-sender{font-size:11px;line-height:16px;color:#46515f;"
          "font-weight:bold;letter-spacing:0.02em;margin:0 0 6px 1px}"
          ".enil-time{font-size:10px;line-height:14px;color:#697583;margin:6px 2px 0}"
          ".enil-bubble a{color:#354f70}"
          ".enil-message.outgoing .enil-time{text-align:right}"
          /* Short outgoing text in a date-wide bubble: shrink-wrap the
           * text block (inline-block) and right-align it so it hugs the
           * right edge under the date, while the words inside stay
           * left-aligned. Empty space falls to the left, which reads
           * naturally for a right-side bubble. */
          ".enil-message.outgoing .enil-seg{text-align:right}"
          ".enil-message.outgoing .enil-text{display:inline-block;"
          "text-align:left}"
          ".enil-text,.enil-placeholder{padding:10px 13px;"
          "background:#eef1f6;border:1px solid #d5dde8;"
          "-webkit-border-radius:7px;border-radius:7px;"
          "-webkit-box-sizing:border-box;box-sizing:border-box;"
          "max-width:100%;line-height:1.5}"
          ".enil-message.outgoing .enil-text,"
          ".enil-message.outgoing .enil-placeholder{"
          "background:#3e506b;border-color:#3e506b;color:#f7f9fc}"
          ".enil-message.outgoing .enil-text a,"
          ".enil-message.outgoing .enil-placeholder a{color:#d5e6ff}"
          ".enil-text .enil-sticon-inline{display:inline-block;"
          "vertical-align:text-bottom;margin:0 1px}"
          ".enil-text .enil-sticon-missing{display:inline-block;"
          "vertical-align:text-bottom;font-size:11px;line-height:18px;"
          "height:18px;padding:0 4px;border:1px solid #ccc;"
          "-webkit-border-radius:10px;color:#777}"
          ".enil-media,.enil-sticker{line-height:0}"
          ".enil-media img{display:block;width:100%;height:auto;border:0;"
          "-webkit-border-radius:0;border-radius:0}"
          ".enil-media a{display:block}"
          ".enil-sticker img{display:block;max-width:100%;height:auto!important}"
          /* Photo rows use the whole viewport on 320-point phones. Text
           * and metadata keep their insets; stickers retain their own size. */
          ".enil-message.enil-photo-message{margin:16px 0 0;padding:0}"
          ".enil-photo-message .enil-avatar{display:none}"
          ".enil-photo-message .enil-bubble{width:320px;max-width:100%;"
          "-webkit-border-radius:0;border-radius:0}"
          ".enil-photo-message .enil-sender,"
          ".enil-photo-message .enil-time,"
          ".enil-photo-message .enil-reactions,"
          ".enil-photo-message .enil-placeholder{margin-left:10px;"
          "margin-right:10px}"
          ".enil-photo-message .enil-sender{margin-bottom:6px}"
          ".enil-placeholder{font-style:italic;color:#737b85}"
          ".enil-send-pending,.enil-send-error{display:none}"
          ".enil-send-pending .fa,.enil-send-error .fa{font-size:9px}"
          ".enil-seg.pending .enil-text{background:#e7edf5;"
          "border-color:#d5dde8;color:#56667e}"
          ".enil-seg.pending .enil-send-pending{display:inline;"
          "color:#9aa3ad}"
          ".enil-seg.pending .enil-sticker img,"
          ".enil-seg.pending .enil-media img{opacity:0.4}"
          ".enil-seg.error .enil-sticker img,"
          ".enil-seg.error .enil-media img{opacity:0.6}"
          ".enil-seg.error .enil-text{background:#fbeded;"
          "border-color:#e5bebe;color:#962d2d}"
          ".enil-seg.error .enil-send-error{display:inline;"
          "color:#c0392b}"
          ".enil-seg.deleted .enil-text{text-decoration:line-through;"
          "background:#f1f3f5;border-color:#dce1e6;color:#697583}"
          ".enil-seg.deleted .enil-sticker img,"
          ".enil-seg.deleted .enil-media img{opacity:0.35}"
          ".enil-seg.deleted .enil-text:after,"
          ".enil-seg.deleted .enil-placeholder:after{"
          "content:' \\00b7 unsent';text-decoration:none;font-style:italic;"
          "font-size:10px;color:#a0a7b0}"
          ".enil-reactions{margin-top:3px;line-height:1}"
          ".enil-message.outgoing .enil-reactions{text-align:right}"
          ".rxn{display:inline-block;margin:4px 5px 0 0;padding:2px 5px;"
          "background:#f4f6f8;border:1px solid #e0e5eb;"
          "-webkit-border-radius:4px;border-radius:4px;vertical-align:middle}"
          ".rxn-badge{display:inline-block;width:18px;height:18px;"
          "line-height:18px;text-align:center;background:transparent}"
          ".rxn-badge .fa{font-size:10px;vertical-align:1px;color:#8a929c}"
          ".rxn-count{margin-left:3px;font-size:11px;color:#5b636e;"
          "font-weight:bold;vertical-align:middle}"
          ".rxn .fa{color:#59636f}"];
}

+ (NSString *)emptyMessageCSS;
{
  return @"body{font-family:-apple-system,Helvetica,Arial,sans-serif;color:#999;"
          "display:table;width:100%;height:100%;margin:0;text-align:center}"
          "p{display:table-cell;vertical-align:middle}";
}

@end
