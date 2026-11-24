# Task 5 implementation plan

Spec: TASK5.md (user's bounded execution request). Implement inline, no commits.

- Add a failing selftest using a temporary script: disabled/other slots, inclusive ranges, exact 16-byte state packing, digital triggers, stick extrema, packet stability/change, release. Implement script parsing in null_input.c and route the existing ordinal plus named/Ex aliases through it. Reject malformed scripts loudly. No physical input backend.
- Add phase logging from Present and file_log; use up to 12 named entry hooks. Preserve the engine input path. Verify the logger with the real boot. The existing archive alias hooks also intercept same-shard calls (plain --wrap missed those during the first run).
- Create input/advance.txt with short A presses every 90 frames through frame 3000. Build, selftest, run 120 seconds with requested data/package roots, examine evidence and tune if justified.
- Stop on lifting faults or substantial real subsystem gaps; document exact evidence and append Task 5 to STATUS.md.
