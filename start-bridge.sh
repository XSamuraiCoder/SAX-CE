#!/bin/sh
# SAX-CE Bridge: starts the relay (lets the bridge page reach IRC) and opens
# the bridge page. Keep it running while you chat.
cd "$(dirname "$0")" || exit 1
if ! command -v node >/dev/null 2>&1; then
  echo "Node.js is needed: https://nodejs.org/ (version 18 or newer)"
  exit 1
fi
exec node relay/relay.mjs "$@"
