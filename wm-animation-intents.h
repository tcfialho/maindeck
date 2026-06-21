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
} AnimationIntent;

/* Send animation intent to compositor for a specific window */
void md_send_animation_intent(struct Window *window, AnimationIntent intent);

/* Get human-readable name for an intent (for logging) */
const char *md_animation_intent_name(AnimationIntent intent);

/* Helper functions: intent selection based on context */
AnimationIntent md_intent_for_action(WindowAction action, size_t visible_count, int32_t window_index);
AnimationIntent md_intent_for_open(size_t visible_count_before);
AnimationIntent md_intent_for_close(int32_t closing_window_index, size_t visible_count_before);
AnimationIntent md_intent_for_deck_navigate(void);
AnimationIntent md_intent_for_swap(void);
AnimationIntent md_intent_for_nudge(void);
AnimationIntent md_intent_for_fs_carousel(void);
AnimationIntent md_intent_for_deck_move(void);
AnimationIntent md_intent_for_reflow(void);

#endif /* WM_ANIMATION_INTENTS_H */
