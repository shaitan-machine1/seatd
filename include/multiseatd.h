#ifndef _SEATD_MULTISEATD_H
#define _SEATD_MULTISEATD_H

#include <stddef.h>
#include "protocol.h"

/* Private bootstrap helpers; not part of the installed libseat API. */
int multiseatd_service_name(const char *seat, char *service, size_t size);
int multiseatd_connect(char seat[MAX_SEAT_LEN]);

#endif
