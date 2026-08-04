/* main.c -- SimCitySNESRecomp desktop host.
 *
 * Phase-1 bring-up: drives the ROM entirely through the shared runner's
 * standalone 65816 interpreter (interp816) over the real device models
 * (snes.c/ppu.c/apu.c/dma.c/cart.c), exactly the "LLE-first" correctness
 * baseline snesrecomp/docs/LLE_FIRST_ANALYSIS.md describes as authoritative
 * for every game before any AOT bank is layered on top. No SimCity-specific
 * addresses or scheduler knowledge are required for this milestone: the
 * host runs a fixed number of accurate H/V master-clock ticks per video
 * frame and pauses, the same frame-boundary technique snesrecomp's own
 * game-neutral reference driver (snesrecomp/cosim/ref_driver.c) uses.
 *
 * Wiring the AOT/CpuState hybrid tier (common_cpu_infra.c's SnesInit /
 * RtlRegisterGame contract, interp_bridge.c's compiled<->interpreted
 * bouncing) is the documented next step once SimCity's own scheduler idiom
 * is understood well enough to declare it safely -- see README.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <SDL.h>

#include "snes/snes.h"
#include "snes/apu.h"
#include "snes/dsp.h"
#include "snes/dsp_shadow.h"
#include "snes/spc.h"
#include "snes/ppu.h"
#include "snes/dma.h"
#include "snes/cart.h"
#include "snes/dsp1.h"
#include "snes/interp816.h"
#include "types.h"

/* ── globals the shared runner device sources reference ─────────────────── */
uint8_t    g_ram[0x20000];
Snes      *g_snes;
Ppu       *g_ppu;
static Interp816 *g_cpu;
static uint64_t   g_master_cycles;

/* ── RTL glue the device sources call. This host bypasses common_rtl.c (the
 * AOT/CpuState runtime) entirely for Phase 1, so these are the same
 * minimal no-op/direct implementations snesrecomp's own reference driver
 * uses (cosim/ref_driver.c) rather than a SimCity-specific reinterpretation. */
void RtlApuLock(void)   {}
void RtlApuUnlock(void) {}
void rtl_sync_apu_to_cpu_locked(void) {}
void RtlApuWrite(uint16 adr, uint8 val) { g_snes->apu->inPorts[adr & 3] = val; }
void rtl_accumulate_apu_catchup(void) {}
void NORETURN Die(const char *e) { fprintf(stderr, "FATAL: %s\n", e ? e : "(null)"); exit(1); }
void debug_on_wram_write_byte(uint32_t a, uint8_t o, uint8_t n) { (void)a; (void)o; (void)n; }
void debug_on_wram_write_word(uint32_t a, uint16_t o, uint16_t n) { (void)a; (void)o; (void)n; }
bool g_fail = false;
uint8 g_snesrecomp_last_hdmaen;
/* Referenced by snes.c/interp_bridge.c; only meaningful once the AOT/
 * CpuState hybrid tier (interp_bridge.c) is wired in. 0 = not driving. */
int g_interp_apu_driving = 0;
void ppudma_record_dma(int ch, int fromB, uint8_t aBank, uint16_t aAdr,
                       uint8_t bAdr, uint16_t size) {
  (void)ch; (void)fromB; (void)aBank; (void)aAdr; (void)bAdr; (void)size;
}
int interp816_opcode_hook(uint32_t addr) { (void)addr; return 0; }
DspShadow *dsp_shadow_create(void) { return NULL; }
void dsp_shadow_free(DspShadow *sh) { (void)sh; }
void dsp_shadow_process(DspShadow *sh, Dsp *dsp, int cL, int cR, int *oL, int *oR) {
  (void)sh; (void)dsp; *oL = cL; *oR = cR;
}
void dsp_shadow_verify_brr(const uint8_t *aram, uint16_t bs, int a, int b, const int16_t *c) {
  (void)aram; (void)bs; (void)a; (void)b; (void)c;
}
void dsp_shadow_verify_echo(const int16_t *l, const int16_t *r, const int8_t *co,
                            int idx, int sL, int sR) {
  (void)l; (void)r; (void)co; (void)idx; (void)sL; (void)sR;
}

static uint64_t s_frames;
static uint64_t s_nmi_requests;
static uint64_t s_nmi_serviced;

/* ── debugging tools (env-gated, zero cost when unset) ───────────────────
 * SC_IO_TRACE=<end-frame>   log every joypad-register read/write ($4016/17,
 *                           $4200, $4218-421b) up to that frame.
 * SC_PC_TRACE=<frame>       on the first $4218 read at/after that frame,
 *                           dump the next 60 executed PCs + registers --
 *                           useful for finding what code just consumed a
 *                           controller read.
 * SC_DEBUG=<interval>       every <interval> frames (default 60), log
 *                           cpu.pc plus $011b (P1 held state), $01df
 *                           (screen-mode index), $0c0f (cursor-mover gate),
 *                           $020d (selected build tool) -- see
 *                           docs/INVESTIGATION_dpad.md.
 * SC_DUMP_AT=<frame> + SC_DUMP_PATH=<file.ppm>   write that frame's
 *                           rendered framebuffer as a P6 PPM.
 * SC_ADDR_TRACE=<bank:addr>[,<bank:addr>...][@<start-frame>]
 *                           log (rate-limited, 200 hits per address) every
 *                           time execution reaches any of the given
 *                           bank:addr PCs, with full registers -- the
 *                           general "is this code path ever reached, and
 *                           with what state" tool. Optional @<start-frame>
 *                           delays tracing until that frame. */
