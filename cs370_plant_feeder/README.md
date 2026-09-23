# Fault Aware Self Watering Plant System

CS370 Operating Systems Term Project

## Overview

This project is a fault aware self watering plant system built around a Raspberry Pi 5. The system is designed to monitor a plant and its water supply, make controlled watering decisions, and continue operating while the user is away.

The system uses a capacitive soil moisture sensor to monitor the condition of the soil and a HALJIA mini float switch to monitor whether water is available in the reservoir. These sensors cooperate when making watering decisions. Dry soil can create a request for water, but the system must also determine that water is available before allowing the pump to operate.

After a watering event, later soil moisture readings can be compared with the condition before watering to determine whether the plant responded as expected. This provides feedback about the watering process rather than assuming that activating the pump means the plant successfully received water.

## Hardware

The project uses the following hardware:

* Raspberry Pi 5
* ELEGOO UNO Super Starter Kit
* Capacitive soil moisture sensor
* HALJIA mini float switch
* Water reservoir
* Water pump
* Tubing
* Relay and control hardware
* Breadboard and jumper wires
* Resistors and diodes
* TIP120 transistors
* Two solenoid valves

The Raspberry Pi 5 is the primary computing platform for the project. The ELEGOO UNO Super Starter Kit provides additional electronic components for development, prototyping, and testing. The Arduino Uno is not intended to replace the Raspberry Pi as the primary project platform.

## System Design

The Raspberry Pi will collect information from both sensors and use that information to determine the current state of the watering system.

The soil moisture sensor determines whether watering should be considered. The reservoir float switch determines whether water is available. The pump will only be allowed to operate when the system determines that watering is appropriate and the reservoir contains enough water.

The software will maintain system state so that watering decisions are not based on a single instantaneous sensor reading. The system will also record important sensor observations, watering events, faults, and recovery events.

## Operating Systems Mechanisms

The first planned operating systems mechanism is Mechanism E, a multi process architecture using IPC and supervision. Sensor acquisition, system control, and supervision can be separated into processes that communicate with each other. This allows the system to detect and respond to a component failure instead of allowing one failed component to silently stop the entire device.

The second planned operating systems mechanism is Mechanism D, custom storage. The system will maintain persistent records of sensor observations, watering decisions, faults, and recovery events. The storage design will allow the behavior of the unattended system to be examined and evaluated.

The exact process structure, IPC method, storage format, and recovery behavior will be finalized as the system is designed and tested.

## Safety and Failure Handling

The normal safe state of the watering hardware is off. Missing, invalid, or unreliable sensor information should not cause the system to begin watering.

If the soil requires water but the reservoir float switch indicates that water is not available, the system will not activate the pump and will record the condition as a fault.

The system will also compare soil moisture readings before and after watering. If the expected change does not occur, the event can be recorded for further diagnosis rather than repeatedly activating the watering hardware without feedback.

## Development

The systems portion of the project will be developed in C using the requirements specified by the course.

Sensor thresholds, watering durations, timing values, and calibration values will be determined using measurements from the actual hardware rather than assumed in advance.

Testing will include normal operation as well as failure conditions so that the system behavior can be measured and evaluated before unattended operation.