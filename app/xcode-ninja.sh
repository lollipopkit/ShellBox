#!/bin/sh

# Try to figure out the user's PATH to pick up their installed utilities.
user_path="$(sudo -n -u "$USER" -i printenv PATH 2>/dev/null || true)"
if [ -n "$user_path" ]; then
    export PATH="$PATH:$user_path"
fi
if [ -n "$SRCROOT" ] && [ -d "$SRCROOT/.venv/bin" ]; then
    export PATH="$SRCROOT/.venv/bin:$PATH"
fi

ninja "$@"
