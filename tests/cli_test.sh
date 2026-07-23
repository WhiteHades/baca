#!/bin/sh

set -eu

binary=${1:?usage: cli_test.sh /path/to/mereader-tui}
root=$PWD/build/test-tmp/cli
server_pid=

cleanup() {
    if [ -n "$server_pid" ]; then
        kill "$server_pid" 2>/dev/null || :
        wait "$server_pid" 2>/dev/null || :
    fi
    rm -rf "$root"
    rmdir "$PWD/build/test-tmp" 2>/dev/null || :
}

fail() {
    printf 'FAIL cli.%s\n' "$1" >&2
    exit 1
}

pass() {
    printf 'PASS cli.%s\n' "$1"
}

trap cleanup EXIT HUP INT TERM
cleanup
mkdir -p "$root/home" "$root/xdg-config" "$root/xdg-cache" "$root/tmp"
export HOME="$root/home"
export XDG_CONFIG_HOME="$root/xdg-config"
export XDG_CACHE_HOME="$root/xdg-cache"
export TMPDIR="$root/tmp"
export LC_ALL=C
unset MEREADER_TUI_LIBRARY_PATH

command -v python3 >/dev/null 2>&1 || fail missing_python3
command -v sqlite3 >/dev/null 2>&1 || fail missing_sqlite3
command -v timeout >/dev/null 2>&1 || fail missing_timeout

help_output=$("$binary" --help 2>&1) || fail help
case $help_output in
    *"usage: mereader-tui"*"TUI ebook reader"*"--doctor"*"--history"*) ;;
    *) fail help ;;
esac
pass help

version_output=$("$binary" --version 2>&1) || fail version
[ "$version_output" = "v0.1.1" ] || fail version
pass version

if option_output=$("$binary" --definitely-invalid 2>&1); then
    fail unknown_option
fi
case $option_output in
    *"mereader-tui: unknown option"*) ;;
    *) fail unknown_option ;;
esac
pass unknown_option

doctor_output=$("$binary" --doctor 2>&1) || fail doctor
case $doctor_output in
    *"mereader-tui Doctor"*"Version: 0.1.1"*) ;;
    *) fail doctor ;;
esac
case $doctor_output in
    *"Config: $XDG_CONFIG_HOME/mereader-tui/config.ini"*"Database: $XDG_CACHE_HOME/mereader-tui/mereader-tui.db"*) ;;
    *) fail doctor ;;
esac
case $doctor_output in
    *"Downloads: $XDG_CACHE_HOME/mereader-tui/downloads"*"MOBI/AZW:"*"WebP: ready"*"SVG: ready"*) ;;
    *) fail doctor ;;
esac
pass doctor

history_output=$("$binary" --history 2>&1) || fail history
case $history_output in
    "mereader-tui History"*"Last Read"*"Progress"*"Path"*) ;;
    *) fail history ;;
esac
[ -f "$XDG_CACHE_HOME/mereader-tui/mereader-tui.db" ] || fail history
pass history

library_output=$(printf '\033q' |
    TERM=xterm-256color timeout 10s "$binary" 2>&1) || fail library_default
case $library_output in
    *"mereader-tui"*"Paste the path to your book directory"*) ;;
    *) fail library_default ;;
esac
pass library_default

mkdir -p "$root/library"
library_override_output=$(printf q |
    MEREADER_TUI_LIBRARY_PATH="$root/library" TERM=xterm-256color timeout 10s "$binary" 2>&1) ||
    fail library_override
case $library_override_output in
    *"mereader-tui / all books"*) ;;
    *) fail library_override ;;
esac
pass library_override

if number_output=$("$binary" 1 2>&1); then
    fail missing_history_number
fi
case $number_output in
    *"mereader-tui History"*"history entry #1 not found"*) ;;
    *) fail missing_history_number ;;
esac
pass missing_history_number

book="$root/control.epub"
: > "$book"
database="$XDG_CACHE_HOME/mereader-tui/mereader-tui.db"
sqlite3 "$database" \
    "INSERT OR REPLACE INTO reading_history(filepath,title,author,reading_progress,last_read) VALUES("\
"'$book','Bad'||char(10)||'Title'||char(9)||'Name'||char(27)||'[31m',"\
"'Auth'||char(127)||'or',0.5,'2026-07-18 12:00:00');"
control_output=$("$binary" --history 2>&1) || fail unsafe_history
escape=$(printf '\033')
case $control_output in
    *"$escape"*) fail unsafe_history ;;
esac
case $control_output in
    *"Bad Title Name [31m"*"Auth or"*) ;;
    *) fail unsafe_history ;;
esac
pass unsafe_history

mkdir -p "$root/server"
printf 'Remote document\n' >"$root/server/remote.txt"
port_file="$root/server.port"
python3 - "$root/server" >"$port_file" 2>"$root/server.log" <<'PY' &
import http.server
import os
import sys

os.chdir(sys.argv[1])
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), http.server.SimpleHTTPRequestHandler)
print(server.server_port, flush=True)
server.serve_forever()
PY
server_pid=$!
attempts=0
while [ ! -s "$port_file" ]; do
    attempts=$((attempts + 1))
    if [ "$attempts" -ge 100 ] || ! kill -0 "$server_pid" 2>/dev/null; then
        fail remote_server
    fi
    sleep 0.1
done
IFS= read -r port <"$port_file"
canonical_url="http://127.0.0.1:$port/remote.txt"
remote_url="$canonical_url#chapter"
(sleep 2; printf b; sleep 1; printf qq) |
    TERM=xterm-256color timeout 15s "$binary" "$remote_url" >/dev/null 2>&1 || fail remote_open
[ "$(sqlite3 "$XDG_CACHE_HOME/mereader-tui/mereader-tui.db" "SELECT filepath FROM reading_history WHERE filepath = '$canonical_url'")" = "$canonical_url" ] ||
    fail remote_history_identity
[ "$(sqlite3 "$XDG_CACHE_HOME/mereader-tui/mereader-tui.db" "SELECT filepath FROM bookmarks WHERE filepath = '$canonical_url'")" = "$canonical_url" ] ||
    fail remote_bookmark_identity
kill "$server_pid" 2>/dev/null || :
wait "$server_pid" 2>/dev/null || :
server_pid=
printf q | TERM=xterm-256color timeout 15s "$binary" 1 >/dev/null 2>&1 || fail remote_offline_history
remote_library_output=$( (sleep 2; printf '\n'; sleep 2; printf q; sleep 1; printf q) |
    TERM=xterm-256color timeout 20s "$binary" 2>&1) || fail remote_offline_library
case $remote_library_output in
    *"Remote document"*) ;;
    *) fail remote_offline_library ;;
esac
pass remote_open_history_bookmark_and_offline_cache

printf 'SUMMARY cli passed\n'
