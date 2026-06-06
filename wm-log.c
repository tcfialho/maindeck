#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>

#include "types.h"
#include "wm-log.h"

static FILE *log_file;

void log_init(void) {
	const char *home = getenv("HOME");
	if (home == NULL) home = "/tmp";
	char path[512];
	snprintf(path, sizeof(path), "%s/.local/state/maindeck", home);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/.local/state/maindeck/maindeck.log", home);
	log_file = fopen(path, "a");
	if (log_file != NULL) {
		// Line-buffered em debug (testes podem ler o log em tempo real);
		// fully-buffered em produção (menos syscalls no caminho quente).
		if (md_verbose()) {
			setvbuf(log_file, NULL, _IOLBF, BUFSIZ);
		} else {
			setvbuf(log_file, NULL, _IOFBF, BUFSIZ);
		}
	}
}

void log_close(void) {
	if (log_file != NULL) {
		fclose(log_file);
		log_file = NULL;
	}
}

// Nível de log: WARN/erros sempre; verboso (INFO/EVENT/STATE) só com
// MAINDECK_LOG=debug. Lido uma vez (cache). O gate fica no TOPO de md_log,
// antes de qualquer formatação, então linha suprimida custa ~0.
int md_verbose(void) {
	static int v = -1;
	if (v < 0) {
		const char *e = getenv("MAINDECK_LOG");
		v = (e && (e[0] == 'd' || e[0] == 'D')) ? 1 : 0; // "debug"
	}
	return v;
}

void md_log(const char *level, const char *fmt, ...) {
	// WARN (level[0]=='W') sempre passa; o resto só em verboso.
	bool is_warn = (level[0] == 'W');
	if (!is_warn && !md_verbose()) return;
	// Destino único: maindeck.log (stderr vira session.log via init do River —
	// escrita dupla evitada gravando só aqui). Mas se o log_file não abriu, um
	// WARN não pode sumir: cai pro stderr como rede de segurança.
	FILE *out = log_file;
	if (out == NULL) {
		if (!is_warn) return;
		out = stderr;
	}

	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	struct tm tmv;
	localtime_r(&ts.tv_sec, &tmv); // _r: sem estado global / reentrante
	char tbuf[16];
	strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmv);

	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	fprintf(out, "[%s.%03ld] [%s] %s\n", tbuf, ts.tv_nsec / 1000000, level, msg);
	if (is_warn || level[0] == 'E' || level[0] == 'C') {
		fflush(out);
	}
}

