#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>

#include "bar-game-mode.h"

static void spawn_mako_mode(bool on) {
	if (!fork()) {
		setsid();
		execlp("makoctl", "makoctl", "mode", on ? "-a" : "-r", "maindeck-game", NULL);
		_exit(1);
	}
}

static void spawn_mako_dismiss_all(void) {
	if (!fork()) {
		setsid();
		execlp("makoctl", "makoctl", "dismiss", "-a", "-h", NULL);
		_exit(1);
	}
}

static void spawn_mode_sound(bool on) {
	if (!fork()) {
		setsid();
		const char *snd = on
			? "/usr/share/sounds/freedesktop/stereo/complete.oga"
			: "/usr/share/sounds/freedesktop/stereo/dialog-information.oga";
		execlp("paplay", "paplay", snd, NULL);
		_exit(1);
	}
}

void bar_game_mode_apply(bool on) {
	spawn_mako_mode(on);
	if (on) {
		spawn_mako_dismiss_all();
	}
	spawn_mode_sound(on);
}

void bar_game_mode_reset_notifications(void) {
	spawn_mako_mode(false);
}
