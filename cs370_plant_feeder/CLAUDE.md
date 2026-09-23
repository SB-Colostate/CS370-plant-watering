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

The Raspberry Pi 5 is the primary computing platform. Components from the ELEGOO UNO Super Starter Kit may be used for development, prototyping, and supporting electronics as needed.

## Hard constraints

* The Raspberry Pi 5 is the primary project platform.
* The systems core must use the C standard required by the course and compile cleanly with `-Wall -Wextra -Werror`.
* At least two physically distinct sensors must cooperate in the same system behavior.
* The soil moisture sensor and reservoir float switch must both contribute to watering decisions.
* Do not reduce the project to a single soil moisture threshold that directly turns on the pump.
* The shipped product must not depend on an LLM or hosted or pretrained model.
* Hardware failures and system call failures must have explicit error handling where applicable.
* When the software cannot establish that watering is safe, the pump and valve should default to OFF.
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
* Preserve genuine raw Claude Code `.jsonl` sessions when they exist. Never fabricate a transcript.

## Build and test

Build, test, sanitizer, and soak test commands will be added when those commands actually exist in the repository.

Do not invent build or test commands for functionality that has not yet been implemented.

## Ownership

* Student 1: Samuel Berman
* Student 2: Jim Guadagnoli

Specific implementation ownership will be divided as development begins.

Both partners should understand the integrated system and review work across the ownership boundary.