#!/bin/sh
# Test harness for escpmd. Each test case is a triple of files in
# tests/md-cases/:
#
#   <name>.md     Markdown input
#   <name>.hex    Expected raw byte output as hex pairs (whitespace and
#                 '#' line-comments are ignored)
#   <name>.args   Optional; CLI flags to pass to escpmd. Defaults to
#                 "--no-init --no-ff" so test cases focus on rendering
#                 rather than the prologue/epilogue.
#
# Usage (from project root):  make test
#         or                  cd tests && ESCPMD=../bin/escpmd sh ./run-md.sh

set -u

ESCPMD=${ESCPMD:-../bin/escpmd}
CASE_DIR=${CASE_DIR:-md-cases}

if [ ! -x "$ESCPMD" ]; then
    printf 'FATAL: %s is not executable; build first with `make`.\n' "$ESCPMD" >&2
    exit 2
fi

PASS=0
FAIL=0

# Render each .md file and compare its byte output against .hex.
for md in "$CASE_DIR"/*.md; do
    [ -f "$md" ] || continue
    name=$(basename "$md" .md)
    hex_file="$CASE_DIR/$name.hex"
    args_file="$CASE_DIR/$name.args"

    if [ ! -f "$hex_file" ]; then
        printf 'SKIP  %-22s (no .hex)\n' "$name"
        continue
    fi

    if [ -f "$args_file" ]; then
        args=$(cat "$args_file")
    else
        args='--no-init --no-ff'
    fi

    want=$(sed 's/#.*$//' "$hex_file" | tr -d ' \t\n' | tr 'A-F' 'a-f')

    # shellcheck disable=SC2086
    got=$($ESCPMD $args < "$md" | od -An -tx1 -v | tr -d ' \n')

    if [ "$got" = "$want" ]; then
        PASS=$((PASS+1))
        printf 'PASS  %s\n' "$name"
    else
        FAIL=$((FAIL+1))
        printf 'FAIL  %s\n' "$name"
        printf '      args:     %s\n' "$args"
        printf '      expected: %s\n' "$want"
        printf '      got:      %s\n' "$got"
    fi
done

extra_fail=0

# --help exits 0
if ! "$ESCPMD" --help >/dev/null 2>&1; then
    extra_fail=$((extra_fail+1))
    echo "FAIL  --help exits non-zero"
else
    PASS=$((PASS+1))
    echo "PASS  --help exits 0"
fi

# --version exits 0
if ! "$ESCPMD" --version >/dev/null 2>&1; then
    extra_fail=$((extra_fail+1))
    echo "FAIL  --version exits non-zero"
else
    PASS=$((PASS+1))
    echo "PASS  --version exits 0"
fi

# Invalid --cpi rejected
if "$ESCPMD" --cpi 11 </dev/null >/dev/null 2>&1; then
    extra_fail=$((extra_fail+1))
    echo "FAIL  --cpi 11 should be rejected"
else
    PASS=$((PASS+1))
    echo "PASS  --cpi 11 rejected"
fi

# -o writes to file
tmp=$(mktemp ./escpmd-test.XXXXXX) || exit 2
printf 'hi\n' | "$ESCPMD" --no-init --no-ff -o "$tmp"
if [ "$(od -An -tx1 -v < "$tmp" | tr -d ' \n')" = "68690a" ]; then
    PASS=$((PASS+1))
    echo "PASS  -o writes to file"
else
    extra_fail=$((extra_fail+1))
    echo "FAIL  -o writes to file"
fi
rm -f "$tmp"

# Multi-file: separator is FF between documents.
fa=$(mktemp ./escpmd-test.XXXXXX) || exit 2
fb=$(mktemp ./escpmd-test.XXXXXX) || exit 2
printf 'a\n' > "$fa"
printf 'b\n' > "$fb"
got=$("$ESCPMD" --no-init --no-ff "$fa" "$fb" | od -An -tx1 -v | tr -d ' \n')
if [ "$got" = "610a0c620a" ]; then
    PASS=$((PASS+1))
    echo "PASS  multi-file insert FF between docs"
else
    extra_fail=$((extra_fail+1))
    echo "FAIL  multi-file insert FF between docs"
    printf '      got: %s\n' "$got"
fi
rm -f "$fa" "$fb"

FAIL=$((FAIL + extra_fail))

echo
echo "Results: $PASS passed, $FAIL failed."
[ "$FAIL" -eq 0 ]
