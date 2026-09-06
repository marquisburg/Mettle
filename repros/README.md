# Repros

Programs kept for a person to read, not for a harness to check.

Each one reproduced a bug once and prints values for eyeballing rather than
asserting anything. They live here rather than in `tests/` because a file in
`tests/` that nothing runs and nothing asserts looks exactly like coverage
while providing none: eighty-five of them had accumulated by the time anybody
counted.

Anything here that becomes worth checking should grow an assertion and move to
`tests/`, where a harness will name it and `tests/check_test_orphans.py` will
keep it named.
