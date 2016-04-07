#ifndef BEAMFORMING
#define BEAMFORMING

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

void get_delays(time_t unix_time, double ra, double dec, const struct Config * config, float * feed_positions, float * phases);

#ifdef __cplusplus
}
#endif

#endif