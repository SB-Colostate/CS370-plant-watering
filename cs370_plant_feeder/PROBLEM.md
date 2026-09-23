# Problem Memo: Fault Aware Self Watering Plant System

## The user

The user of this device will be Sam, who keeps plants at his home in Florida and periodically travels for remote work and vacations. The device will remain with a plant while Sam is away so that its condition can be monitored and watering can occur without requiring him to be physically present.

## The problem

Sam needs a reliable way to keep his plants watered while he is away from home. The problem is not simply detecting dry soil. A basic automatic watering system can turn on a pump when a moisture reading crosses a threshold, but that does not establish whether water is available or whether the watering attempt had the expected effect.

This project will address that problem by monitoring both the plant and its water supply, making controlled watering decisions, and maintaining enough system state to detect and report abnormal behavior rather than repeatedly watering without feedback.

## Why a device

The system must remain physically present with the plant because it needs to measure the soil, monitor the water reservoir, and physically deliver water. A phone application alone cannot perform these actions.

The Raspberry Pi can remain next to the plant and continue sensing, logging, making watering decisions, and controlling the watering hardware while Sam is away or not actively interacting with the system.

## The sensors

The first sensor is a capacitive soil moisture sensor, which measures the condition the watering system is intended to affect. The second sensor is a HALJIA mini float switch installed in the water reservoir, which indicates whether enough water is available for a watering attempt.

The sensors cooperate in the watering decision. Dry soil can create a request for water, but the controller must also determine that water is available before allowing the pump to operate. After watering, later soil moisture readings can be compared with the condition before watering to determine whether the plant responded as expected.

This allows the software to distinguish between multiple system states instead of treating every low soil moisture reading as an unconditional command to run the pump.

## The mechanisms

Our first mechanism choice is a multi process architecture with IPC and supervision, Mechanism E. Sensor acquisition, system control, and supervision can be separated so that the failure of one component is observable and can be handled instead of silently stopping the entire device. The exact process boundaries and IPC mechanism will be finalized during the design phase.

Our second initial choice is custom storage, Mechanism D. The system needs a persistent record of sensor observations, watering decisions, faults, and recovery events so that unattended behavior can be evaluated afterward. The storage format and recovery strategy will be designed and measured during later milestones.

## The risk

The largest technical risk is obtaining reliable sensor readings that are good enough to make safe watering decisions. Soil moisture readings can vary with sensor placement and physical conditions, while the float switch only reports the water level at its installed position.

We will reduce this risk by collecting measurements from the actual hardware before selecting final thresholds and timing values, using multiple observations rather than trusting a single instantaneous soil reading, and keeping the watering system off when there is not enough information to determine that watering is appropriate.