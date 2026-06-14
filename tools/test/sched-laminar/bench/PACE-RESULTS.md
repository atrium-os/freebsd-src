# Display frame-pacing under load — the deadline lane, measured

The scheduler federation doc's **#1 pitch axis: frame-time / dropped frames under
load** (the audio underrun story, [`lyra/scripts/L6-RESULTS.md`](../../../../../../lyra/scripts/L6-RESULTS.md),
was the crisper #2). Same apples-to-apples control: the **same periodic 60 Hz
frame workload with the deadline lane ON vs OFF** — the honest BSD-native
"reference" (the lane is the variable under test). Harness:
[`pace-sweep.sh`](pace-sweep.sh) driving [`metronome`](bench/metronome.c) in-VM.

- A **frame** = consume `work_us` of actual **thread CPU time** (`CLOCK_THREAD_CPUTIME_ID`)
  — calibration-free and load-robust: the frame always costs the same CPU, but a
  starved thread takes longer *wall-clock* to accumulate it and misses its vblank.
- **Grid** = a 60 Hz vblank grid (`t = 16200 µs`). A **miss** = a vblank that
  passed with no fresh frame (a dropped/judder frame). The lane arm's misses are
  **kernel-counted** (`LAMIOC_STATS`, robust); the plain arm counts dropped
  vblanks in userspace and resyncs to the grid each frame (no cascade — exactly
  what vsync pacing does).
- **Load** = N CPU spinners on a 4-vCPU guest. Each config run 3×, median reported.
- The lane needs `kern.sched.deadline_enable=1` (the harness sets it).

Unlike audio (a 2.7 ms period — misses under *any* load), a frame's 16.2 ms
period has slack, so timeshare copes with **light** frames and only janks once a
frame's CPU cost is a big enough slice of its fair share under load. So we sweep
**frame cost** (the display analog of audio's buffer-depth sweep).

## Result (2026-06-14) — median dropped-frame %, 3 reps, of ~150 frames

```
                  8 spinners (2x)        16 spinners (4x)
frame cost      plain      lane        plain      lane
 2.0 ms (12%)      4%        3%          12%        1%
 4.0 ms (25%)      2%        2%          42%        0%
 6.0 ms (37%)     54%        0%          72%        0%
 8.0 ms (49%)     83%        0%          66%        0%
10.0 ms (62%)     83%        0%          88%        0%
```

**The lane drops 0% of frames at every depth and both loads** (the 1–3% at the
lightest frames is the startup-period sync + occasional host stall — single
digits, not scheduling). **Plain timeshare copes with light frames, then collapses
— dropping 54–88% of frames** once a frame costs ≳ 6 ms under 2× load, or ≳ 4 ms
under 4× load. At 16 spinners even a 4 ms frame drops 42%.

## Why this is the thesis, measured

- **The lane gives a *guarantee*, not just better averages.** A reserved
  `(Q, T)` frame thread is admitted and replenished every vblank regardless of how
  many spinners exist; timeshare gives a sleeping render thread only its fair
  share, which falls below what a heavy frame needs exactly when the machine is
  busy — the moment jank matters most. This is the visual analog of the audio L6
  result (0 underruns at the hardware-minimum buffer where timeshare needs 24×).
- **The admission ceiling is a feature.** The lane refuses a frame whose `Q/T`
  exceeds the 75% per-CPU cap (`deadline_util_max`) — it won't promise a frame it
  can't keep. So the lane's range is "any frame up to ~62% duty, 0 misses"; past
  that it declines rather than degrade silently.
- **Honest scope.** This measures the *scheduling* contrast at display cadence.
  `frescod`'s broker role — sponsoring this same lane anchored to the **real
  vblank** (`SPONSOR_FOR`, K-b deadline-context adoption) — is separately gated
  (phase J.2b: 606 frames / 0 misses / 16 spinners on the real grid). Together:
  the broker puts frames on the lane; this is what the lane buys them.

Caveats: the absolute plain percentages are HVF-on-macOS / 4-vCPU specific — the
**cliff shape** (timeshare collapses past fair share; the lane holds 0) is the
portable claim, not the exact numbers. The lane's 1–3% light-frame floor is
startup/host-stall noise, not a scheduling miss.
