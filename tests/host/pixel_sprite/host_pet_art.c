/* Host-only file loading. Firmware never copies the resource pack to heap. */
#include "presentation/bobo_art.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static void *host_pack;
__attribute__((constructor)) static void host_pet_art_load(void) {
    FILE *file = fopen(PET_ART_TEST_FILE, "rb"); assert(file);
    assert(!fseek(file, 0, SEEK_END)); long size = ftell(file); assert(size > 0 && size <= 0x100000);
    rewind(file); host_pack = malloc((size_t)size); assert(host_pack);
    assert(fread(host_pack, 1, (size_t)size, file) == (size_t)size); fclose(file);
    assert(bobo_art_bind(host_pack, (size_t)size)); assert(bobo_art_ready());
}
__attribute__((destructor)) static void host_pet_art_free(void) {
    bobo_art_unbind(); free(host_pack);
}
