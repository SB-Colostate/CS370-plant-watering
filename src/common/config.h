/* All tunable paths, rates, and timeouts live here (CLAUDE.md: keep them centralized).
 * Values marked UNMEASURED are design targets from DESIGN.md, not hardware-verified numbers. */
#ifndef PLANT_CONFIG_H
#define PLANT_CONFIG_H

/* Uno serial link. by-id survives re-enumeration (ttyACM0 -> ttyACM1 after an unplug). */
#define CFG_SERIAL_DEV  "/dev/serial/by-id/usb-Arduino__www.arduino.cc__0043_43439343134351905243-if00"
#define CFG_SERIAL_BAUD 9600
#define CFG_LINE_MAX    64          /* bytes; longer lines are discarded */

/* Sampler: exit (code 3) if no valid frame arrives for this long. Uno sends ~1 line / 1.1 s. */
#define CFG_FRAME_TIMEOUT_MS 5000
/* Opening the port resets the Uno (~2 s boot) and the first line is discarded. UNMEASURED. */
#define CFG_FIRST_FRAME_TIMEOUT_MS 10000

/* Storage (mechanism D). DESIGN §4: fsync every 10 s or 50 records. UNMEASURED. */
#define CFG_DATA_DIR_DEFAULT "plant-data"   /* relative to $HOME */
#define CFG_LOG_FILE         "plant.plog"
#define CFG_LOCK_FILE        "LOCK"
#define CFG_SOCK_FILE        "hub.sock"
#define CFG_SYNC_MAX_RECORDS 50
#define CFG_SYNC_MAX_AGE_MS  10000

/* Supervisor (mechanism E). DESIGN §2: backoff 1 s -> 60 s. UNMEASURED. */
#define CFG_HEARTBEAT_TIMEOUT_MS 5000
#define CFG_BACKOFF_MIN_MS       1000
#define CFG_BACKOFF_MAX_MS       60000
#define CFG_STABLE_UPTIME_MS     60000  /* backoff resets after this much uptime */
#define CFG_STATUS_PERIOD_MS     60000  /* heartbeat line with RSS to plant.log */
#define CFG_STOP_GRACE_MS        3000   /* SIGTERM -> SIGKILL */

/* Pump safety (CLAUDE.md hard constraint): no single pump run may exceed 10 minutes. Enforced by
 * the pump-control layer and the supervisor; the engine only refuses to ask for more.
 * Never raise without both partners agreeing. Tests may lower it (in their own params). */
#define CFG_PUMP_CAP_MS 600000

/* Watering engine (src/analysis). PLACEHOLDERS, NOT MEASUREMENTS: each value must be replaced
 * from calibration on the real pot, probe and tank before it is trusted.
 * Soil is raw ADC counts; our module reads ~0 dry and rises when wet.
 * Level is the HC-SR04 DISTANCE to the water in mm: larger distance = less water. */
#define CFG_ENG_SOIL_DRY_ENTER      40      /* UNMEASURED placeholder */
#define CFG_ENG_SOIL_DRY_EXIT       80      /* UNMEASURED placeholder */
#define CFG_ENG_SOIL_RAIL_HIGH      1023    /* ADC full scale */
#define CFG_ENG_DRY_DWELL_MS        600000  /* 10 min; UNMEASURED */
#define CFG_ENG_LEVEL_EMPTY_DIST_MM 200     /* UNMEASURED placeholder: depends on the tank */
#define CFG_ENG_LEVEL_FULL_DIST_MM  40      /* UNMEASURED; HC-SR04 is noisy under ~20 mm (DESIGN §5) */
#define CFG_ENG_LEVEL_RESERVE_MM    20      /* UNMEASURED placeholder */
#define CFG_ENG_LEVEL_HYST_MM       10      /* UNMEASURED placeholder */
#define CFG_ENG_LEVEL_CONFIRM       3       /* filtered readings in a row (~3 s) to change state */
#define CFG_ENG_NO_ECHO_RUN         3       /* no-echo readings in a row before LEVEL_NO_ECHO */
#define CFG_ENG_SOIL_RAIL_RUN       5       /* readings in a row at the rail before SOIL_RAIL */
#define CFG_ENG_DOSE_MS             20000   /* UNMEASURED: pump not on the bench yet */
#define CFG_ENG_MIN_INTERVAL_MS     3600000 /* DESIGN §3: at most 1 watering per hour */
#define CFG_ENG_DAILY_DOSE_MS       120000  /* UNMEASURED placeholder */
#define CFG_ENG_START_CONFIRM_MS    3000    /* UNMEASURED: pump control must confirm a start */
#define CFG_ENG_STOP_GRACE_MS       3000    /* UNMEASURED: ...and a stop */
#define CFG_ENG_STALE_MS            5000    /* DESIGN §6 #4 target: fault noticed within 5 s */
#define CFG_ENG_WARMUP_MS           30000   /* UNMEASURED */

#endif
