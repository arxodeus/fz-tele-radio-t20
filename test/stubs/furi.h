/* Minimal host stand-ins for the Furi API used by tele_radio_remote_radio.c. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define UNUSED(x) (void)(x)
#define furi_check(c)                                                        \
    do {                                                                     \
        if(!(c)) {                                                           \
            printf("furi_check failed %s:%d: %s\n", __FILE__, __LINE__, #c); \
            exit(2);                                                         \
        }                                                                    \
    } while(0)
void furi_delay_ms(uint32_t ms);
uint32_t furi_get_tick(void);
static inline uint32_t furi_ms_to_ticks(uint32_t ms) {
    return ms;
}