static uint64_t s_io_trace_until;
static int s_pc_capture_after = -1;
static uint64_t s_pc_trace_at_frame;
#define SC_ADDR_TRACE_MAX 64
static uint32_t s_addr_trace_pcs[SC_ADDR_TRACE_MAX];   /* (bank<<16)|addr */
static uint32_t s_addr_trace_hits[SC_ADDR_TRACE_MAX];
static int s_addr_trace_count;
static uint64_t s_addr_trace_start_frame;
#define SC_PC_HISTORY_SIZE 128
static uint32_t s_pc_history[SC_PC_HISTORY_SIZE]; /* ring buffer of (bank<<16)|pc, one entry per executed opcode */
static int s_pc_history_head;
static int s_pc_history_filled;
/* SC_PC_BITMAP_BANK=<bank hex>|all + SC_PC_BITMAP_PATH=<file> [+ SC_PC_BITMAP_START=<frame>]:
 * records a 1-bit-per-address "was this PC ever executed" bitmap over the given
 * bank's 32KB ROM window ($8000-$ffff) -- or, with "all", over every bank
 * $00-$3F at once (16x4KB) -- dumped to file at exit (both --qualify and
 * windowed/interactive mode, so this also works live: launch, play, close the
 * window, the dump is written on exit same as qualify-exit). Meant to be
 * diffed between two runs (e.g. direction held vs not) to find exactly which
 * code paths differ, instead of guessing candidate addresses from static
 * disassembly. */
static int s_pc_bitmap_bank = -1; /* -1 = off, -2 = all banks, else single bank */
static uint64_t s_pc_bitmap_start_frame;
static uint8_t s_pc_bitmap[4096];
static uint8_t s_pc_bitmap_all[64][4096];
static uint64_t s_banks_seen; /* bit N set if bank N ever held cpu->k (diagnostic only) */
static uint8_t bus_read(void *mem, uint32_t adr) {
  (void)mem;
  uint8_t v = snes_read(g_snes, adr);
  uint16_t reg = (uint16_t)adr;
  uint8_t bank = (uint8_t)(adr >> 16);
  bool hw = bank < 0x40 || (bank >= 0x80 && bank < 0xc0);
  if (s_io_trace_until && s_frames < s_io_trace_until && hw &&
      (reg == 0x4016 || reg == 0x4017 || reg == 0x4200 ||
       reg == 0x4218 || reg == 0x4219 || reg == 0x421a || reg == 0x421b))
    fprintf(stderr, "[io f=%llu] READ  %04x = %02x\n",
            (unsigned long long)s_frames, reg, v);
  if (hw && reg == 0x4218 && s_pc_capture_after == -2 && s_frames >= s_pc_trace_at_frame) {
    s_pc_capture_after = 60;
    fprintf(stderr, "[pctrace f=%llu] $4218 read = %02x -- capturing next 60 PCs\n",
            (unsigned long long)s_frames, v);
  }
  if (s_addr_trace_count && s_frames >= s_addr_trace_start_frame &&
      (reg == 0x011b || reg == 0x011c)) {
    static uint32_t s_read_watch_hits;
    if (s_read_watch_hits < 400) {
      fprintf(stderr, "[readwatch f=%llu] pc=%02x:%04x READ $%04x = %02x\n",
              (unsigned long long)s_frames, g_cpu->k, g_cpu->pc, reg, v);
      s_read_watch_hits++;
    }
  }
  return v;
}
static void bus_write(void *mem, uint32_t adr, uint8_t v) {
  (void)mem;
  uint16_t reg = (uint16_t)adr;
  uint8_t bank = (uint8_t)(adr >> 16);
  bool hw = bank < 0x40 || (bank >= 0x80 && bank < 0xc0);
  if (s_io_trace_until && s_frames < s_io_trace_until && hw &&
      (reg == 0x4016 || reg == 0x4017 || reg == 0x4200 ||
       reg == 0x4218 || reg == 0x4219 || reg == 0x421a || reg == 0x421b))
    fprintf(stderr, "[io f=%llu] WRITE %04x = %02x\n",
            (unsigned long long)s_frames, reg, v);
  if (s_addr_trace_count && s_frames >= s_addr_trace_start_frame &&
      (reg == 0x01eb || reg == 0x01ec || reg == 0x01ed || reg == 0x01ee)) {
    static uint32_t s_wram_watch_hits;
    if (s_wram_watch_hits < 200) {
      fprintf(stderr, "[wramwrite f=%llu] pc=%02x:%04x $%04x = %02x\n",
              (unsigned long long)s_frames, g_cpu->k, g_cpu->pc, reg, v);
      s_wram_watch_hits++;
    }
  }
  snes_write(g_snes, adr, v);
}

/* SPC cycles per master clock (LakeSnes: (32040*32)/(1364*262*60)). */
static const double kApuCyclesPerMaster = (32040.0 * 32.0) / (1364.0 * 262.0 * 60.0);

enum { kVideoWidth = 256, kVideoHeight = 224, kVideoPitch = kVideoWidth * 4 };
static uint8_t s_video_pixels[kVideoPitch * kVideoHeight];

/* `input1_currentState`'s bit layout is NOT the plain hardware joypad
 * register layout. The shared runner's auto-joy-read path (snes.c) does
 * `$4218 = SwapInputBits(input1_currentState) & 0xff; $4219 = ... >> 8`,
 * where SwapInputBits reverses all 16 bits. So bit i of input1_currentState
 * ends up at bit (15-i) of the value $4218/4219 are split from. Working
 * backwards from the real hardware $4218 (bit7=B..bit0=Right) / $4219
 * (bit7=A,6=X,5=L,4=R) layout the game actually reads, the constants below
 * are what must be set in input1_currentState -- verified empirically
 * against SimCity itself (a literal 0x1000 here reads back as "Up" at
 * $4218, not "Start", confirming the derivation). Do not "simplify" these
 * to the naive hardware bit order -- that was the original bug. */
enum {
  kPad_Right = 0x8000, kPad_Left = 0x4000, kPad_Down = 0x2000, kPad_Up = 0x1000,
  kPad_Start = 0x0800, kPad_Select = 0x0400, kPad_Y = 0x0200, kPad_B = 0x0100,
  kPad_R = 0x0008, kPad_L = 0x0004, kPad_X = 0x0002, kPad_A = 0x0001,
};

