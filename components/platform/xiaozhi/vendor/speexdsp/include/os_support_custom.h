#pragma once
#define OVERRIDE_SPEEX_ALLOC
#define OVERRIDE_SPEEX_ALLOC_SCRATCH
#define OVERRIDE_SPEEX_REALLOC
#define OVERRIDE_SPEEX_FREE
#define OVERRIDE_SPEEX_FREE_SCRATCH
#define OVERRIDE_SPEEX_FATAL
#define OVERRIDE_SPEEX_WARNING
#define OVERRIDE_SPEEX_WARNING_INT
void *xz_speex_alloc(int bytes);
void *xz_speex_realloc(void *ptr, int bytes);
void xz_speex_free(void *ptr);
_Noreturn void xz_speex_fatal(const char *text, const char *file, int line);
#define speex_alloc xz_speex_alloc
#define speex_alloc_scratch xz_speex_alloc
#define speex_realloc xz_speex_realloc
#define speex_free xz_speex_free
#define speex_free_scratch xz_speex_free
#define _speex_fatal xz_speex_fatal
static inline void speex_warning(const char *text) { (void)text; }
static inline void speex_warning_int(const char *text, int value) { (void)text; (void)value; }
