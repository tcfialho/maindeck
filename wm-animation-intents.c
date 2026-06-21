#include <stdint.h>
#include <string.h>

#include <river-window-management-v1-client-protocol.h>

#include "wm-animation-intents.h"
#include "wm-log.h"
#include "wm-state.h"

/* Animation intent duration lookup table (in milliseconds).
 * These can be tuned via #define overrides at compile time.
 * Compositor uses these durations when WM specifies an intent. */

#ifndef INTENT_FADE_OPEN_MS
#define INTENT_FADE_OPEN_MS    220
#endif

#ifndef INTENT_FADE_CLOSE_MS
#define INTENT_FADE_CLOSE_MS   200
#endif

#ifndef INTENT_SLIDE_CLOSE_MS
#define INTENT_SLIDE_CLOSE_MS  200
#endif

#ifndef INTENT_SLIDE_DECK_OUT_MS
#define INTENT_SLIDE_DECK_OUT_MS 200
#endif

#ifndef INTENT_SLIDE_IN_MS
#define INTENT_SLIDE_IN_MS     200
#endif

#ifndef INTENT_REFLOW_EASE_MS
#define INTENT_REFLOW_EASE_MS  280
#endif

#ifndef INTENT_SPRING_MS
#define INTENT_SPRING_MS       280
#endif

#ifndef INTENT_NUDGE_MS
#define INTENT_NUDGE_MS        160
#endif

#ifndef INTENT_FS_CAROUSEL_MS
#define INTENT_FS_CAROUSEL_MS  240
#endif

#ifndef INTENT_MINIMIZE_MS
#define INTENT_MINIMIZE_MS     200
#endif

#ifndef INTENT_UNMINIMIZE_MS
#define INTENT_UNMINIMIZE_MS   220
#endif

/* Animation easing selections */
typedef enum {
    EASING_LINEAR = 0x0,
    EASING_EASE_IN = 0x1,
    EASING_EASE_OUT = 0x2,
    EASING_EASE_IN_OUT = 0x3,
    EASING_CUBIC_SPRING = 0x4,
} AnimationEasing;

/* Internal function: get duration and easing for an intent */
struct IntentConfig {
    uint32_t duration_ms;
    AnimationEasing easing;
};

static struct IntentConfig intentConfig(AnimationIntent intent) {
    switch (intent) {
        case ANIMATION_INTENT_NONE:
            return (struct IntentConfig){.duration_ms = 0, .easing = EASING_LINEAR};
        case ANIMATION_INTENT_FADE_OPEN:
            return (struct IntentConfig){.duration_ms = INTENT_FADE_OPEN_MS, .easing = EASING_EASE_OUT};
        case ANIMATION_INTENT_FADE_CLOSE:
            return (struct IntentConfig){.duration_ms = INTENT_FADE_CLOSE_MS, .easing = EASING_EASE_IN};
        case ANIMATION_INTENT_SLIDE_CLOSE:
            return (struct IntentConfig){.duration_ms = INTENT_SLIDE_CLOSE_MS, .easing = EASING_EASE_IN};
        case ANIMATION_INTENT_SLIDE_DECK_OUT:
            return (struct IntentConfig){.duration_ms = INTENT_SLIDE_DECK_OUT_MS, .easing = EASING_EASE_IN};
        case ANIMATION_INTENT_SLIDE_IN:
            return (struct IntentConfig){.duration_ms = INTENT_SLIDE_IN_MS, .easing = EASING_EASE_OUT};
        case ANIMATION_INTENT_REFLOW_EASE:
            return (struct IntentConfig){.duration_ms = INTENT_REFLOW_EASE_MS, .easing = EASING_EASE_IN_OUT};
        case ANIMATION_INTENT_SPRING:
            return (struct IntentConfig){.duration_ms = INTENT_SPRING_MS, .easing = EASING_CUBIC_SPRING};
        case ANIMATION_INTENT_NUDGE:
            return (struct IntentConfig){.duration_ms = INTENT_NUDGE_MS, .easing = EASING_EASE_OUT};
        case ANIMATION_INTENT_FS_CAROUSEL:
            return (struct IntentConfig){.duration_ms = INTENT_FS_CAROUSEL_MS, .easing = EASING_CUBIC_SPRING};
        case ANIMATION_INTENT_MINIMIZE:
            return (struct IntentConfig){.duration_ms = INTENT_MINIMIZE_MS, .easing = EASING_EASE_IN};
        case ANIMATION_INTENT_UNMINIMIZE:
            return (struct IntentConfig){.duration_ms = INTENT_UNMINIMIZE_MS, .easing = EASING_EASE_OUT};
        default:
            return (struct IntentConfig){.duration_ms = 0, .easing = EASING_LINEAR};
    }
}