/* ── accurate H/V position driver, ported from snesrecomp/cosim/ref_driver.c
 * (the framework's own game-neutral reference frame loop) ──────────────── */
static void handle_pos_stuff(void) {
  Snes *snes = g_snes;
  Interp816 *cpu = g_cpu;
  if (snes->autoJoyTimer)
    snes->autoJoyTimer = snes->autoJoyTimer <= 2 ? 0 : (uint16_t)(snes->autoJoyTimer - 2);

  if (snes->vIrqEnabled && snes->hIrqEnabled) {
    if (snes->vPos == (snes->vTimer + 1) && snes->hPos == (4 * snes->hTimer)) {
      snes->inIrq = true; cpu->irqWanted = true;
    }
  } else if (snes->vIrqEnabled && !snes->hIrqEnabled) {
    if (snes->vPos == (snes->vTimer + 1) && snes->hPos == 1024) {
      snes->inIrq = true; cpu->irqWanted = true;
    }
  } else if (!snes->vIrqEnabled && snes->hIrqEnabled) {
    if (snes->hPos == (4 * snes->hTimer)) { snes->inIrq = true; cpu->irqWanted = true; }
  }

  if (snes->hPos == 0) {
    bool startingVblank = false;
    if (snes->vPos <= kVideoHeight)
      ppu_runLine(g_ppu, snes->vPos);
    if (snes->vPos == 0) {
      snes->inVblank = false; snes->inNmi = false;
      dma_startDma(snes->dma, 0, true);
    } else if (snes->vPos == 225) {
      startingVblank = !ppu_checkOverscan(g_ppu);
    } else if (snes->vPos == 240) {
      if (!snes->inVblank) startingVblank = true;
    }
    if (startingVblank) {
      ppu_handleVblank(g_ppu);
      snes->inVblank = true;
      snes->inNmi = true;
      if (snes->nmiEnabled) { cpu->nmiWanted = true; s_nmi_requests++; }
      if (snes->autoJoyRead) snes->autoJoyTimer = 4224;
    }
  } else if (snes->hPos == 1024) {
    if (!snes->inVblank) dma_cycle(snes->dma);
  }

  snes->hPos += 2;
  if (snes->hPos == 1364) {
    snes->hPos = 0;
    snes->vPos++;
    if (snes->vPos == 262) { snes->vPos = 0; s_frames++; }
  }
}

static void parse_addr_trace(const char *spec) {
  const char *at = strchr(spec, '@');
  char buf[256];
  size_t len = at ? (size_t)(at - spec) : strlen(spec);
  if (len >= sizeof(buf)) len = sizeof(buf) - 1;
  memcpy(buf, spec, len);
  buf[len] = '\0';
  if (at) s_addr_trace_start_frame = strtoull(at + 1, NULL, 0);
  char *tok = strtok(buf, ",");
  while (tok && s_addr_trace_count < SC_ADDR_TRACE_MAX) {
    unsigned bank = 0, addr = 0;
    if (sscanf(tok, "%x:%x", &bank, &addr) == 2)
      s_addr_trace_pcs[s_addr_trace_count++] = (bank << 16) | addr;
    tok = strtok(NULL, ",");
  }
}

static bool run_one_frame(void) {
  Snes *snes = g_snes;
  Interp816 *cpu = g_cpu;
  uint64_t target = s_frames + 1;
  long guard = 20000000; /* runaway guard: caps opcodes/frame, mirrors ref_driver.c */
  while (s_frames < target && guard-- > 0) {
    if (cpu->k == 0x00 && cpu->pc == 0x80b2) s_nmi_serviced++;
    if (s_addr_trace_count && s_frames >= s_addr_trace_start_frame) {
      uint32_t pc = ((uint32_t)cpu->k << 16) | cpu->pc;
      for (int i = 0; i < s_addr_trace_count; i++) {
        if (s_addr_trace_pcs[i] == pc && s_addr_trace_hits[i] < 200) {
          fprintf(stderr, "[addrtrace f=%llu] pc=%02x:%04x a=%04x x=%04x y=%04x "
                  "s=%04x d=%04x db=%02x m%s x%s hit#%u\n",
                  (unsigned long long)s_frames, cpu->k, cpu->pc, cpu->a, cpu->x,
                  cpu->y, cpu->sp, cpu->dp, cpu->db, cpu->mf ? "8" : "16",
                  cpu->xf ? "8" : "16", s_addr_trace_hits[i]);
          if (s_addr_trace_hits[i] < 3) {
            fprintf(stderr, "  pc history (oldest..newest, this pc last):\n");
            int n = s_pc_history_filled;
            for (int h = n - 1; h >= 0; h--) {
              int pos = ((s_pc_history_head - 1 - h) % SC_PC_HISTORY_SIZE + SC_PC_HISTORY_SIZE) % SC_PC_HISTORY_SIZE;
              uint32_t hpc = s_pc_history[pos];
              fprintf(stderr, "    %02x:%04x\n", (unsigned)(hpc >> 16) & 0xff, (unsigned)(hpc & 0xffff));
            }
          }
          s_addr_trace_hits[i]++;
        }
      }
    }
    if (s_addr_trace_count) {
      s_pc_history[s_pc_history_head] = ((uint32_t)cpu->k << 16) | cpu->pc;
      s_pc_history_head = (s_pc_history_head + 1) % SC_PC_HISTORY_SIZE;
      if (s_pc_history_filled < SC_PC_HISTORY_SIZE) s_pc_history_filled++;
    }
    if (s_pc_bitmap_bank >= 0 && cpu->k == s_pc_bitmap_bank && cpu->pc >= 0x8000 &&
        s_frames >= s_pc_bitmap_start_frame) {
      uint32_t idx = cpu->pc - 0x8000;
      s_pc_bitmap[idx >> 3] |= (uint8_t)(1u << (idx & 7));
    }
    if (s_pc_bitmap_bank == -2 && cpu->pc >= 0x8000 && cpu->k < 64 &&
        s_frames >= s_pc_bitmap_start_frame) {
      uint32_t idx = cpu->pc - 0x8000;
      s_pc_bitmap_all[cpu->k][idx >> 3] |= (uint8_t)(1u << (idx & 7));
    }
    if (cpu->k < 64) s_banks_seen |= (1ULL << cpu->k);
    if (s_pc_capture_after > 0) {
      fprintf(stderr, "[pctrace] pc=%02x:%04x a=%04x x=%04x y=%04x p=%02x%s%s\n",
              cpu->k, cpu->pc, cpu->a, cpu->x, cpu->y,
              interp816_getFlags(cpu), cpu->mf ? " m8" : " m16", cpu->xf ? " x8" : " x16");
      s_pc_capture_after--;
    }
    int cyc = interp816_runOpcode(cpu);
    if (cyc <= 0) cyc = 1;
    int master = cyc * 8;
    g_master_cycles += (uint64_t)master;
    for (int i = 0; i < master; i += 2) handle_pos_stuff();
    snes->apuCatchupCycles += (double)master * kApuCyclesPerMaster;
    snes_catchupApu(snes);
  }
  return guard > 0;
}

