/* ============================================================================
 * enil_render_scale.h — display scaling for sticker / sticon images.
 *
 * Each value is the integer divisor applied to a source bitmap's pixel
 * dimensions before it is rendered: the image is drawn at 1/N of its true
 * size. LINE's CDN assets are large, so downscaling the display box keeps
 * stickers/sticons sized for the chat layout while leaving the full-res
 * bitmap to oversample — crisp on Retina (2x), clean downsample on 1x.
 *
 * These are the knobs to turn for sticker/sticon sizing; nothing else needs
 * to change. Used by enil_html.c (chat-bubble stickers) and
 * enil_message_format.c (inline sticons).
 * ==========================================================================*/
#ifndef ENIL_RENDER_SCALE_H
#define ENIL_RENDER_SCALE_H

#define ENIL_STICKER_SCALE_DIV       1.3  /* chat-bubble sticker: two-thirds size */
#define ENIL_STICON_SCALE_DIV        6    /* inline sticon w/ text: one-sixth     */
#define ENIL_STICON_JUMBO_SCALE_DIV  3    /* sticon-only ("jumbo") message: third */

#endif /* ENIL_RENDER_SCALE_H */