/* Human-readable intent name (for logging) */
const char *md_animation_intent_name(AnimationIntent intent) {
    switch (intent) {
        case ANIMATION_INTENT_NONE: return "NONE";
        case ANIMATION_INTENT_FADE_OPEN: return "FADE_OPEN";
        case ANIMATION_INTENT_FADE_CLOSE: return "FADE_CLOSE";
        case ANIMATION_INTENT_SLIDE_CLOSE: return "SLIDE_CLOSE";
        case ANIMATION_INTENT_SLIDE_DECK_OUT: return "SLIDE_DECK_OUT";
        case ANIMATION_INTENT_SLIDE_IN: return "SLIDE_IN";
        case ANIMATION_INTENT_REFLOW_EASE: return "REFLOW_EASE";
        case ANIMATION_INTENT_SPRING: return "SPRING";
        case ANIMATION_INTENT_NUDGE: return "NUDGE";
        case ANIMATION_INTENT_FS_CAROUSEL: return "FS_CAROUSEL";
        case ANIMATION_INTENT_MINIMIZE: return "MINIMIZE";
        case ANIMATION_INTENT_UNMINIMIZE: return "UNMINIMIZE";
        default: return "UNKNOWN";
    }
}

/* Send animation intent to compositor via protocol.
 * Requires the window object bound at protocol v6 (set_animation_intent is
 * since=6). The compositor applies the intent to the NEXT render_finish for this
 * window, so this must be called within the same manage/render sequence as the
 * geometry/visibility change it describes (which is how wm-layout.c calls it). */
void md_send_animation_intent(struct Window *window, AnimationIntent intent) {
    if (!window || !window->obj) {
        LOG_WARN("md_send_animation_intent: invalid window");
        return;
    }

    /* The request is since=6. If the compositor only offered an older version,
     * the proxy lacks the opcode and calling it would be a protocol error; skip
     * silently so an old compositor still works (animations just don't fire). */
    if (wl_proxy_get_version((struct wl_proxy *)window->obj) < 6) {
        return;
    }

    struct IntentConfig config = intentConfig(intent);

    if (md_verbose()) {
        LOG_EVENT("anim intent: window=\"%s\" intent=%s duration=%u ms easing=%u",
                  window->title ? window->title : "",
                  md_animation_intent_name(intent),
                  config.duration_ms,
                  config.easing);
    }

    river_window_v1_set_animation_intent(window->obj,
                                         (uint32_t)intent,
                                         config.duration_ms,
                                         (uint32_t)config.easing);
}

/* Helper: determine intent based on acion context.
 * Used by wm-layout.c to automatically select the right animation for each action. */
AnimationIntent md_intent_for_action(WindowAction action, size_t visible_count, int32_t window_index) {
    switch (action) {
        case WINDOW_ACTION_MINIMIZE:
            return ANIMATION_INTENT_MINIMIZE;
        case WINDOW_ACTION_MAXIMIZE:
            return ANIMATION_INTENT_NONE; /* No animation for maximize itself */
        case WINDOW_ACTION_RESTORE:
            return ANIMATION_INTENT_NONE; /* No animation for restore itself */
        default:
            return ANIMATION_INTENT_NONE;
    }
}

/* Helper: intent for opening a window. Context: is it solo or in a group? */
AnimationIntent md_intent_for_open(size_t visible_count_before) {
    if (visible_count_before == 0) {
        /* Opening the first window — solo open */
        return ANIMATION_INTENT_FADE_OPEN;
    } else {
        /* Opening in a group — slide in from left */
        return ANIMATION_INTENT_SLIDE_IN;
    }
}

/* Helper: intent for closing a window. Context: which slot, how many visible after? */
AnimationIntent md_intent_for_close(int32_t closing_window_index, size_t visible_count_before) {
    if (visible_count_before == 1) {
        /* Closing the last visible window — solo close */
        return ANIMATION_INTENT_FADE_CLOSE;
    } else if (closing_window_index == 1) {
        /* Closing the deck window — slide right */
        return ANIMATION_INTENT_SLIDE_DECK_OUT;
    } else {
        /* Closing main with deck visible — slide left */
        return ANIMATION_INTENT_SLIDE_CLOSE;
    }
}

/* Helper: intent for deck navigation (next/prev). Always reflow, no special effect. */
AnimationIntent md_intent_for_deck_navigate(void) {
    return ANIMATION_INTENT_REFLOW_EASE;
}

/* Helper: intent for swap main↔deck. Spring geometry. */
AnimationIntent md_intent_for_swap(void) {
    return ANIMATION_INTENT_SPRING;
}

/* Helper: intent for focus nudge (Ctrl+Tab). Small lateral translate. */
AnimationIntent md_intent_for_nudge(void) {
    return ANIMATION_INTENT_NUDGE;
}

/* Helper: intent for fullscreen carousel (maximized app cycling). */
AnimationIntent md_intent_for_fs_carousel(void) {
    return ANIMATION_INTENT_FS_CAROUSEL;
}

/* Helper: intent for deck movement. */
AnimationIntent md_intent_for_deck_move(void) {
    return ANIMATION_INTENT_REFLOW_EASE;
}

/* Helper: intent for layout reflow. */
AnimationIntent md_intent_for_reflow(void) {
    return ANIMATION_INTENT_REFLOW_EASE;
}