static bool write_ppm(const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  fprintf(f, "P6\n%d %d\n255\n", kVideoWidth, kVideoHeight);
  const uint32_t *pixels = (const uint32_t *)s_video_pixels;
  for (size_t i = 0; i < (size_t)kVideoWidth * kVideoHeight; i++) {
    uint8_t rgb[3] = { (uint8_t)(pixels[i] >> 16), (uint8_t)(pixels[i] >> 8), (uint8_t)pixels[i] };
    if (fwrite(rgb, 1, 3, f) != 3) { fclose(f); return false; }
  }
  return fclose(f) == 0;
}

static bool write_wram_dump(const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  bool ok = fwrite(g_ram, 1, sizeof(g_ram), f) == sizeof(g_ram);
  return fclose(f) == 0 && ok;
}

static uint8_t *read_file(const char *path, uint32_t *size_out) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  if (n <= 0) { fclose(f); return NULL; }
  uint8_t *b = (uint8_t *)malloc((size_t)n);
  if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
  fclose(f);
  if (b) *size_out = (uint32_t)n;
  return b;
}

/* ── synthetic input injection for headless repro (mirrors the --input
 * flag in snesrecomp/cosim/ref_driver.c) -- lets a specific controller
 * press be reproduced deterministically at an exact frame, instead of
 * guessing from an interactive session. ─────────────────────────────── */
typedef struct InputEvent { uint64_t start, duration; uint16_t mask; } InputEvent;
static InputEvent s_input_events[64];
static uint32_t s_input_event_count;

static bool add_input_event(const char *text) {
  unsigned long long start = 0, duration = 0;
  unsigned mask = 0;
  char trailing = '\0';
  if (s_input_event_count >= 64 ||
      sscanf(text, "%llu:%llu:%x%c", &start, &duration, &mask, &trailing) != 3 ||
      !duration || mask > 0xffffu)
    return false;
  s_input_events[s_input_event_count++] = (InputEvent){start, duration, (uint16_t)mask};
  return true;
}

static void apply_frame_input(uint64_t frame) {
  uint16_t input = 0;
  for (uint32_t i = 0; i < s_input_event_count; i++) {
    InputEvent *e = &s_input_events[i];
    if (frame >= e->start && frame - e->start < e->duration) input |= e->mask;
  }
  g_snes->input1_currentState = input;
}

/* ── generic activity qualification (--qualify N): the same pass/fail bar
 * as snesrecomp/cosim/ref_driver.c's standalone mode -- "goes through the
 * attract demo without logic, video, or audio errors" made concrete and
 * automatable, with zero SimCity-specific WRAM knowledge required. ─────── */
static void write_pc_bitmap_dump(void) {
  if (s_pc_bitmap_bank == -1) return;
  const char *path = getenv("SC_PC_BITMAP_PATH");
  if (!path) return;
  FILE *f = fopen(path, "wb");
  if (!f) return;
  if (s_pc_bitmap_bank == -2)
    fwrite(s_pc_bitmap_all, 1, sizeof(s_pc_bitmap_all), f);
  else
    fwrite(s_pc_bitmap, 1, sizeof(s_pc_bitmap), f);
  fclose(f);
}

