# Watering engine ↔ hub/storage integration (proposal, nothing changed yet)

**Author:** Sam · **Status:** proposal for Jim to review · **Engine:** `src/analysis/engine.h` (branch `sam/watering-engine`)

The watering engine is finished on its own side and tested without hardware. Wiring it in touches
Jim's code (`src/store/record.h`, `src/hub/hub.c`, `src/plantctl/plantctl.c`, `src/common/msg.h`).
**None of those files have been changed.** This document lists exactly what would change, so we can
agree on it first.

## 1. New log record types (`src/store/record.h`)

The engine's events become ordinary 32-byte records: `sensor_id = SENSOR_SYSTEM`,
`flags = REC_EVENT_FLAGS(kind)`, `value` = the engine event's value, `mono_ns` = the hub's
`CLOCK_MONOTONIC` at append time, `wall_s = time(NULL)`. **No change to the record layout, CRC, or
store code.**

The event kind is the top 4 bits of `flags`, so at most 15 kinds; 1–4 are used. Proposed:

| New `REC_EV_*` | Value | Engine event | `value` field | Durable? |
|---|---|---|---|---|
| `REC_EV_WATER_INTENT` | 5 | `ENG_EV_WATER_INTENT` | dose in ms | **yes**, before the pump may start |
| `REC_EV_DIAG` | 6 | `ENG_EV_DIAG` | `enum eng_diag` | **yes** |
| `REC_EV_LOCKOUT` | 7 | `ENG_EV_LOCKOUT` | `enum eng_lock` | **yes** |
| `REC_EV_ACK` | 8 | `ENG_EV_ACK` | lock reason that was cleared | **yes** |
| `REC_EV_PHASE` | 9 | `ENG_EV_PHASE` | `enum eng_phase` | no |
| `REC_EV_BLOCKS` | 10 | `ENG_EV_BLOCKS` | block bitmask | no |

That uses 10 of 15. "Durable" means appended with `STORE_DURABLE` (fdatasync before returning).
Every `WATER_INTENT` is closed by exactly one durable `DIAG` or `LOCKOUT`; restore relies on that.

## 2. Hub startup order (`src/hub/hub.c`)

1. Pump control forces the pump **OFF** (before anything else).
2. `store_open()` as today (it appends `START`).
3. `eng_params_default(&p); if (eng_init(&e, &p, mono_ns()) != 0) exit(EXIT_USAGE);`
4. Replay the **whole** log, oldest first, with `store_reader_open()` / `store_reader_get()`
   (the reader takes no lock, so it works while the hub holds the writer lock):
   - an event record of kind 5–10 → `eng_restore_event(&e, <matching ENG_EV_*>, r.value, r.mono_ns)`
   - **every other record** (samples, `START`, `GAP`, ...) → `eng_restore_event(&e, ENG_EV_NONE, 0, r.mono_ns)`.
     The engine uses their times to see reboots (monotonic time going backwards).
   - Do **not** filter by time or skip records: the hub cannot tell which boot a monotonic time came
     from, and dropping a `WATER_INTENT` or `DIAG` would weaken the rate limit, the daily cap, or a
     lockout.
5. `eng_restore_done(&e, &out)` and handle `out` (section 3). It may lock out
   (`UNFINISHED_WATERING`) and request STOP.
6. Bind the socket and enter the loop.

Cost to note: step 4 reads the whole file a second time (`store_open` already scans it). That's fine
for the 48 h soak (~11 MB), but at ~5.5 MB/day it grows; segments or a checkpoint record are the later fix.

## 3. Handling every engine output (`struct eng_out`)

After **every** engine call:
1. Append `out.ev[0 .. n_events-1]` **in order**, `STORE_DURABLE` where `durable` is set.
   If a durable append fails, the hub must **not** pass the request on; exit `EXIT_SYSERR` (as it
   already does on storage errors) so the pump stays off and the supervisor restarts it.
2. Only then pass `out.request` to pump control (`START` with `out.dose_ms`, or `STOP`).
3. If `out.dropped_info` or `out.dropped_critical` is nonzero, log an ERROR line (those events never
   reached the log). `dropped_critical` should never happen; the engine already turned the request into STOP.

## 4. Hub main loop

- After `store_append(&s, &r, 0)` of a sample: `eng_on_sample(&e, &r, &out)` with the record exactly as stored.
- Every loop pass (`poll` returns at least once a second): `eng_tick(&e, mono_ns(), &out)`.
- From pump control: `eng_on_watering_started(&e, now, &out)` and
  `eng_on_watering_stopped(&e, now, ran_ms, why, &out)` with `why` = `ENG_STOP_*`.

The sampler and the hub both use `CLOCK_MONOTONIC` on the same host, so sample times and hub times
can be compared directly.

## 5. Clearing a lockout (`src/common/msg.h`, `plantctl`)

`plantctl` stays read-only on the log. Proposal: `plantctl ack` sends one datagram to `hub.sock` with
a new `MSG_ACK` type (same 32-byte `struct plant_msg`, new `type`). The hub validates it like
samples, then:
1. `eng_acknowledge(&e, mono_ns(), &out)` and handle `out`: it emits a durable `ACK`. The engine
   **stays locked out** (and keeps requesting STOP) at this point.
2. Only if that `ACK` was appended with `STORE_DURABLE` successfully:
   `eng_ack_saved(&e, mono_ns(), &out)`. Only now is the fault cleared. If the save failed, do not
   call it (and exit as in §3); after the restart the `ACK` is not in the log, so the lockout returns.

The rate limit and daily cap are deliberately **not** reset by an ACK.

## 6. What must stay independent of the engine (pump control + supervisor)

The engine only **requests** STOP. It cannot switch anything off, and if the hub hangs it is not
called at all. So, separately from the engine:
- **Pump control** enforces its own hard cap (`CFG_PUMP_CAP_MS`, 10 min) and turns the pump OFF by
  itself if it stops hearing from the hub; OFF at start and on exit; reports
  `started` / `stopped(ran_ms, why)`.
- **The supervisor** forces the pump OFF when the hub dies or hangs (DESIGN §3, §7), without asking
  the engine.

## 7. Smaller items

- `plantctl tail/status`: names for kinds 5–10, and in `status` the latest phase, blocks, lockout or
  ACK, and diagnosis.
- `msg.h` comment says the level value is "level in mm"; it is the HC-SR04 **distance** in mm
  (larger = less water). Suggest fixing the comment.
- A hub integration test (`tests/it_engine.sh`) with shortened test timings, labelled as such.

## 8. Decisions for Jim and Sam

1. The record kind numbers in §1.
2. Who writes the `hub.c` changes (§2–4) and who reviews.
3. `MSG_ACK` (§5), or another way to clear a lockout.
4. Where pump control lives (inside the hub or as its own process under the supervisor) and its
   "no news from the hub" timeout.
5. Whether replaying the whole log at startup (§2 step 4) is acceptable for now.
