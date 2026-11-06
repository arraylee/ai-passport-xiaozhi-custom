// components/core/services/include/services/rtttl.h
#pragma once

typedef struct {
    const char *p, *end;
    int def_dur, def_oct, bpm, whole_ms;
} rtttl_t;

/* Bounded input, optionally ending in one NUL. Defaults are d=4,o=6,b=63.
 * Durations: 1,2,4,8,16,32; octaves: 0..8; BPM: 25..900.
 * Notes require comma separators; whitespace and either c.5 or c5. are valid.
 * init validates the header only. next returns 1/note, 0/end, -1/invalid. */
int rtttl_init(rtttl_t *r, const char *s, int len);
int rtttl_next(rtttl_t *r, int *freq_hz, int *ms);

/* Validate every note before saving or playing an untrusted song.
 * Returns 0 for a complete nonempty song, -1 for any invalid input. */
int rtttl_validate(const char *s, int len);
