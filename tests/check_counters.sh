
#!/usr/bin/env bash

set -u

EXPECTED=4000000
PROGRAMS=("./mutex_counter" "./atomic_counter")
FAILURES=0

for program in "${PROGRAMS[@]}"; do
    echo "Testing $program"

    if [[ ! -x "$program" ]]; then
        echo "FAIL: executable not found"
        FAILURES=$((FAILURES + 1))
        continue
    fi

    for run in $(seq 1 10); do
        output=$("$program") || {
            echo "FAIL: program returned an error"
            FAILURES=$((FAILURES + 1))
            continue
        }

        actual=$(printf '%s\n' "$output" |
            awk '/Actual counter:/ {print $3}')

        if [[ "$actual" == "$EXPECTED" ]]; then
            echo "  Run $run: PASS ($actual)"
        else
            echo "  Run $run: FAIL (got ${actual:-missing})"
            FAILURES=$((FAILURES + 1))
        fi
    done
done

if [[ "$FAILURES" -eq 0 ]]; then
    echo "All counter correctness checks passed."
    exit 0
else
    echo "$FAILURES check(s) failed."
    exit 1
fi
