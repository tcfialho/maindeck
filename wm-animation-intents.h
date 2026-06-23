#ifndef WM_ANIMATION_INTENTS_H
#define WM_ANIMATION_INTENTS_H

#include <stddef.h>
#include <stdint.h>

#include "types.h"

/* Animation intent enum — matches AnimationIntent from river/AnimationIntent.zig */
typedef enum {
    ANIMATION_INTENT_NONE = 0x0,
    ANIMATION_INTENT_FADE_OPEN = 0x1,
    ANIMATION_INTENT_FADE_CLOSE = 0x2,
    ANIMATION_INTENT_SLIDE_CLOSE = 0x3,
    ANIMATION_INTENT_SLIDE_DECK_OUT = 0x4,
    ANIMATION_INTENT_SLIDE_IN = 0x5,
    ANIMATION_INTENT_REFLOW_EASE = 0x6,
    ANIMATION_INTENT_SPRING = 0x7,
    ANIMATION_INTENT_NUDGE = 0x8,
    ANIMATION_INTENT_FS_CAROUSEL = 0x9,
    ANIMATION_INTENT_MINIMIZE = 0xa,
    ANIMATION_INTENT_UNMINIMIZE = 0xb,
    ANIMATION_INTENT_SLIDE_DECK_OUT_LEFT = 0xc,
    ANIMATION_INTENT_DECK_IN_RIGHT = 0xd,
    ANIMATION_INTENT_DECK_IN_LEFT = 0xe,
} AnimationIntent;

/* Send animation intent to compositor for a specific window */
void md_send_animation_intent(struct Window *window, AnimationIntent intent);

/* Pré-registra a animação de CLOSE desta janela (sticky no compositor). Chamada a
 * cada relayout com o papel atual da janela (deck → SLIDE_DECK_OUT, main-com-deck →
 * SLIDE_CLOSE, solo → FADE_CLOSE), para que um close iniciado pelo cliente (que o
 * compositor processa antes do WM reagir) tenha a direção certa sem inferência
 * geométrica. Requer proxy v7 (set_close_intent); em compositor antigo é no-op. */
void md_send_close_intent(struct Window *window, AnimationIntent intent);

/* Get human-readable name for an intent (for logging) */
const char *md_animation_intent_name(AnimationIntent intent);

/* Helper functions: intent selection based on context.
 * (Após a refatoração declarativa, a maioria das animações é decidida na AÇÃO via
 * wm.pending_anim; restam os helpers usados pelos blocos de janelas
 * transient/floating e pela transição de visibilidade.) */
AnimationIntent md_intent_for_open(size_t visible_count_before);
AnimationIntent md_intent_for_close(int32_t closing_window_index, size_t visible_count_before);
AnimationIntent md_intent_for_nudge(void);
AnimationIntent md_intent_for_reflow(void);

#endif /* WM_ANIMATION_INTENTS_H */
