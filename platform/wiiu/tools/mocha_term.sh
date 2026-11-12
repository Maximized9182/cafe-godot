#!/usr/bin/env bash
# Interactive terminal for the shared Mocha TCP shell; see mocha_term.py.
# Usage: tools/mocha_term.sh   (Ctrl+C or Ctrl+D quits; the logger keeps running)
exec python3 -I "$(dirname "$(readlink -f "$0")")/mocha_term.py" "$@"
