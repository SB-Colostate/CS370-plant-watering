# Problem memo -- Fault Aware Self Watering Plant System (Samuel Berman, Jim Guadagnoli)

## The user

Us: Sam, who keeps potted plants at his home in Florida and regularly travels for remote work and vacations, and Jim, who keeps potted plants at his home in Fort Collins, Colorado, and is also away from home for stretches. Each of us will put one device next to one plant and its water reservoir at our own home and rely on it while we are gone.

Two homes in two very different climates is a feature for the design: a humid Florida summer and a dry, high-altitude Colorado home dry the same pot out at very different rates, which is exactly why a fixed watering schedule fails and why the device has to learn each plant's own dry-down rate.

<!-- TODO(Sam, Jim): each add the real numbers for yourself: roughly how many trips per year, typical length, and what has actually happened to plants during past trips (lost plants, a neighbor asked to water, etc.). Do not estimate; use what really happened. -->

## The problem

Neither of us can keep our plants watered while we are gone. The obvious fix, a timer or a moisture threshold wired to a pump, fails in ways nobody sees until we get home:

* **The reservoir runs dry.** The pump keeps "watering" with no water, the plant dries out, and the pump can run dry.
* **Water is delivered but does not reach the roots.** A kinked or clogged line, or a stuck valve, delivers nothing. Very dry peat based potting mix also becomes hydrophobic: water channels down the inside of the pot and out the drainage hole while the root ball stays dry.
* **Overwatering.** A fixed schedule keeps watering soil that is still wet. Saturated media for days leads to root rot, which kills plants as reliably as drought and is less visible.
* **The sensor fails.** A disconnected or corroded moisture probe reads as permanently dry (the pump runs until the reservoir is empty) or permanently wet (the plant is never watered).

Each of these ends with a dead or damaged plant and the same question: what went wrong while I was away?

**What we will build:** a device that waters on demand, not on a clock. There is no watering schedule. The Pi continuously reads the soil moisture sensor, waters only when the soil has actually dried to the point the plant needs water, and then checks that the water arrived. A plant that is drying slowly (cool week, humid weather) gets watered less often; one drying fast (hot, dry week) gets watered more often, with no settings changed.

## Why a device

The failures above happen on their own schedule, often overnight, when nobody is looking. Something has to be physically next to the plant, awake all the time, to measure the soil, check the reservoir, and open or close the water supply. A phone app cannot move water, and consumer Bluetooth plant sensors only report readings when a phone is nearby and someone reads them, which is exactly what is not happening while we travel. Wi-Fi watering timers water on a schedule without knowing whether the water arrived. The device must make and check its own decisions with the network unplugged.

## The sensors

A capacitive soil moisture sensor (read through an ADC, since the Pi has no analog inputs) and a HALJIA float switch in the reservoir. Neither sensor alone can tell the failures apart. Together, and over time, they can:

| Soil moisture after a watering attempt | Float switch | Diagnosis | Action |
|---|---|---|---|
| Rises and holds | Water present | Watering worked | Normal |
| No rise | Water present | Delivery fault (clog, kink, valve) | Stop retrying, log fault |
| No rise | Dropped to empty | Reservoir ran dry | Lock out pump, log fault |
| Rises, then falls unusually fast | Water present | Channeling / runoff, possible leak | Log, change watering pattern |
| Stays wet for a long time | any | Overwatering risk | Suppress watering |
| Flat-lined or pinned at a rail | any | Moisture sensor fault | Fail safe (valve OFF), log fault |

The soil moisture sensor is the trigger: watering is requested only when the rolling soil reading shows the pot has dried past the plant's needs (calibrated on the real pot and mix, not guessed), never because a timer fired. The float switch must then confirm water is available before the valve or pump may run, and the soil response afterward confirms or rejects the attempt. That is a state machine over two sensors and time, not a threshold.

## The mechanisms

**E, multi process architecture with supervision.** A controller that silently dies while we are away means a dead plant, and a hung actuator process could leave water running. Sensing, control, and actuation run as separate processes under a supervisor that restarts failed children and forces the actuator OFF when it cannot confirm it is safe.

**D, custom storage.** Our first question when we get home is what happened while we were gone, and each plant's watering history (baseline dry-down rate, past responses) must survive power cuts, such as Florida summer storms. An append-only, crash-consistent log of readings, decisions, and faults answers both.

## The risk

Getting soil moisture readings reliable enough to diagnose from. Capacitive readings depend on probe depth, soil type, temperature, and how firmly the probe is seated, and the float switch reports only one bit at its mounting height. If we cannot tell a real "no rise" from sensor noise, the diagnoses above collapse into guesses. We will calibrate against the actual pot and mix (bone dry and freshly saturated), use rolling statistics over many samples rather than single readings, and default to OFF whenever the data does not support watering.
