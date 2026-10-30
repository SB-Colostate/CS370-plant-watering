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

#endif