static int run_qualification(uint64_t frames) {
  uint64_t logic_changes = 0, video_changes = 0, audio_active_frames = 0;
  uint64_t last_ram_hash = 0, last_video_hash = 0;
  uint32_t last_sample_write = 0;
  int16_t audio_buf[1024 * 2];

  uint64_t stall_run = 0, stall_max = 0;
  for (uint64_t f = 0; f < frames; f++) {
    apply_frame_input(f);
    if (!run_one_frame()) {
      fprintf(stderr, "qualify: opcode guard tripped at frame %llu (hang/runaway)\n",
              (unsigned long long)f);
      return 1;
    }

    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < sizeof(g_ram); i++) { h ^= g_ram[i]; h *= 1099511628211ULL; }
    if (f > 1 && h != last_ram_hash) { logic_changes++; stall_run = 0; }
    else if (f > 1) { stall_run++; if (stall_run > stall_max) stall_max = stall_run; }
    last_ram_hash = h;
    static int s_dbg_interval = -1;
    if (s_dbg_interval < 0) {
      const char *e = getenv("SC_DEBUG");
      s_dbg_interval = e ? (atoi(e) > 0 ? atoi(e) : 60) : 0;
    }
    if (s_dbg_interval > 0 && (f % (uint64_t)s_dbg_interval) == 0) {
      fprintf(stderr, "[dbg f=%llu] cpu.pc=%02x:%04x ram_stall_run=%llu input=%04x "
              "$011b=%02x $12=%02x $01f5=%02x%02x $01c1=%02x%02x $d7=%02x "
              "$01df=%02x%02x $0c0f=%02x%02x $c9=%02x $ca=%02x\n",
              (unsigned long long)f, g_cpu->k, g_cpu->pc,
              (unsigned long long)stall_run, g_snes->input1_currentState,
              g_ram[0x011b], g_ram[0x0012],
              g_ram[0x01f6], g_ram[0x01f5], g_ram[0x01c2], g_ram[0x01c1],
              g_ram[0x00d7],
              g_ram[0x01e0], g_ram[0x01df], g_ram[0x0c10], g_ram[0x0c0f],
              g_ram[0x00c9], g_ram[0x00ca]);
    }

    uint64_t vh = 1469598103934665603ULL;
    for (size_t i = 0; i < sizeof(s_video_pixels); i++) { vh ^= s_video_pixels[i]; vh *= 1099511628211ULL; }
    if (f > 1 && vh != last_video_hash) video_changes++;
    last_video_hash = vh;

    {
      const char *dump_at = getenv("SC_DUMP_AT");
      const char *dump_path = getenv("SC_DUMP_PATH");
      if (dump_at && dump_path && f == strtoull(dump_at, NULL, 0)) {
        if (write_ppm(dump_path))
          fprintf(stderr, "dumped frame %llu to %s\n", (unsigned long long)f, dump_path);
        else
          fprintf(stderr, "failed to write frame dump to %s\n", dump_path);
      }
      const char *wram_path = getenv("SC_WRAM_DUMP_PATH");
      if (dump_at && wram_path && f == strtoull(dump_at, NULL, 0)) {
        if (write_wram_dump(wram_path))
          fprintf(stderr, "dumped WRAM at frame %llu to %s\n", (unsigned long long)f, wram_path);
        else
          fprintf(stderr, "failed to write WRAM dump to %s\n", wram_path);
      }
    }

    Dsp *dsp = g_snes->apu->dsp;
    uint32_t available = dsp->sampleWrite - dsp->sampleRead;
    bool active = false;
    uint32_t inspect = available < DSP_SAMPLE_RING ? available : DSP_SAMPLE_RING;
    for (uint32_t i = 0; i < inspect; i++) {
      uint32_t idx = (dsp->sampleRead + i) & (DSP_SAMPLE_RING - 1);
      if (dsp->sampleBuffer[idx * 2] || dsp->sampleBuffer[idx * 2 + 1]) { active = true; break; }
    }
    if (active) audio_active_frames++;
    /* dsp_getSamples() (runner/src/snes/dsp.c) always consumes a fixed 534
     * native samples per call and resamples them to the `samplesPerFrame`
     * argument -- it does NOT consume `samplesPerFrame` samples. Gating the
     * call on "available >= (a ~533 target output count)" therefore lets it
     * fire when fewer than 534 raw samples truly exist, pushing sampleRead
     * past sampleWrite; the unsigned wraparound then reads as "ring
     * completely full" to the DSP's own backpressure check in dsp_cycle and
     * freezes sample production forever. Gate on the real fixed quantum. */
    if (available >= 534) dsp_getSamples(dsp, audio_buf, 534);
    last_sample_write = dsp->sampleWrite;
  }

  int rc = 0;
  if (frames >= 120) {
    if (!logic_changes) {
      fprintf(stderr, "qualify: FAIL -- logic did not progress across %llu frames\n",
              (unsigned long long)frames);
      rc = 1;
    }
    if (last_sample_write < frames * 500 || !audio_active_frames) {
      fprintf(stderr, "qualify: FAIL -- audio did not produce active continuous output "
              "(samples=%u active_frames=%llu)\n",
              last_sample_write, (unsigned long long)audio_active_frames);
      rc = 1;
    }
    if (!video_changes) {
      fprintf(stderr, "qualify: FAIL -- rendered video stayed frozen\n");
      rc = 1;
    }
  }
  fprintf(stderr,
          "qualify: %s frames=%llu master=%llu logic_changes=%llu "
          "logic_stall_max=%llu audio_samples=%u audio_active_frames=%llu "
          "video_changes=%llu nmi_requests=%llu nmi_serviced=%llu "
          "final_pc=%02x:%04x\n",
          rc == 0 ? "PASS" : "FAIL",
          (unsigned long long)frames, (unsigned long long)g_master_cycles,
          (unsigned long long)logic_changes, (unsigned long long)stall_max,
          last_sample_write, (unsigned long long)audio_active_frames,
          (unsigned long long)video_changes,
          (unsigned long long)s_nmi_requests, (unsigned long long)s_nmi_serviced,
          g_cpu->k, g_cpu->pc);
  fprintf(stderr, "banks_seen=%016llx\n", (unsigned long long)s_banks_seen);
  write_pc_bitmap_dump();
  return rc;
}

