#ifndef PLANT_PATHS_H
#define PLANT_PATHS_H

#include <stddef.h>

/* $HOME/CFG_DATA_DIR_DEFAULT. Returns 0, or -1 if $HOME is unset or the path is too long. */
int default_data_dir(char *out, size_t n);

#endif
