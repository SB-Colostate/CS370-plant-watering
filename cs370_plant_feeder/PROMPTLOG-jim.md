# PROMPTLOG.md — Jim Guadagnoli

Annotated episodes of working with the AI agent. Final submission needs 6–10, including:
one revised plan, one rejected diff, one tool-output debugging loop with real hardware evidence,
and one review of Sam's work.

> Note: Episodes 1–4 happened in a Claude chat session with desktop control during M2 bring-up,
> not a Claude Code CLI session, so they do not appear in my .jsonl transcripts.
> Episodes from M3 on will be in Claude Code.

---

## Episode 1: Floating pin read as sensor data (hardware-evidence debugging loop)
**Date:** 2026-10-28 · **Milestone:** M2
**What I asked:** Check that the Uno soil sketch was uploaded and reading correctly.
**What the agent did:** Opened the Serial Monitor, saw `SOIL,446 … 438 … 409`, and reported that
the soil sensor was "wired correctly and the Uno is reading it."
**What was wrong:** Nothing was plugged into the Uno except USB. A0 was floating, and the
"readings" were electrical noise drifting downward.
**How it was caught:** I told it I had no sensors connected. It corrected itself and explained floating inputs.
**Lesson / rule added:** A changing number is not proof of a sensor. The test is a *controlled change*
(air vs. water) with known wiring. Added "no hardware conclusion without a measurement" to CLAUDE.md.

## Episode 2: Wrong header pin caught from a photo
**Date:** 2026-10-28 · **Milestone:** M2
**Evidence:** Photo of the Uno with the soil sensor wired.
**What happened:** The agent was given the photo and noticed the red (V) wire was in pin 8,
not pin 7 (the Uno's digital header has a gap between 7 and 8), and that the ground wire might be
in AREF instead of GND. I confirmed GND and moved red to 7.
**Lesson:** Photos are evidence; the agent is useful at checking wiring *from a photo*, not from my description.

## Episode 3: Sensor polarity predicted backwards
**Date:** 2026-10-28 · **Milestone:** M2
**What happened:** The agent told me to expect ~900–1023 in dry air, dropping when wet (typical
of bare resistive probes). My module has an on-board transistor (Q1). It read **0 dry** and **rose
when touched or wet**. The agent revised once it saw `SOIL,0` and the board markings.
**Lesson:** Calibration direction must come from my own measurements, not the agent's
"most modules" assumption. Dry/wet calibration points are now part of the design (DESIGN.md §5).

## Episode 4: Architecture shortcut I'm walking back (revised plan)
**Date:** 2026-10-28 · **Milestone:** M2
**What happened:** To get both sensors alive fast, the agent proposed putting the HC-SR04 on the
Uno too, so the Pi just reads one USB serial stream. It worked (`SOIL,0,DIST,9.4`). After reading
the project PDF, the plan changed: sensors should enter the Pi through the kernel and the
mechanisms must live on the Pi, so for M3 the HC-SR04 moves to Pi GPIO (with divider) and the
Uno is only the soil ADC.
**What changed:** DESIGN.md §1 hardware decision; firmware will drop the ultrasonic code.

---

## Episode 5: _(M3: rejected diff — TODO)_

## Episode 6: _(Review of Sam's work — TODO)_
