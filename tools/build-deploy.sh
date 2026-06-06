#!/usr/bin/env bash
#
# Build and deploy maindeck binaries to the current user's local bin dir.
#
# Default restart order is deliberate:
#   1. maindeck-menu: transient client, no supervisor expected.
#   2. maindeck-wm: provider for the taskbar IPC socket.
#   3. maindeck-bar: consumer of the WM IPC socket, restarted last.
#
# The script does not kill river or the river init loop. It only terminates
# maindeck processes owned by the current user, by exact process name.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_DIR/build}"
INSTALL_DIR="${INSTALL_DIR:-$HOME/.local/bin}"
TIMEOUT="${DEPLOY_TIMEOUT:-8}"
UID_FILTER="$(id -u)"

DRY_RUN=0
RESTART=1
RUN_TESTS=0

usage() {
	cat <<EOF
Usage: tools/build-deploy.sh [options]

Options:
  --test        Run tools/test-transient-behavior.sh before deploy.
  --no-restart  Build/install only; do not restart running processes.
  --dry-run     Print actions without executing them.
  -h, --help    Show this help.

Environment:
  BUILD_DIR       Build directory. Default: $REPO_DIR/build
  INSTALL_DIR     Install directory. Default: $HOME/.local/bin
  DEPLOY_TIMEOUT  Seconds to wait for stop/restart. Default: 8
EOF
}

log() {
	printf '[deploy] %s\n' "$*"
}

die() {
	printf '[deploy] ERROR: %s\n' "$*" >&2
	exit 1
}

while [ "$#" -gt 0 ]; do
	case "$1" in
		--test)
			RUN_TESTS=1
			;;
		--no-restart)
			RESTART=0
			;;
		--dry-run)
			DRY_RUN=1
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			die "unknown option: $1"
			;;
	esac
	shift
done

run() {
	log "+ $*"
	if [ "$DRY_RUN" -eq 0 ]; then
		"$@"
	fi
}

pids_for() {
	local name="$1"
	pgrep -u "$UID_FILTER" -x "$name" || true
}

pid_is_in() {
	local needle="$1"
	shift
	local pid
	for pid in "$@"; do
		[ "$pid" = "$needle" ] && return 0
	done
	return 1
}

pid_is_alive() {
	kill -0 "$1" 2>/dev/null
}

wait_until_gone() {
	local deadline=$((SECONDS + TIMEOUT))
	local pid

	while [ "$SECONDS" -lt "$deadline" ]; do
		local alive=0
		for pid in "$@"; do
			if pid_is_alive "$pid"; then
				alive=1
				break
			fi
		done
		[ "$alive" -eq 0 ] && return 0
		sleep 0.2
	done

	return 1
}

terminate_pids() {
	local name="$1"
	shift
	local pids=("$@")
	local pid

	[ "${#pids[@]}" -eq 0 ] && return 0

	log "stopping $name: ${pids[*]}"
	if [ "$DRY_RUN" -eq 0 ]; then
		for pid in "${pids[@]}"; do
			kill -TERM "$pid" 2>/dev/null || true
		done

		if ! wait_until_gone "${pids[@]}"; then
			log "$name did not stop after ${TIMEOUT}s; sending KILL to remaining old pids"
			for pid in "${pids[@]}"; do
				if pid_is_alive "$pid"; then
					kill -KILL "$pid" 2>/dev/null || true
				fi
			done
			wait_until_gone "${pids[@]}" || die "$name still alive after KILL"
		fi
	fi
}

wait_for_replacement() {
	local name="$1"
	shift
	local old_pids=("$@")
	local deadline=$((SECONDS + TIMEOUT))

	while [ "$SECONDS" -lt "$deadline" ]; do
		local current=()
		local pid
		mapfile -t current < <(pids_for "$name")
		for pid in "${current[@]}"; do
			if ! pid_is_in "$pid" "${old_pids[@]}"; then
				log "$name restarted: $pid"
				return 0
			fi
		done
		sleep 0.2
	done

	log "warning: $name did not restart within ${TIMEOUT}s"
	return 0
}

restart_exact_name() {
	local name="$1"
	local supervised="$2"
	local old=()
	mapfile -t old < <(pids_for "$name")

	if [ "${#old[@]}" -eq 0 ]; then
		log "$name is not running"
		return 0
	fi

	terminate_pids "$name" "${old[@]}"
	if [ "$supervised" -eq 1 ] && [ "$DRY_RUN" -eq 0 ]; then
		wait_for_replacement "$name" "${old[@]}"
	fi
}

ensure_build_dir() {
	if [ ! -f "$BUILD_DIR/build.ninja" ]; then
		run meson setup "$BUILD_DIR" "$REPO_DIR"
	fi
}

build() {
	ensure_build_dir
	run meson compile -C "$BUILD_DIR"
}

run_tests() {
	if [ "$RUN_TESTS" -eq 1 ]; then
		run "$REPO_DIR/tools/test-transient-behavior.sh"
	fi
}

install_binaries() {
	local bin
	run mkdir -p "$INSTALL_DIR"

	for bin in maindeck-wm maindeck-bar maindeck-menu; do
		local src="$BUILD_DIR/$bin"
		local dst="$INSTALL_DIR/$bin"
		local tmp="$dst.tmp.$$"

		[ -x "$src" ] || die "missing built binary: $src"
		run install -m755 "$src" "$tmp"
		run mv -f "$tmp" "$dst"
	done
}

restart_processes() {
	[ "$RESTART" -eq 1 ] || return 0

	restart_exact_name maindeck-menu 0
	restart_exact_name maindeck-wm 1
	restart_exact_name maindeck-bar 1
}

cd "$REPO_DIR"
log "repo: $REPO_DIR"
log "build dir: $BUILD_DIR"
log "install dir: $INSTALL_DIR"

build
run_tests
install_binaries
restart_processes

log "deploy complete"