int main(int argc, char **argv) {
  const char *rom_path = "simcity.sfc";
  uint64_t qualify_frames = 0;
  int scale = 3;
  { const char *e = getenv("SC_IO_TRACE"); if (e && *e) s_io_trace_until = strtoull(e, NULL, 0); }
  { const char *e = getenv("SC_PC_TRACE");
    if (e && *e) { s_pc_trace_at_frame = strtoull(e, NULL, 0); s_pc_capture_after = -2; } }
  { const char *e = getenv("SC_ADDR_TRACE"); if (e && *e) parse_addr_trace(e); }
  { const char *e = getenv("SC_PC_BITMAP_BANK");
    if (e && *e) s_pc_bitmap_bank = !strcmp(e, "all") ? -2 : (int)strtol(e, NULL, 16); }
  { const char *e = getenv("SC_PC_BITMAP_START"); if (e && *e) s_pc_bitmap_start_frame = strtoull(e, NULL, 0); }
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--qualify") && i + 1 < argc) {
      qualify_frames = strtoull(argv[++i], NULL, 0);
    } else if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
      scale = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--input") && i + 1 < argc) {
      if (!add_input_event(argv[++i])) {
        fprintf(stderr, "invalid --input event; expected start:duration:hexmask\n");
        return 2;
      }
    } else if (argv[i][0] != '-') {
      rom_path = argv[i];
    }
  }

  uint32_t rom_size = 0;
  uint8_t *rom_data = read_file(rom_path, &rom_size);
  if (!rom_data) {
    fprintf(stderr, "cannot read ROM '%s'\n", rom_path);
    return 1;
  }

  /* D-pad fix, applied unconditionally at load time (game-specific ROM data
   * patch; never touches the shared snesrecomp runtime). The stock ROM's
   * D-pad handling is genuinely broken across every list/grid/cursor screen
   * that uses it -- confirmed dead on this recomp, independently
   * re-confirmed on real hardware-accurate emulation (bsnes), and confirmed
   * fixed by live interactive testing across all 9 affected screens: map
   * scrolling, the build cursor, the toolbar, in-game menus, the
   * Information panel, the mode-select (start) menu, Scenario Select,
   * Save, and Tax.
   *
   * Root cause, variant 1: 8 places in the stock ROM (found by scanning for
   * the byte pattern `AD xx xx 29 00 0F`) do `LDA $011b; AND #$0f00; ...`,
   * each followed by an ASL/BCC ladder calibrated for testing bits 8-11
   * (highest first). But those bits are the low nibble of $011c, which
   * mirrors hardware register $4219's low nibble: unconnected
   * controller-port pins, guaranteed 0 on any real SNES. That makes every
   * one of these branches structurally dead for real controller input; the
   * D-pad's Up/Down/Left/Right actually live in bits 0-3 of $011b (traced
   * and confirmed: 00:928f-92cb, the shared per-port edge-detect routine,
   * stores $4218/4219 into $011b/$011c verbatim -- no bit-duplication
   * anywhere).
   *
   * Fix: rather than changing the AND mask (desyncs it from the ladder's
   * shift calibration -- tried on the map site, confirmed by bitmap-diff
   * to just fall through every tap to the "no match" case), shift which
   * byte the ladder sees instead: changing the LDA's address from $011b to
   * $011a makes $011a the low byte (irrelevant -- AND #$0f00 discards it
   * anyway) and $011b the *high* byte of the load, landing bit3 (Up) at
   * loaded-bit11, bit2 (Down) at bit10, bit1 (Left) at bit9, bit0 (Right)
   * at bit8 -- exactly where each site's existing, unmodified ladder
   * already expects them, in the same highest-first test order. One
   * address byte per site, same instruction length, nothing downstream
   * shifts. File offset == ROM address numerically for every site here
   * (this cart has no copier header; bank 0 offset = addr-0x8000, bank 1
   * offset = addr, and by coincidence every listed site's low-operand-byte
   * address already equals its own file offset once bank is accounted for
   * -- verified individually against raw ROM bytes before patching). */
  {
    static const uint32_t kSites[] = {
      0x3b0a, /* 00:bb09 */
      0x3c66, /* 00:bc65 */
      0x4221, /* 00:c220 */
      0xa7b9, /* 01:a7b8 */
      0xa7fe, /* 01:a7fd */
      0xae2f, /* 01:ae2e */
      0xc133, /* 01:c132 */
      0xc2f4, /* 01:c2f3 */
    };
    int patched = 0;
    for (size_t i = 0; i < sizeof(kSites) / sizeof(kSites[0]); i++) {
      uint32_t off = kSites[i];
      if (off < rom_size && rom_data[off] == 0x1b) {
        rom_data[off] = 0x1a;
        patched++;
      }
    }
    fprintf(stderr, "dpad fix: patched %d/%d LDA $011b -> LDA $011a sites\n",
            patched, (int)(sizeof(kSites) / sizeof(kSites[0])));

    /* Same bug, direct-page variant: `LDA $c9 (dp); AND #$0f00; ...` at
     * 01:cc24 and 01:e8f4 -- $c9 is the edge-detected ("just pressed")
     * direct-page mirror (see 00:92a5 in the shared edge-detector), not the
     * held-state $011b, which is why this pair drives *edge-triggered*,
     * single-step-per-press list navigation with wraparound (the mode-
     * select menu, and other list-style menus) rather than the map's
     * continuous held-state movement. Same fix, same reasoning: shift the
     * direct-page operand from $c9 to $c8 so the real edge byte lands in
     * the high byte of the 16-bit load, where `AND #$0f00` and the
     * following XBA/LSR ladder (01:cc3b onward) already expect it. */
    static const uint32_t kDpSites[] = {
      0xcc25, /* 01:cc24 */
      0xe8f5, /* 01:e8f4 */
    };
    int dpPatched = 0;
    for (size_t i = 0; i < sizeof(kDpSites) / sizeof(kDpSites[0]); i++) {
      uint32_t off = kDpSites[i];
      if (off < rom_size && rom_data[off] == 0xc9) {
        rom_data[off] = 0xc8;
        dpPatched++;
      }
    }
    fprintf(stderr, "dpad fix: patched %d/%d LDA $c9(dp) -> LDA $c8(dp) sites\n",
            dpPatched, (int)(sizeof(kDpSites) / sizeof(kDpSites[0])));

    /* Same bug family, third variant: the Tax-screen modal (and likely
     * Save/other bank-2 modals) does `LDA $ca (dp, 8-bit); AND #$0f/#$0c/
     * #$08; ...` at 02:a50c and 02:ab1f. $ca is P1's edge-detect *high*
     * byte (paralleling $011c) -- same hardware-zero-low-nibble issue as
     * every other site in this family, confirmed empirically: this whole
     * screen's cooperative-scheduler tasks are paused while the modal is
     * open (banks_seen only shows 0,2 -- verified via live bitmap-diff
     * sessions), and 01:ae26 (the map/menu cursor mover this session
     * already fixed) is never even called here, so this bank-2 pair is the
     * modal's own, separate direction check. Unlike the earlier two
     * variants, this is a single 8-bit direct-page load (not a 16-bit
     * load spanning two bytes), so the fix is simpler: just point the load
     * at the real edge byte, $c9, directly -- no byte-shift trick needed. */
    static const uint32_t kCaSites[] = {
      0x1250d, /* 02:a50c (file offset = bank*0x8000 + (addr-0x8000) = 0x1250d) */
      0x12b20, /* 02:ab1f (file offset = 0x12b20) */
    };
    int caPatched = 0;
    for (size_t i = 0; i < sizeof(kCaSites) / sizeof(kCaSites[0]); i++) {
      uint32_t off = kCaSites[i];
      if (off < rom_size && rom_data[off] == 0xca) {
        rom_data[off] = 0xc9;
        caPatched++;
      }
    }
    fprintf(stderr, "dpad fix: patched %d/%d LDA $ca(dp) -> LDA $c9(dp) sites\n",
            caPatched, (int)(sizeof(kCaSites) / sizeof(kCaSites[0])));

    /* Found via a live bsnes hardware trace (user-captured), not static
     * scanning: the mode-select/list-menu navigation dispatch is
     * NMI -> dp $14-indexed table at 03:d255 -> handler 03:d333, which does
     * `LDA $ca; AND #$0c; BEQ $d360` (bits 2-3 = Down/Up in the edge-mirror
     * convention -- same hardware-zero-low-nibble bug as every other site
     * in this family) immediately followed by `LDX $3e; CMP #$04; ...;
     * INX; CPX #$04` -- a 4-slot index with wraparound, matching exactly
     * the "holding doesn't repeat, pressing again wraps at the ends"
     * behavior described for the real cartridge. This is confirmed
     * *reached* during mode-select via the live trace (unlike 02:a50c/
     * 02:ab1f, which looked identical but turned out to be dead code).
     * The very next check at 03:d360, `LDA $ca; AND #$90` (bits 4,7 =
     * R/A), reads real, valid bits -- R and A are not in the hardware-zero
     * region -- so that one is left alone. */
    {
      uint32_t off = 0x1d33f; /* 03:d33e's operand, file offset = 3*0x8000+(0xd33f-0x8000) */
      if (off < rom_size && rom_data[off] == 0xca) {
        rom_data[off] = 0xc9;
        fprintf(stderr, "dpad fix: patched 03:d33e LDA $ca(dp) -> LDA $c9(dp)\n");
      } else {
        fprintf(stderr, "dpad fix: 03:d33e site NOT patched (byte mismatch)\n");
      }
    }

    /* The 03:d255 table (found via the bsnes trace above) has 20 entries,
     * one per screen/menu mode selected by dp $14. Mode 3 (03:d333, mode-
     * select) is fixed above; several other modes' handlers have the same
     * `LDA $ca` bug reading real, hand-verified-live bits:
     *   - mode 5 (03:d3ca): 03:d62a does `LDA $ca; LSR A; BCC ...` testing
     *     bit0 (Right) -- fixed. (03:d3e2 in the same handler ALSO reads
     *     $ca, but that load's result feeds *both* a BMI testing bit7 (A,
     *     a real/valid bit) *and* a chained AND #$0f -- shifting its source
     *     would break the valid A-button check, so it's left alone;
     *     mode 5 may only be partially fixed as a result.)
     *   - mode 11 (03:ddb6): 03:ddc3 does `LDA $ca; AND #$0f; ...; AND
     *     #$0c; ...` (a 2-stage any-direction/vertical ladder, both stages
     *     sharing this one load) and 03:ddd9 does a separate `LDA $ca;
     *     AND #$03` (horizontal) -- both fixed.
     * (03:dbb8 in mode 9 also reads $ca but only via `BPL` testing bit7/A
     * directly -- a real, valid check, not part of this bug family, left
     * alone.) */
    static const uint32_t kCa2Sites[] = {
      0x1d62b, /* 03:d62a (mode 5) */
      0x1ddc4, /* 03:ddc3 (mode 11) */
      0x1ddda, /* 03:ddd9 (mode 11) */
    };
    int ca2Patched = 0;
    for (size_t i = 0; i < sizeof(kCa2Sites) / sizeof(kCa2Sites[0]); i++) {
      uint32_t off = kCa2Sites[i];
      if (off < rom_size && rom_data[off] == 0xca) {
        rom_data[off] = 0xc9;
        ca2Patched++;
      }
    }
    fprintf(stderr, "dpad fix: patched %d/%d more LDA $ca(dp) -> LDA $c9(dp) sites (modes 5,11)\n",
            ca2Patched, (int)(sizeof(kCa2Sites) / sizeof(kCa2Sites[0])));

    /* Tax screen's remaining blocker, found via live collaborative bsnes +
     * our-own-recomp tracing: 02:a4ec does `LDA $011b; AND #$fff0; BNE
     * $a4f7; JMP $a594` -- a gate that (deliberately, this is NOT part of
     * the $ca/hardware-zero-nibble bug family; #$fff0 is a real, correct
     * mask, matching the "any button other than direction" idiom seen
     * elsewhere in this ROM) only lets Tax's per-frame cursor-update code
     * (leading into the already-fixed 02:a50c/02:ab1f) run when some
     * non-direction button is *also* held. Confirmed live: with only the
     * $ca fixes applied, Down/Up alone did nothing in Tax, but Down+A did
     * (moved the cursor) -- "not as intended" per interactive testing,
     * exactly matching this gate's behavior. Rather than touching the
     * $ca sites (already correct) or removing the gate outright (it may
     * exist to skip redundant redraw work), widen its mask from #$fff0 to
     * #$ffff so direction bits alone also satisfy it -- one byte, and the
     * only observable effect is this gate opening for direction-only input
     * too, same as it already does for every other button. */
    {
      uint32_t off = 0x124f0; /* 02:a4ef's low operand byte */
      if (off < rom_size && rom_data[off] == 0xf0) {
        rom_data[off] = 0xff;
        fprintf(stderr, "dpad fix: patched 02:a4ef AND #$fff0 -> AND #$ffff\n");
      } else {
        fprintf(stderr, "dpad fix: 02:a4ef site NOT patched (byte mismatch)\n");
      }
    }
  }

  g_snes = snes_init(g_ram);
  cart_set_master_clock_source(g_snes->cart, &g_master_cycles);
  g_ppu = g_snes->ppu;
  if (!snes_loadRom(g_snes, rom_data, (int)rom_size)) {
    fprintf(stderr, "loadRom failed for '%s'\n", rom_path);
    return 1;
  }
  snes_reset(g_snes, true);
  PpuBeginDrawing(g_ppu, s_video_pixels, kVideoPitch, 0);

  g_cpu = interp816_init(NULL, bus_read, bus_write);
  interp816_reset(g_cpu);

  if (qualify_frames) {
    return run_qualification(qualify_frames);
  }

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window *window = SDL_CreateWindow(
      "SimCitySNESRecomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      kVideoWidth * scale, kVideoHeight * scale, SDL_WINDOW_SHOWN);
  if (!window) { fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError()); return 1; }
  SDL_Renderer *renderer = SDL_CreateRenderer(
      window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) { fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError()); return 1; }
  SDL_Texture *texture = SDL_CreateTexture(
      renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      kVideoWidth, kVideoHeight);

  SDL_AudioSpec want, have;
  SDL_memset(&want, 0, sizeof(want));
  want.freq = 32040;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 1024;
  SDL_AudioDeviceID audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (audio_dev) SDL_PauseAudioDevice(audio_dev, 0);

  double audio_acc = 0.0;
  int16_t audio_buf[1024 * 2];
  bool quit = false;
  while (!quit) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT) quit = true;
      if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) quit = true;
      if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_F1 &&
          s_pc_bitmap_bank != -1) {
        if (s_pc_bitmap_bank == -2) memset(s_pc_bitmap_all, 0, sizeof(s_pc_bitmap_all));
        else memset(s_pc_bitmap, 0, sizeof(s_pc_bitmap));
        fprintf(stderr, "[F1] PC bitmap capture reset at frame %llu\n",
                (unsigned long long)s_frames);
      }
    }
    const uint8_t *keys = SDL_GetKeyboardState(NULL);
    uint16_t input = 0;
    /* Diamond cluster U/H/J/K as an alternate D-pad, alongside arrow keys,
     * for testing (U=up, H=left, J=down, K=right). */
    if (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_U]) input |= kPad_Up;
    if (keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_J]) input |= kPad_Down;
    if (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_H]) input |= kPad_Left;
    if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_K]) input |= kPad_Right;
    /* SDL scancodes are physical/positional (QWERTY-based); on a QWERTZ
     * (e.g. German) keyboard the Y/Z key positions are swapped, so accept
     * either scancode here regardless of active keyboard layout. */
    if (keys[SDL_SCANCODE_Z] || keys[SDL_SCANCODE_Y]) input |= kPad_B;
    if (keys[SDL_SCANCODE_X]) input |= kPad_A;
    if (keys[SDL_SCANCODE_A]) input |= kPad_Y;
    if (keys[SDL_SCANCODE_S]) input |= kPad_X;
    if (keys[SDL_SCANCODE_Q]) input |= kPad_L;
    if (keys[SDL_SCANCODE_E]) input |= kPad_R;
    if (keys[SDL_SCANCODE_RETURN]) input |= kPad_Start;
    if (keys[SDL_SCANCODE_RSHIFT] || keys[SDL_SCANCODE_LSHIFT]) input |= kPad_Select;
    apply_frame_input(s_frames);
    g_snes->input1_currentState |= input;

    if (!run_one_frame()) {
      fprintf(stderr, "frame %llu: opcode guard tripped (hang/runaway) -- stopping\n",
              (unsigned long long)s_frames);
      break;
    }

    if (getenv("SC_LIVE_C9CA")) {
      static uint8_t s_last_c9 = 0xff, s_last_ca = 0xff, s_last_011b = 0xff;
      if (g_ram[0x00c9] != s_last_c9 || g_ram[0x00ca] != s_last_ca || g_ram[0x011b] != s_last_011b) {
        fprintf(stderr, "[c9ca f=%llu] $011b=%02x $c9=%02x $ca=%02x $01fb=%02x $14=%02x\n",
                (unsigned long long)s_frames, g_ram[0x011b], g_ram[0x00c9], g_ram[0x00ca],
                g_ram[0x01fb], g_ram[0x0014]);
        s_last_c9 = g_ram[0x00c9]; s_last_ca = g_ram[0x00ca]; s_last_011b = g_ram[0x011b];
      }
    }

    /* Audio: drain one frame's worth of DSP output at the native SNES rate,
     * same pacing model as snesrecomp/cosim/ref_driver.c's deterministic
     * consumer, queued to the SDL audio device instead of discarded. */
    if (audio_dev) {
      audio_acc += (double)have.freq / 60.0988;
      int wantN = (int)audio_acc;
      Dsp *dsp = g_snes->apu->dsp;
      uint32_t available = dsp->sampleWrite - dsp->sampleRead;
      /* dsp_getSamples() always consumes a fixed 534 native samples per
       * call and resamples them to `wantN` -- gate on that real fixed
       * quantum, not on `wantN`, or the DSP's own ring-full backpressure
       * permanently freezes production (see run_qualification()). */
      if (available >= 534 && wantN > 0 && wantN <= 1024) {
        audio_acc -= (double)wantN;
        dsp_getSamples(dsp, audio_buf, wantN);
        SDL_QueueAudio(audio_dev, audio_buf, (Uint32)(wantN * 2 * sizeof(int16_t)));
      }
    }

    void *pixels; int pitch;
    SDL_LockTexture(texture, NULL, &pixels, &pitch);
    memcpy(pixels, s_video_pixels, sizeof(s_video_pixels));
    SDL_UnlockTexture(texture);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
  }

  if (audio_dev) SDL_CloseAudioDevice(audio_dev);
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  write_pc_bitmap_dump();
  return 0;
}
