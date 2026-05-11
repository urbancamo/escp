#!/bin/sh
# Test harness for escp.  Verifies the exact byte output of every CLI
# option against an expected hex string.
#
# Usage (from project root):   make test
#         or                    cd tests && ESCP=../bin/escp sh ./run.sh
#
# Each test case is a single line:
#     LABEL : ARGS... : EXPECTED_HEX
#
# Whitespace in EXPECTED_HEX is ignored.  The line is split on the
# literal ' : ' three-character separator.

set -u

ESCP=${ESCP:-../bin/escp}

if [ ! -x "$ESCP" ]; then
    printf 'FATAL: %s is not executable; build first with `make`.\n' "$ESCP" >&2
    exit 2
fi

CASES_FILE=${CASES_FILE:-cases.txt}
PASS=0
FAIL=0

while IFS= read -r line; do
    case "$line" in
        '' | \#*) continue ;;
    esac

    label=$(printf '%s' "$line" | awk -F ' : ' '{print $1}')
    args=$(printf '%s' "$line"  | awk -F ' : ' '{print $2}')
    want=$(printf '%s' "$line"  | awk -F ' : ' '{print $3}' \
           | tr -d ' \t' | tr 'A-F' 'a-f')

    # shellcheck disable=SC2086
    got=$(eval "$ESCP" $args | od -An -tx1 -v | tr -d ' \n')

    if [ "$got" = "$want" ]; then
        PASS=$((PASS+1))
        printf 'PASS  %s\n' "$label"
    else
        FAIL=$((FAIL+1))
        printf 'FAIL  %s\n' "$label"
        printf '      args:     %s\n' "$args"
        printf '      expected: %s\n' "$want"
        printf '      got:      %s\n' "$got"
    fi
done < "$CASES_FILE"

# Behavioural tests that can't be expressed by a single hex literal.
extra_fail=0

# Help should exit 0 and mention the program name.
if ! "$ESCP" --help >/dev/null 2>&1; then
    extra_fail=$((extra_fail+1))
    echo "FAIL  --help exits non-zero"
else
    PASS=$((PASS+1))
    echo "PASS  --help exits 0"
fi

# No-arg invocation should exit non-zero.
if "$ESCP" >/dev/null 2>&1; then
    extra_fail=$((extra_fail+1))
    echo "FAIL  no-arg invocation should fail"
else
    PASS=$((PASS+1))
    echo "PASS  no-arg invocation fails"
fi

# Invalid range should fail.
if "$ESCP" --cpi 99 >/dev/null 2>&1; then
    extra_fail=$((extra_fail+1))
    echo "FAIL  --cpi 99 should be rejected"
else
    PASS=$((PASS+1))
    echo "PASS  --cpi 99 rejected"
fi

# -o writes to a file.
tmp=$(mktemp ./escp-test.XXXXXX) || exit 2
"$ESCP" -o "$tmp" --init
if [ "$(od -An -tx1 -v < "$tmp" | tr -d ' \n')" = "1b40" ]; then
    PASS=$((PASS+1))
    echo "PASS  -o writes to file"
else
    extra_fail=$((extra_fail+1))
    echo "FAIL  -o writes to file"
fi
rm -f "$tmp"

FAIL=$((FAIL + extra_fail))

echo
echo "Results: $PASS passed, $FAIL failed."
[ "$FAIL" -eq 0 ]
