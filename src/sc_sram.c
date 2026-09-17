/* The cartridge's save memory on disk -- see sc_sram.h. */
#include "sc_sram.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* Host frames the SRAM must stay unchanged before it is written. */
enum { kQuietFrames = 30 };

static uint8_t *s_ram;          /* the cart model's SRAM */
static uint32_t s_size;
static char s_path[1024];
static uint8_t *s_disk;         /* what the file holds */
static uint8_t *s_seen;         /* the SRAM as of the last tick */
static uint8_t *s_held;         /* the SRAM from before a state load */
static int s_quiet;
static bool s_active;
static bool s_write_failed;

static bool write_file(const char *path, const uint8_t *data, uint32_t size) {
  char tmp[1040];
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE *f = fopen(tmp, "wb");
  if (!f) return false;
  bool ok = fwrite(data, 1, size, f) == size;
  ok = fclose(f) == 0 && ok;
#ifdef _WIN32
  ok = ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING |
                                        MOVEFILE_WRITE_THROUGH) != 0;
#else
  ok = ok && rename(tmp, path) == 0;
#endif
  if (!ok) remove(tmp);
  return ok;
}

bool ScSram_Open(uint8_t *ram, uint32_t size, const char *path) {
  if (!ram || !size || !path || !*path) return false;
  snprintf(s_path, sizeof s_path, "%s", path);

  FILE *f = fopen(s_path, "rb");
  if (f) {
    long n = -1;
    if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
    if (n != (long)size) {
      fclose(f);
      fprintf(stderr, "sram: %s has %ld bytes, this cartridge %u -- the file is "
                      "left alone and saving is off\n", s_path, n, (unsigned)size);
      return false;
    }
    fseek(f, 0, SEEK_SET);
    const bool ok = fread(ram, 1, size, f) == size;
    fclose(f);
    if (!ok) {
      fprintf(stderr, "sram: could not read %s -- saving is off\n", s_path);
      return false;
    }
    char bak[1040];
    snprintf(bak, sizeof bak, "%s.bak", s_path);
    if (!write_file(bak, ram, size))
      fprintf(stderr, "sram: could not write the backup %s\n", bak);
    fprintf(stderr, "sram: loaded %s\n", s_path);
  } else {
    fprintf(stderr, "sram: no %s yet; it is written when the game first "
                    "stores something\n", s_path);
  }

  s_disk = (uint8_t *)malloc(size);
  s_seen = (uint8_t *)malloc(size);
  s_held = (uint8_t *)malloc(size);
  if (!s_disk || !s_seen || !s_held) {
    free(s_disk); free(s_seen); free(s_held);
    s_disk = s_seen = s_held = NULL;
    return false;
  }
  memcpy(s_disk, ram, size);
  memcpy(s_seen, ram, size);
  s_ram = ram;
  s_size = size;
  s_quiet = kQuietFrames;
  s_active = true;
  return true;
}

bool ScSram_Active(void) { return s_active; }

static void save_now(void) {
  if (write_file(s_path, s_ram, s_size)) {
    memcpy(s_disk, s_ram, s_size);
    s_write_failed = false;
    fprintf(stderr, "sram: saved %s\n", s_path);
  } else if (!s_write_failed) {
    s_write_failed = true;
    fprintf(stderr, "sram: could not write %s\n", s_path);
  }
}

void ScSram_Tick(void) {
  if (!s_active) return;
  if (memcmp(s_ram, s_seen, s_size) != 0) {
    memcpy(s_seen, s_ram, s_size);
    s_quiet = 0;
    return;
  }
  if (s_quiet >= kQuietFrames) return;
  if (++s_quiet < kQuietFrames) return;
  if (memcmp(s_ram, s_disk, s_size) != 0) {
    save_now();
    if (s_write_failed) s_quiet = 0;   /* try again in half a second */
  }
}

void ScSram_Flush(void) {
  if (s_active && memcmp(s_ram, s_disk, s_size) != 0) save_now();
}

void ScSram_Hold(void) {
  if (s_active) memcpy(s_held, s_ram, s_size);
}

void ScSram_Release(void) {
  if (!s_active) return;
  memcpy(s_ram, s_held, s_size);
  memcpy(s_seen, s_ram, s_size);
}
