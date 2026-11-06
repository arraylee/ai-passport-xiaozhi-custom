// components/core/services/src/rtttl.c —— RTTTL 解析(纯逻辑,无 FPU)
#include "services/rtttl.h"
#include <limits.h>
#include <stddef.h>

static const int OCT4[12] = {262,277,294,311,330,349,370,392,415,440,466,494};

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static void skip_space(const char **pp, const char *end) {
    while (*pp < end && is_space(**pp)) ++*pp;
}

/* -1 means absent, -2 means overflow; neither may become a default value. */
static int rd_int(const char **pp, const char *end) {
    int v = 0;
    const char *p = *pp;
    if (p == end || !is_digit(*p)) return -1;
    while (p < end && is_digit(*p)) {
        int digit = *p - '0';
        if (v > (INT_MAX - digit) / 10) return -2;
        v = v * 10 + digit;
        ++p;
    }
    *pp = p;
    return v;
}

static int valid_duration(int d) {
    return d >= 1 && d <= 32 && (d & (d - 1)) == 0;
}

static int note_semi(char c) {
    switch (c) {
        case 'c': return 0; case 'd': return 2; case 'e': return 4; case 'f': return 5;
        case 'g': return 7; case 'a': return 9; case 'b': return 11; default: return -1;
    }
}

int rtttl_init(rtttl_t *r, const char *s, int len) {
    if (!r) return -1;
    *r = (rtttl_t){0};
    if (!s || len <= 0) return -1;
    const char *end = s + len, *p = s;
    /* Length-delimited transfers need no NUL; tolerate one terminator supplied
     * by a caller, but never an embedded NUL that would hide a song suffix. */
    if (end[-1] == '\0') --end;
    for (const char *q = s; q < end; ++q) if (*q == '\0') return -1;
    while (p < end && *p != ':') ++p;
    if (p == end) return -1;
    ++p;
    rtttl_t next = {.def_dur = 4, .def_oct = 6, .bpm = 63};
    unsigned seen = 0;
    skip_space(&p, end);
    while (p < end && *p != ':') {
        char key = *p++;
        unsigned bit = key == 'd' ? 1u : key == 'o' ? 2u : key == 'b' ? 4u : 0u;
        if (!bit || (seen & bit)) return -1;
        seen |= bit;
        skip_space(&p, end);
        if (p == end || *p++ != '=') return -1;
        skip_space(&p, end);
        int v = rd_int(&p, end);
        if (key == 'd') {
            if (!valid_duration(v)) return -1;
            next.def_dur = v;
        } else if (key == 'o') {
            if (v < 0 || v > 8) return -1;
            next.def_oct = v;
        } else {
            if (v < 25 || v > 900) return -1;
            next.bpm = v;
        }
        skip_space(&p, end);
        if (p == end) return -1;
        if (*p == ':') break;
        if (*p++ != ',') return -1;
        skip_space(&p, end);
        if (p == end || *p == ':') return -1;
    }
    if (p == end || *p++ != ':') return -1;
    skip_space(&p, end);
    if (p == end) return -1;
    next.p = p;
    next.end = end;
    next.whole_ms = 240000 / next.bpm;
    *r = next;
    return 0;
}

int rtttl_next(rtttl_t *r, int *freq_hz, int *ms) {
    if (!r || !freq_hz || !ms || !r->p || !r->end ||
        !valid_duration(r->def_dur) || r->def_oct < 0 || r->def_oct > 8 ||
        r->bpm < 25 || r->bpm > 900 || r->whole_ms != 240000 / r->bpm) return -1;
    const char *p = r->p, *end = r->end;
    skip_space(&p, end);
    if (p == end) { r->p = p; return 0; }
    if (p > end) return -1;

    int dur = rd_int(&p, end);
    if (dur == -1) dur = r->def_dur;
    if (!valid_duration(dur) || p == end) return -1;
    char nc = *p++;
    if (nc >= 'A' && nc <= 'Z') nc += 'a' - 'A';
    int semi = note_semi(nc);
    if (nc != 'p' && semi < 0) return -1;
    if (nc != 'p' && p < end && *p == '#') { ++semi; ++p; }

    int dot = 0, oct = r->def_oct;
    if (p < end && *p == '.') { dot = 1; ++p; }
    if (p < end && is_digit(*p)) {
        oct = rd_int(&p, end);
        if (oct < 0 || oct > 8) return -1;
    }
    if (p < end && *p == '.') {
        if (dot) return -1;
        dot = 1;
        ++p;
    }
    skip_space(&p, end);
    if (p < end) {
        if (*p++ != ',') return -1;
        skip_space(&p, end);
        if (p == end || *p == ',') return -1;
    }

    int freq = 0;
    if (nc != 'p') {
        int octave = oct + semi / 12;
        if (octave > 8) return -1;
        freq = OCT4[semi % 12];
        /* octave is now 0..8, so both shift counts and results are bounded. */
        if (octave > 4) freq <<= octave - 4;
        else if (octave < 4) freq >>= 4 - octave;
    }
    int duration = r->whole_ms / dur;
    if (dot) duration = duration * 3 / 2;
    *freq_hz = freq;
    *ms = duration;
    r->p = p;
    return 1;
}

int rtttl_validate(const char *s, int len) {
    rtttl_t r;
    if (rtttl_init(&r, s, len) < 0) return -1;
    int hz, ms, result, notes = 0;
    while ((result = rtttl_next(&r, &hz, &ms)) > 0) ++notes;
    return result == 0 && notes > 0 ? 0 : -1;
}
