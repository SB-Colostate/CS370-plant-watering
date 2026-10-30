/* Process exit codes, shared so the supervisor can log why a child died. */
#ifndef PLANT_EXITCODES_H
#define PLANT_EXITCODES_H

#define EXIT_OK          0
#define EXIT_USAGE       1  /* bad arguments, config, or data dir already locked */
#define EXIT_SYSERR      2  /* system call or I/O failure */
#define EXIT_DEVICE      3  /* sensor device lost or silent too long */
#define EXIT_CORRUPT     4  /* storage cannot be recovered without guessing */

#endif
