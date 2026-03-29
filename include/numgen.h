#ifndef NUMGEN_H
#define NUMGEN_H

#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <string.h>

unsigned char random_byte(void);
uint16_t random_two_byte(void);
uint32_t random_four_byte(void);
uint64_t random_eight_byte(void);

#endif
