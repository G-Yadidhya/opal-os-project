# B+ Tree ThreadSanitizer Validation

The existing concurrent B+ Tree correctness test was built with GCC
ThreadSanitizer and run in both mutex-baseline and OPAL-inspired modes.
Clang is unavailable in this environment, so this check uses GCC 13.3.0.

## Result

The non-PIE TSan binary passed three consecutive executions when launched
with ASLR disabled for that process. Every run reported successful lookup,
update, and split-insert checks in both modes, followed by `RESULT: PASS`.
ThreadSanitizer emitted no race report in those three completed runs.

Launching TSan without this setup was unreliable on the host: the default
PIE build failed during startup, and the non-PIE binary also intermittently
failed with `FATAL: ThreadSanitizer: unexpected memory mapping`. Those startup
failures occurred before the test ran. The successful runs provide sanitizer
evidence for this test, not a proof of race freedom. Cross-compiler TSan
validation remains future work.

## Reproduce in a fresh temporary build directory

Run from the project root. A new temporary directory avoids replacing any
existing executable:

```bash
TSAN_DIR=$(mktemp -d /tmp/opal-bptree-tsan.XXXXXX)
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g \
    -fsanitize=thread -fno-omit-frame-pointer -fno-pie -no-pie \
    -pthread -Iinclude src/opal_bptree.c tests/test_opal_bptree.c \
    -o "$TSAN_DIR/opal_bptree_tsan"
for run in 1 2 3; do
    echo "TSan run $run/3"
    setarch "$(uname -m)" -R env TSAN_OPTIONS='halt_on_error=1' \
        "$TSAN_DIR/opal_bptree_tsan" || exit $?
done
```

The build flags were `-std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g
-fsanitize=thread -fno-omit-frame-pointer -fno-pie -no-pie -pthread -Iinclude`.
The test seeds 256 keys, runs two concurrent insert threads, two update
threads, and two lookup threads, then validates the final key count, values,
and update callback count in both modes.
