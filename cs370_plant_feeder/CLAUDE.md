# CS370 Plant Watering Project: Team Rules

## Project purpose

Build a Raspberry Pi 5 device that uses at least two physically distinct, cooperating sensors to support unattended plant watering and observable fault handling.

The system will use a capacitive soil moisture sensor to monitor soil conditions and a HALJIA mini float switch to monitor whether water is available in the reservoir. These sensors will cooperate when making watering decisions.

## Known hardware

* Raspberry Pi 5
* ELEGOO UNO Super Starter Kit
* Capacitive soil moisture sensor
* HALJIA mini float switch
* Small water pump and tubing
* Irrigation relay and control hardware
* Two 1/2 inch solenoid valves
* TIP120 transistors
* Diodes and resistors
* Breadboards, jumper wires, and connectors

Still needed: an ADC for the capacitive soil moisture sensor (analog output). The Pi 5 has no analog inputs; an ADS1115 (I2C) or MCP3008 (SPI) is the expected choice. Do not write driver code for a specific ADC until the part is on the bench.

Electrical notes: Pi GPIO is 3.3 V and not 5 V tolerant. Wire with power off. Pump and valve loads are switched through TIP120s or the relay with flyback diodes, never driven from a GPIO pin directly.

The Raspberry Pi 5 is the primary computing platform. Components from the ELEGOO UNO Super Starter Kit may be used for development, prototyping, and supporting electronics as needed.

## Hardware changes since M1

See `DESIGN.md` §10. Current build: Raspberry Pi 3 (budget), the kit's resistive soil probe read through the Arduino Uno as a USB ADC (until an ADS1115/MCP3008 is ordered or the TA rules on the Uno), and an HC-SR04 ultrasonic gauge for reservoir level in place of the float switch. The HC-SR04 echo pin needs a 1k/2k divider on the Pi. The pump is on order; until it arrives, watering is simulated and must be labeled as simulated.

## Hard constraints

* The Raspberry Pi 5 is the primary project platform.
* The systems core must use the C standard required by the course and compile cleanly with `-Wall -Wextra -Werror`.
* At least two physically distinct sensors must cooperate in the same system behavior.
* The soil moisture sensor and reservoir float switch must both contribute to watering decisions.
* Do not reduce the project to a single soil moisture threshold that directly turns on the pump.
* The shipped product must not depend on an LLM or hosted or pretrained model.
* The product makes no network calls except serving its own LAN interface, and must keep watering correctly with the network unplugged.
* Every daemon must be supervisable: clean exit codes, no orphaned file descriptors across restarts, and a heartbeat the supervisor can check.
* If the controller or actuator process dies or hangs, the actuator output must end up OFF. Water must never be left running because a process crashed.
* Hardware failures and system call failures must have explicit error handling where applicable.
* When the software cannot establish that watering is safe, the pump and valve should default to OFF.
* Pump runs are hard-capped at 10 minutes (controller timer, with the supervisor as an independent backstop). Never remove or raise the cap without both partners agreeing; tests may lower it but must say so.
* Never invent measurements, passing tests, hardware behavior, transcripts, commits, or completed features.

## Current architecture direction

The likely architecture separates sensor acquisition, watering decisions, actuation and logging responsibilities, and supervision where those process boundaries provide a real reliability or measurement benefit.

Mechanism E, multi process architecture with IPC and supervision, and Mechanism D, custom storage, are the current mechanism choices. They are not yet implemented, and the final design should justify them with measurements.

The soil moisture sensor will determine whether watering should be considered. The reservoir float switch will determine whether water is available. The controller should only permit watering when the available sensor information supports that decision.

After watering, later soil moisture observations can be compared with the condition before watering to help determine whether the watering event had the expected effect.

## Development rules

* Inspect existing code before changing it.
* Make small, reviewable changes.
* Compile with warnings treated as errors.
* Test error paths as well as normal paths.
* Record real hardware evidence when debugging.
* Do not weaken a test simply to make it pass.
* Keep hardware specific pin numbers, thresholds, timing values, and calibration values centralized rather than scattered through the code.
* Do not claim a threshold, timing value, or calibration constant is correct until it has been measured on the actual hardware.
* Do not claim a feature or failure recovery behavior works until it has been tested.
* Preserve genuine raw Claude Code `.jsonl` sessions when they exist. Never fabricate a transcript. Each partner copies their own sessions into `transcripts/<name>/` at every milestone.
* Multi file or algorithmic changes: plan first and wait for approval.
* Commit only from a green state. Message format: `M<n>: <what>`.

## Build and test

Run on the Pi from the repo root (gcc 14, aarch64):

* `make` builds `build/bin/{hub,sampler,plantctl}`.
* `make test` runs the unit tests (record, store, parse) and the integration scripts (`tests/it_plantctl.sh`, `tests/it_pipeline.sh`).
* `make asan` rebuilds into `build-asan/` with UBSan and runs the same tests under valgrind memcheck. GCC's ASan cannot run on the stock Pi kernel (39-bit VA); see the Makefile. Needs `sudo apt install valgrind`.
* Live run: `build/bin/hub &` then `build/bin/sampler` (defaults: data in `~/plant-data`, Uno by `/dev/serial/by-id`). Inspect with `build/bin/plantctl status` and `build/bin/plantctl tail N`.

Not implemented yet, so no commands: supervisor, soak test, power-pull trials.

Do not invent build or test commands for functionality that has not yet been implemented.

## Ownership

* Student 1: Samuel Berman
* Student 2: Jim Guadagnoli

Specific implementation ownership will be divided as development begins. Until then, the agent edits outside the current session owner's area only when told explicitly.

Both partners should understand the integrated system and review work across the ownership boundary.