#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint32_t duration; /* 0 = reset */
    bool level;
} LevelDuration;
static inline LevelDuration level_duration_make(bool level, uint32_t duration) {
    LevelDuration ld = {duration, level};
    return ld;
}
static inline LevelDuration level_duration_reset(void) {
    LevelDuration ld = {0, false};
    return ld;
}
static inline bool level_duration_is_reset(LevelDuration ld) {
    return ld.duration == 0;
}
static inline bool level_duration_get_level(LevelDuration ld) {
    return ld.level;
}
static inline uint32_t level_duration_get_duration(LevelDuration ld) {
    return ld.duration;
}
