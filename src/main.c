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
 *                           delays tracing until that frame.
 * SC_GFX_TRACE=1            log (rate-limited, 300 hits) every write to
 *                           BGMODE ($2105), the Mode 7 matrix/center regs
 *                           ($211b-$2114), HDMAEN ($420c), and every HDMA
 *                           channel's control/dest/addr regs ($43x0-$43xa)
 *                           -- for finding whether/how a screen sets up
 *                           Mode 7 + HDMA (e.g. the tilted "View" map). */
static bool s_gfx_trace;
static uint32_t s_gfx_trace_hits;
static uint32_t s_dbg_live_hits;
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
  /* Gated behind its own SC_CADENCE_WATCH flag, not just s_addr_trace_count
   * -- this used to piggyback on any SC_ADDR_TRACE use at all, which meant
   * tracing something unrelated (e.g. a task-scheduler lead far from the
   * cadence investigation this was built for) silently also turned on
   * hundreds of lines/frame of unrelated $011b/$011c read spam, tanking
   * framerate badly enough to make the window unplayable long before
   * reaching whatever screen was actually being tested. */
  if (s_addr_trace_count && s_frames >= s_addr_trace_start_frame &&
      getenv("SC_CADENCE_WATCH") &&
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
      getenv("SC_CADENCE_WATCH") &&
      (reg == 0x01eb || reg == 0x01ec || reg == 0x01ed || reg == 0x01ee || reg == 0x007c)) {
    static uint32_t s_wram_watch_hits;
    if (s_wram_watch_hits < 200) {
      fprintf(stderr, "[wramwrite f=%llu] pc=%02x:%04x $%04x = %02x\n",
              (unsigned long long)s_frames, g_cpu->k, g_cpu->pc, reg, v);
      s_wram_watch_hits++;
    }
  }
  if (s_gfx_trace && hw &&
      (reg == 0x2105 || (reg >= 0x211b && reg <= 0x2114) || reg == 0x420c ||
       (reg >= 0x4300 && reg <= 0x437f && (reg & 0x0f) <= 0x0a))) {
    if (s_gfx_trace_hits < 300) {
      fprintf(stderr, "[gfxtrace f=%llu] pc=%02x:%04x WRITE $%04x = %02x\n",
              (unsigned long long)s_frames, g_cpu->k, g_cpu->pc, reg, v);
      s_gfx_trace_hits++;
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

/* HDMA per-scanline execution. The shared engine's cycle-accurate DMA path
 * (snes/dma.c) tracks $420C-enabled channels via `hdmaActive` but never
 * actually walks their tables -- only plain DMA ($420B, dma_doDma) is wired
 * up. A per-line table-walk implementation exists in the shared runtime
 * (common_rtl.c's SimpleHdma_Init/DoLine), but it's written for the
 * AOT/decompiled recomp path (raw host pointers into its own g_ram, which
 * would collide with this file's g_ram if common_rtl.c were linked in) and
 * this interpreter-only project never calls it anyway, so every HDMA-driven
 * effect in this ROM (found so far: the View screen's per-scanline window
 * (`$2126-$2129`) + BG1/BG2 horizontal-scroll (`$210d`/`$210f`) tilt effect)
 * silently never applies -- confirmed via SC_GFX_TRACE, which showed the
 * channels correctly configured and enabled every frame but their target
 * PPU registers never actually written. Ported here instead, using the same
 * snes_read/snes_writeBBus bus primitives dma.c's own plain-DMA path already
 * uses (see dma_transferByte), so it works uniformly for WRAM- or
 * ROM-sourced tables without needing raw host pointers. This is a
 * game-specific addition, not a shared-runtime change, but it fixes HDMA
 * generally for this ROM, not just the one effect that surfaced the gap. */
typedef struct {
  bool active;
  uint8_t bank;       /* bank of the table pointer (and, in direct mode, the data) */
  uint16_t addr;       /* current table read pointer */
  uint8_t repCount;
  uint8_t mode;         /* dc->mode (bits 0-2) | 0x40 if indirect */
  uint8_t ppuAddr;      /* B-bus dest offset, $00-$3f */
  uint8_t indirBank;
  uint16_t indirAddr;    /* current indirect data pointer (mode & 0x40 only) */
} HdmaChanState;
static HdmaChanState s_hdma[8];

static void hdma_init_channel(HdmaChanState *c, const DmaChannel *dc) {
  if (!dc->hdmaActive) { c->active = false; return; }
  c->active = true;
  c->bank = dc->aBank;
  c->addr = dc->aAdr;
  c->repCount = 0;
  c->mode = (uint8_t)(dc->mode | (dc->indirect ? 0x40 : 0));
  c->ppuAddr = dc->bAdr;
  c->indirBank = dc->indBank;
}

static void hdma_do_line(HdmaChanState *c) {
  static const uint8_t kBAdrOffsets[8][4] = {
    {0, 0, 0, 0}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1},
    {0, 1, 2, 3}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1},
  };
  static const uint8_t kTransferLength[8] = { 1, 2, 2, 4, 4, 4, 2, 4 };

  if (!c->active) return;
  bool do_transfer = false;
  if ((c->repCount & 0x7f) == 0) {
    c->repCount = snes_read(g_snes, ((uint32_t)c->bank << 16) | c->addr);
    c->addr++;
    if (c->repCount == 0) { c->active = false; return; }
    if (c->mode & 0x40) {
      uint8_t lo = snes_read(g_snes, ((uint32_t)c->bank << 16) | c->addr); c->addr++;
      uint8_t hi = snes_read(g_snes, ((uint32_t)c->bank << 16) | c->addr); c->addr++;
      c->indirAddr = (uint16_t)(lo | (hi << 8));
    }
    do_transfer = true;
  }
  if (do_transfer || (c->repCount & 0x80)) {
    int len = kTransferLength[c->mode & 7];
    for (int j = 0; j < len; j++) {
      uint8_t val;
      if (c->mode & 0x40) {
        val = snes_read(g_snes, ((uint32_t)c->indirBank << 16) | c->indirAddr);
        c->indirAddr++;
      } else {
        val = snes_read(g_snes, ((uint32_t)c->bank << 16) | c->addr);
        c->addr++;
      }
      uint8_t reg = (uint8_t)(c->ppuAddr + kBAdrOffsets[c->mode & 7][j]);
      snes_writeBBus(g_snes, reg, val);
    }
  }
  c->repCount--;
}

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
      /* Real "HDMA init": (re)latch each currently-enabled channel's table
       * pointer once per frame. This replaces an old `dma_startDma(dma, 0,
       * true)` call here that unconditionally zeroed every channel's
       * hdmaActive flag every frame -- harmless while nothing consumed that
       * flag, but exactly backwards for SimpleHdma_Init, which needs to see
       * whatever the game's own $420C write last set it to. */
      for (int i = 0; i < 8; i++)
        hdma_init_channel(&s_hdma[i], &snes->dma->channel[i]);
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
    if (!snes->inVblank) {
      dma_cycle(snes->dma);
      /* Per-line HDMA transfer, same timing as plain-DMA continuation
       * above: this fires during the current line's hblank, so the values
       * it writes take effect starting with the *next* line's render, at
       * this loop's hPos==0 branch. */
      for (int i = 0; i < 8; i++) hdma_do_line(&s_hdma[i]);
    }
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

/* Self-arming gate for SC_ADDR_TRACE, mirroring SC_DEBUG_LIVE's: don't
 * start logging until $01ed (the cursor/scroll byte under investigation)
 * first changes, so imprecise timing getting into gameplay doesn't burn
 * through the 200-hit-per-address cap on dead frames. Only takes effect
 * when SC_ADDR_TRACE_ARM_ON_01ED is set; otherwise behaves as before. */
static bool s_addr_trace_armed = true;
static uint8_t s_addr_trace_last_ed = 0xff;

/* Auto-fast-forward during map/scenario generation. Found live (bsnes
 * trace, user-captured): 03:d862 is a 10-iteration loop (X counts 9..0)
 * calling the checksum/hash accumulator at 00:824b/00:824f, which folds
 * scenario parameters ($0b27-$0b29) into a rolling pair of WRAM
 * accumulators ($59/$5b/$5d) -- almost certainly part of procedural
 * map/seed generation. This is genuine, real computation, not a dumb
 * idle-delay loop (confirmed: our interpreter already finishes every
 * frame's work in far under the 16.67ms budget -- SC_FRAME_TIME never
 * fires -- so the "wait" is the ROM deliberately spreading this work
 * across many real seconds of paced frames, not CPU cost). Rather than
 * replacing the algorithm (risk: any mismatch could produce a different
 * generated map than the real ROM would), just detect execution passing
 * through it and apply the same frame-batching fast-forward uses,
 * automatically, without needing Tab held. Also covers the Nintendo
 * LC_LZ5-style decompressor at 00:90dd (confirmed live: fires repeatedly
 * during the same map/scenario-load wait, decompressing tile/text data --
 * see tools/extract_graphics.py and docs/REFERENCE_third_party_optimization_patch.md). */
static int s_gen_loop_active_frames; /* counts down; >0 means "recently seen" */
#define SC_GEN_LOOP_HOLDOFF 20 /* frames to keep boosting after the last hit */

static bool run_one_frame(void) {
  Snes *snes = g_snes;
  Interp816 *cpu = g_cpu;
  uint64_t target = s_frames + 1;
  long guard = 20000000; /* runaway guard: caps opcodes/frame, mirrors ref_driver.c */
  while (s_frames < target && guard-- > 0) {
    if (cpu->k == 0x00 && cpu->pc == 0x80b2) s_nmi_serviced++;
    if ((cpu->k == 0x03 && cpu->pc == 0xd862) || (cpu->k == 0x00 && cpu->pc == 0x824b) ||
        (cpu->k == 0x00 && cpu->pc == 0x90dd))
      s_gen_loop_active_frames = SC_GEN_LOOP_HOLDOFF;
    if (s_addr_trace_count && s_frames >= s_addr_trace_start_frame && s_addr_trace_armed) {
      uint32_t pc = ((uint32_t)cpu->k << 16) | cpu->pc;
      for (int i = 0; i < s_addr_trace_count; i++) {
        if (s_addr_trace_pcs[i] == pc && s_addr_trace_hits[i] < 200) {
          fprintf(stderr, "[addrtrace f=%llu] pc=%02x:%04x a=%04x x=%04x y=%04x "
                  "s=%04x d=%04x db=%02x m%s x%s hit#%u\n",
                  (unsigned long long)s_frames, cpu->k, cpu->pc, cpu->a, cpu->x,
                  cpu->y, cpu->sp, cpu->dp, cpu->db, cpu->mf ? "8" : "16",
                  cpu->xf ? "8" : "16", s_addr_trace_hits[i]);
          if (s_addr_trace_hits[i] < 40) {
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
    /* Gated on s_addr_trace_start_frame too, not just s_addr_trace_count --
     * this ran on every single opcode (not just every frame) from frame 0
     * the moment SC_ADDR_TRACE was set at all, regardless of an @start
     * suffix, which made interactive play visibly slower for however long
     * it took to reach the frame actually being investigated. Now a
     * delayed start via SC_ADDR_TRACE=...@N keeps the game at full,
     * untraced speed until frame N. */
    if (s_addr_trace_count && s_frames >= s_addr_trace_start_frame) {
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
  if (getenv("SC_ADDR_TRACE_ARM_ON_01ED")) {
    if (s_addr_trace_last_ed == 0xff) { s_addr_trace_last_ed = g_ram[0x01ed]; s_addr_trace_armed = false; }
    else if (!s_addr_trace_armed && g_ram[0x01ed] != s_addr_trace_last_ed) s_addr_trace_armed = true;
  }
  if (s_gen_loop_active_frames > 0) s_gen_loop_active_frames--;
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
 * guessing from an interactive session. Player 2 gets its own identical
 * array/parser (--input2) -- needed for the documented debug-menu code
 * entry, which is read on controller 2. ──────────────────────────────── */
typedef struct InputEvent { uint64_t start, duration; uint16_t mask; } InputEvent;
static InputEvent s_input_events[64];
static uint32_t s_input_event_count;
static InputEvent s_input2_events[96];
static uint32_t s_input2_event_count;

static bool add_input_event_to(InputEvent *arr, uint32_t *count, uint32_t cap, const char *text) {
  unsigned long long start = 0, duration = 0;
  unsigned mask = 0;
  char trailing = '\0';
  if (*count >= cap ||
      sscanf(text, "%llu:%llu:%x%c", &start, &duration, &mask, &trailing) != 3 ||
      !duration || mask > 0xffffu)
    return false;
  arr[(*count)++] = (InputEvent){start, duration, (uint16_t)mask};
  return true;
}

static bool add_input_event(const char *text) {
  return add_input_event_to(s_input_events, &s_input_event_count, 64, text);
}

static bool add_input2_event(const char *text) {
  return add_input_event_to(s_input2_events, &s_input2_event_count, 96, text);
}

/* One-button macro for the documented debug-menu entry code (Peter's
 * SimCity SNES Guide, crediting Corey Miller/"ZaphodBee"): a fixed
 * 16-step sequence read on controller 2 while on the "Goodbye! See you
 * soon" quit-confirmation screen. Static ROM analysis found no code
 * anywhere in this ROM dump reading a second controller (no $421A/$421B
 * or manual $4016/$4017 access), so this is unverified for this specific
 * ROM revision -- this macro exists to test it live/headlessly rather
 * than requiring 16 hand-timed presses. Each step is held for
 * kP2StepHold frames with a kP2StepGap release between steps so the
 * game's edge-detection (if any) sees 16 distinct presses, not one held
 * button. */
enum { kP2StepHold = 6, kP2StepGap = 6, kP2StepFrames = kP2StepHold + kP2StepGap };
static bool queue_debug_menu_code(uint64_t start_frame) {
  static const uint16_t kSeq[] = {
    kPad_Left, kPad_A, kPad_Right, kPad_Y, kPad_Up, kPad_B, kPad_Down, kPad_X,
    kPad_Select, kPad_Start, kPad_Start, kPad_Select, kPad_R, kPad_R, kPad_L, kPad_L,
  };
  uint32_t n = (uint32_t)(sizeof(kSeq) / sizeof(kSeq[0]));
  if (s_input2_event_count + n > 96) return false;
  for (uint32_t i = 0; i < n; i++) {
    s_input2_events[s_input2_event_count++] = (InputEvent){
      start_frame + (uint64_t)i * kP2StepFrames, kP2StepHold, kSeq[i]
    };
  }
  fprintf(stderr, "queued debug-menu code on controller 2 starting frame %llu (%u steps, %d frames each)\n",
          (unsigned long long)start_frame, n, kP2StepFrames);
  return true;
}

static void apply_frame_input(uint64_t frame) {
  uint16_t input = 0;
  for (uint32_t i = 0; i < s_input_event_count; i++) {
    InputEvent *e = &s_input_events[i];
    if (frame >= e->start && frame - e->start < e->duration) input |= e->mask;
  }
  g_snes->input1_currentState = input;

  uint16_t input2 = 0;
  for (uint32_t i = 0; i < s_input2_event_count; i++) {
    InputEvent *e = &s_input2_events[i];
    if (frame >= e->start && frame - e->start < e->duration) input2 |= e->mask;
  }
  g_snes->input2_currentState = input2;
}

/* ── host-mouse cursor control, ported from the community "SimCity mouse
 * patch" (https://github.com/Selicre/simcity-mouse, main.asm/mouse.asm).
 * That patch hooks the NMI to bit-bang an actual SNES mouse's serial
 * protocol on controller port 2 and accumulates the result into two WRAM
 * bytes it identified by testing: $7E01EB (X) and $7E01ED (Y) -- the same
 * $01eb,X "cursor-offset ladder" this project's own D-pad investigation
 * found and fixed for the Comprehensive/Information overlay screen (see
 * docs/INVESTIGATION_dpad.md, variant 6). Rather than apply the original
 * ASM patch (which would mean shipping a modified ROM binary, contrary to
 * this project being ROM-free, and would require an NMI-vector splice
 * this recomp's C driver doesn't need), this ports just the destination
 * semantics: since we already have direct WRAM access every frame, skip
 * the serial-read entirely and drive the same two accumulator bytes from
 * the real host mouse instead. Upstream's own README calls this "lots of
 * jank" (menus visually desync until the D-pad is used, no button
 * support, occasional resets to origin) -- ported as-is, same caveats
 * apply here. Toggle with F3 (see SDL_SCANCODE_F3 above). */
static bool s_mouse_enabled;

static void apply_mouse_delta(int dx, int dy) {
  if (dx > 127) dx = 127; else if (dx < -127) dx = -127;
  if (dy > 127) dy = 127; else if (dy < -127) dy = -127;

  int x = (int)g_ram[0x01eb] + dx;
  if (x > 0xff) x = 0xff;
  if (x < 0x00) x = 0x00;
  g_ram[0x01eb] = (uint8_t)x;

  int y = (int)g_ram[0x01ed] + dy;
  if (y > 0xdf) y = 0xdf; /* 223: patch clamps Y to the visible scanline range */
  if (y < 0x00) y = 0x00;
  g_ram[0x01ed] = (uint8_t)y;
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
      /* SC_DUMP_DIR + SC_DUMP_INTERVAL [+ SC_DUMP_START]: same idea as the
       * SC_WRAM_DUMP_DIR family below, but for video (.ppm) frames -- lets
       * a single --qualify run capture a whole navigation sequence (e.g.
       * every frame of a menu transition) for offline visual inspection,
       * instead of needing one run per SC_DUMP_AT frame. */
      const char *dump_dir = getenv("SC_DUMP_DIR");
      const char *dump_interval_s = getenv("SC_DUMP_INTERVAL");
      if (dump_dir && dump_interval_s) {
        uint64_t interval = strtoull(dump_interval_s, NULL, 0);
        const char *start_s = getenv("SC_DUMP_START");
        uint64_t start = start_s ? strtoull(start_s, NULL, 0) : 0;
        if (interval > 0 && f >= start && (f - start) % interval == 0) {
          char path[512];
          snprintf(path, sizeof(path), "%s/frame_%010llu.ppm", dump_dir, (unsigned long long)f);
          if (!write_ppm(path))
            fprintf(stderr, "failed to write frame dump to %s\n", path);
        }
      }
      /* SC_WRAM_DUMP_DIR + SC_WRAM_DUMP_INTERVAL [+ SC_WRAM_DUMP_START]:
       * repeatedly dump WRAM every <interval> frames starting at <start>
       * (default 0), one file per dump named wram_<frame>.bin in <dir> --
       * for bulk automation (e.g. stepping through many Map Select
       * screens in a single --qualify run) where a single SC_DUMP_AT
       * frame isn't enough. */
      const char *wram_dir = getenv("SC_WRAM_DUMP_DIR");
      const char *wram_interval_s = getenv("SC_WRAM_DUMP_INTERVAL");
      if (wram_dir && wram_interval_s) {
        uint64_t interval = strtoull(wram_interval_s, NULL, 0);
        const char *start_s = getenv("SC_WRAM_DUMP_START");
        uint64_t start = start_s ? strtoull(start_s, NULL, 0) : 0;
        if (interval > 0 && f >= start && (f - start) % interval == 0) {
          char path[512];
          snprintf(path, sizeof(path), "%s/wram_%010llu.bin", wram_dir, (unsigned long long)f);
          if (!write_wram_dump(path))
            fprintf(stderr, "failed to write WRAM dump to %s\n", path);
        }
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
  { const char *e = getenv("SC_GFX_TRACE"); if (e && *e) s_gfx_trace = true; }
  { const char *e = getenv("SC_PC_BITMAP_BANK");
    if (e && *e) s_pc_bitmap_bank = !strcmp(e, "all") ? -2 : (int)strtol(e, NULL, 16); }
  { const char *e = getenv("SC_PC_BITMAP_START"); if (e && *e) s_pc_bitmap_start_frame = strtoull(e, NULL, 0); }
  /* SC_DEBUG_CODE_AT=<frame>: queue the one-button debug-menu code macro
   * (see queue_debug_menu_code) on controller 2 at that frame, for
   * headless verification in --qualify mode without hand-timing 16
   * presses. */
  { const char *e = getenv("SC_DEBUG_CODE_AT");
    if (e && *e) queue_debug_menu_code(strtoull(e, NULL, 0)); }
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
    } else if (!strcmp(argv[i], "--input2") && i + 1 < argc) {
      if (!add_input2_event(argv[++i])) {
        fprintf(stderr, "invalid --input2 event; expected start:duration:hexmask\n");
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

    /* Save/Load/Exit top-level menu (found via live bsnes tracing, with the
     * user pointing out the live "main loop" at $0008c0 and independently
     * spotting $0421 as the changing selection-index byte): 00:d1b8 does
     * `LDA $c9 (dp, 16-bit); AND #$0300; ...` -- bits 8-9 of that combined
     * 16-bit word are $ca's bits 0-1, the same hardware-zero region as
     * every other site in this family. This one gate feeds a symmetric
     * Left/Right pair (00:d1c4 decrements $0421, 00:d1db increments it,
     * both wrapping/clamping into a small range and used as a table index
     * at 00:d23a to reposition the selection-highlight sprite) -- fixing
     * this one site fixes both directions. The two neighboring checks at
     * 00:d1aa (`AND #$8000`, tests $ca bit7 = A) and 00:d1b1 (`AND #$0040`,
     * tests $c9's own bit6 = Y) already read real, valid bits and are left
     * alone. */
    {
      uint32_t off = 0x51b9; /* 00:d1b8's operand byte, file offset = 0xd1b9-0x8000 */
      if (off < rom_size && rom_data[off] == 0xc9) {
        rom_data[off] = 0xc8;
        fprintf(stderr, "dpad fix: patched 00:d1b8 LDA $c9(dp) -> LDA $c8(dp)\n");
      } else {
        fprintf(stderr, "dpad fix: 00:d1b8 site NOT patched (byte mismatch)\n");
      }
    }

    /* Map Select scenario-number picker (found via extensive live
     * collaborative bsnes tracing -- $0b2d looked like the right selection
     * variable and genuinely did change on direction presses, but the
     * "confirm" action never reflected it; traced further and found the
     * real gate). 03:d3e2 does `LDA $ca` (8-bit); `BMI $d3ed` (tests bit7 =
     * A, real/valid, left alone); `AND #$0f` (tests bits 0-3, $ca's
     * hardware-zero low nibble); `BEQ $d459` -- same bug family as
     * everywhere else. This gate sits ahead of whatever logic actually
     * drives which control has focus, upstream of the $0b2d
     * sprite-position update (which runs unconditionally afterward via
     * 03:d3d4 regardless of this gate's outcome, which is why $0b2d
     * appeared to "work" while focus never actually changed). Fix: repoint
     * the load at $c9 directly (single 8-bit dp load, no byte-shift trick
     * needed, same as the other 8-bit `LDA $ca` sites). */
    {
      uint32_t off = 0x1d3e3; /* 03:d3e2's operand byte, file offset = 3*0x8000+(0xd3e3-0x8000) */
      if (off < rom_size && rom_data[off] == 0xca) {
        rom_data[off] = 0xc9;
        fprintf(stderr, "dpad fix: patched 03:d3e2 LDA $ca(dp) -> LDA $c9(dp)\n");
      } else {
        fprintf(stderr, "dpad fix: 03:d3e2 site NOT patched (byte mismatch)\n");
      }
    }

    /* City-name-entry on-screen keyboard (found via a fresh F1-bitmap-diff
     * pass, since none of the known $c9/$ca(dp) sites were reachable on
     * this screen at all). Same bug, new shape: 03:dad9 does `LDA $0124`
     * (*absolute*, not direct-page) then `AND #$0f` -- $0124 is the
     * absolute high byte of the shared edge-detector's $0123 mirror (see
     * 00:928f-92cb: 16-bit `STA $0123,X` writes low byte to $0123, high
     * byte to $0124), the exact same hardware-zero-nibble region as $ca,
     * just accessed via absolute addressing instead of the direct-page
     * mirror. Fix: repoint at $0123 (one byte; absolute addressing is
     * always 3 bytes regardless of M width, so nothing downstream
     * shifts). */
    {
      uint32_t off = 0x1dada; /* 03:dad9's low operand byte, file offset = 3*0x8000+(0xdada-0x8000) */
      if (off < rom_size && rom_data[off] == 0x24) {
        rom_data[off] = 0x23;
        fprintf(stderr, "dpad fix: patched 03:dad9 LDA $0124 -> LDA $0123\n");
      } else {
        fprintf(stderr, "dpad fix: 03:dad9 site NOT patched (byte mismatch)\n");
      }
    }

    /* "Select game level" (Easy/Medium/Hard) screen, right after name
     * entry: 03:d97b does `LDA $c9 (dp, 16-bit); AND #$0300; ...` -- the
     * exact same shape as 00:d1b8 (Save/Load/Exit), bits 8-9 landing in
     * $ca's hardware-zero low nibble. Same fix: shift the dp source back
     * one byte so the real data lands in the tested high-byte position. */
    {
      uint32_t off = 0x1d97c; /* 03:d97b's operand byte, file offset = 3*0x8000+(0xd97c-0x8000) */
      if (off < rom_size && rom_data[off] == 0xc9) {
        rom_data[off] = 0xc8;
        fprintf(stderr, "dpad fix: patched 03:d97b LDA $c9(dp) -> LDA $c8(dp)\n");
      } else {
        fprintf(stderr, "dpad fix: 03:d97b site NOT patched (byte mismatch)\n");
      }
    }

    /* Comprehensive/Information map overlay (found via a fresh F1-bitmap-
     * diff pass on this specific screen). Two sites, same shape, both
     * absolute (not direct-page): `LDA $011c; AND #$0f; BEQ ...` at 02:8525
     * and 02:9f37. $011c is the absolute mirror of the hardware-zero-
     * low-nibble byte (paralleling $ca), so the AND #$0f always comes back
     * zero and both routines fall straight through their BEQ into an
     * immediate RTS -- confirmed by reading the branch targets directly
     * (8589 and 9f75 are both bare RTS), matching the reported "no
     * scrolling, no button selection" behavior exactly: the handler that's
     * supposed to act on the ladder never gets past its first check. Same
     * fix as every other absolute-addressing site: repoint at $011b, the
     * real edge/held-state byte (one byte each; absolute addressing is
     * always 3 bytes regardless of M width, so nothing downstream
     * shifts). */
    {
      uint32_t off = 0x10526; /* 02:8525's low operand byte, file offset = 2*0x8000+(0x8526-0x8000) */
      if (off < rom_size && rom_data[off] == 0x1c) {
        rom_data[off] = 0x1b;
        fprintf(stderr, "dpad fix: patched 02:8525 LDA $011c -> LDA $011b\n");
      } else {
        fprintf(stderr, "dpad fix: 02:8525 site NOT patched (byte mismatch)\n");
      }
    }
    {
      uint32_t off = 0x11f38; /* 02:9f37's low operand byte, file offset = 2*0x8000+(0x9f38-0x8000) */
      if (off < rom_size && rom_data[off] == 0x1c) {
        rom_data[off] = 0x1b;
        fprintf(stderr, "dpad fix: patched 02:9f37 LDA $011c -> LDA $011b\n");
      } else {
        fprintf(stderr, "dpad fix: 02:9f37 site NOT patched (byte mismatch)\n");
      }
    }

    /* Same routine, second read: once 02:9f37's gate above lets execution
     * through (PHB; LDA #$02; PHA; PLB switches DBR to bank 2), 02:9f43
     * re-reads the *same* dead byte -- `LDA $011c; LDX #$00` -- and feeds it
     * straight into the actual cursor-offset ladder at 02:9f48 (`LSR A;
     * PHA; BCC +e; LDA $01eb,X; ADC #$02/SBC #$02; CMP` clamp table;
     * `STA $01eb,X`; `PLA; LSR A; ...`, looping X+=2 up to 4 -- i.e. X/Y
     * offset increment or decrement per direction bit, clamped against the
     * tables at 02:9f76/02:9f79). With $011c always zero, every LSR/BCC in
     * this ladder always branches over its STA, so $01eb,X (the cursor
     * offset) never actually changes -- confirmed live: reachable in this
     * exact capture once 02:9f37 alone was fixed, matching the user's
     * report that scrolling now works but the cursor itself still doesn't
     * move. Same fix: repoint at $011b. */
    {
      uint32_t off = 0x11f44; /* 02:9f43's low operand byte, file offset = 2*0x8000+(0x9f44-0x8000) */
      if (off < rom_size && rom_data[off] == 0x1c) {
        rom_data[off] = 0x1b;
        fprintf(stderr, "dpad fix: patched 02:9f43 LDA $011c -> LDA $011b\n");
      } else {
        fprintf(stderr, "dpad fix: 02:9f43 site NOT patched (byte mismatch)\n");
      }
    }

    /* View screen (the watch icon), found via a fresh F1-bitmap-diff pass
     * once its graphics were fixed (see the HDMA work above -- the D-pad
     * bug was previously masked by the whole screen not rendering
     * correctly). 01:f0d3 does `SEP #$30; LDA $011c; AND #$0f; BEQ ...` --
     * the exact same absolute-addressing shape as 02:8525/02:9f37/02:9f43
     * above, just in the shared bank-1 code (home to several already-fixed
     * cursor/edge-detector sites). Same fix: repoint at $011b. Two
     * neighboring bank-1 sites the same capture surfaced, 01:8958
     * (`AND #$f0f0`) and 01:8c68 (`AND #$4f80`), are deliberate
     * non-direction gates (B/X-button checks) like 02:a4ec's Tax gate --
     * left alone. */
    {
      uint32_t off = 0xf0d4; /* 01:f0d3's low operand byte, file offset = addr (bank 1) */
      if (off < rom_size && rom_data[off] == 0x1c) {
        rom_data[off] = 0x1b;
        fprintf(stderr, "dpad fix: patched 01:f0d3 LDA $011c -> LDA $011b\n");
      } else {
        fprintf(stderr, "dpad fix: 01:f0d3 site NOT patched (byte mismatch)\n");
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
  /* No SDL_RENDERER_PRESENTVSYNC: on some hosts (observed under a VM) the
   * driver's vsync wait blocks for longer than one real display refresh
   * (e.g. ~33ms instead of ~16.67ms), silently halving the whole loop's
   * rate -- since simulation advancement here is 1:1 with each present,
   * that drags the SNES-side "logical" game down to half speed too (every
   * animation, not just this cursor), while the interpreter itself was
   * never the bottleneck (SC_FRAME_TIME showed zero frames exceeding the
   * 16.67ms budget). Pace manually against the wall clock instead below. */
  SDL_Renderer *renderer = SDL_CreateRenderer(
      window, -1, SDL_RENDERER_ACCELERATED);
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
  if (audio_dev) {
    SDL_PauseAudioDevice(audio_dev, 0);
    fprintf(stderr, "audio: opened freq=%d format=%04x channels=%d samples=%d\n",
            have.freq, have.format, have.channels, have.samples);
  } else {
    /* Previously silent on failure -- every audio code path below is
     * gated on `if (audio_dev)`, so a failed open just ran the whole
     * game with no sound and no indication why. */
    fprintf(stderr, "audio: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
  }

  double audio_acc = 0.0;
  int16_t audio_buf[1024 * 2];
  bool quit = false;
  /* Live FPS counter in the window title, updated once/sec -- lets a user
   * on a slow host (e.g. a VM) tell at a glance whether the emulator itself
   * is keeping up with real time, independent of anything ROM-side. */
  uint64_t fps_window_start = SDL_GetPerformanceCounter();
  uint64_t fps_window_frames = 0;
  /* Manual frame pacer, replacing vsync (see the renderer-creation comment
   * above): target the SNES's real ~60.0988fps, sleeping off any leftover
   * budget each loop iteration instead of blocking on a potentially-broken
   * driver vsync wait. */
  const double kTargetFrameSeconds = 1.0 / 60.0988;
  uint64_t next_frame_deadline = SDL_GetPerformanceCounter();
  while (!quit) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT) quit = true;
      if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) quit = true;
      if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_F1) {
        if (s_pc_bitmap_bank != -1) {
          if (s_pc_bitmap_bank == -2) memset(s_pc_bitmap_all, 0, sizeof(s_pc_bitmap_all));
          else memset(s_pc_bitmap, 0, sizeof(s_pc_bitmap));
        }
        s_gfx_trace_hits = 0;
        s_dbg_live_hits = 0;
        fprintf(stderr, "[F1] PC bitmap capture / gfx trace reset at frame %llu\n",
                (unsigned long long)s_frames);
      }
      /* F2: one-button entry of the documented debug-menu code on
       * controller 2 (see queue_debug_menu_code) -- press once while on
       * the "Goodbye! See you soon" quit-confirmation screen, instead of
       * hand-timing all 16 inputs. !ev.key.repeat so holding F2 doesn't
       * re-queue every auto-repeat tick. */
      if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_F2 && !ev.key.repeat) {
        queue_debug_menu_code(s_frames + 1);
      }
      /* F3: toggle host-mouse cursor control (see apply_mouse_delta below).
       * Off by default -- it's a ported experimental community patch, and
       * incidental OS mouse movement over the window shouldn't silently
       * steer the game cursor unless asked for. */
      if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_F3 && !ev.key.repeat) {
        s_mouse_enabled = !s_mouse_enabled;
        if (s_mouse_enabled) SDL_GetRelativeMouseState(NULL, NULL); /* discard stale accumulated delta */
        fprintf(stderr, "[F3] mouse cursor control %s\n", s_mouse_enabled ? "ON" : "OFF");
      }
      /* F5-F8: directly toggle the stock ROM's own debug-menu cheat flags
       * word, WRAM $0425 -- found by tracing a published Pro Action Replay
       * code list's "Enable Debugger" address (01:88e7, a boot-time load
       * from SRAM $700009 into $0425) forward to every site that reads
       * $0425, and the in-game debug-menu handler itself (00:da04-da3f,
       * which XORs a per-option bitmask table at 00:da50 into $0425, and
       * commits it to SRAM $700009 when "Memory: SET" is chosen). This
       * sidesteps needing the documented controller-2 entry code or menu
       * navigation entirely -- same end effect, poked directly.
       * Bit 0x02 (Needless Money) and 0x04 (Valve Max) are independently
       * confirmed by disassembling their consumers (01:bb7a's money-
       * deduction skip; 03:8b37's RCI-demand-meter force). Bits 0x01/0x08
       * are inferred from the option table's position/ordering only (not
       * yet confirmed against a consumer) -- label accordingly if this
       * turns out wrong. */
      if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
        const char *label = NULL; uint8_t bit = 0;
        switch (ev.key.keysym.scancode) {
          case SDL_SCANCODE_F5: label = "No Disasters (unconfirmed bit)"; bit = 0x01; break;
          case SDL_SCANCODE_F6: label = "Needless Money"; bit = 0x02; break;
          case SDL_SCANCODE_F7: label = "Valve Max"; bit = 0x04; break;
          case SDL_SCANCODE_F8: label = "Water Reclaim (unconfirmed bit)"; bit = 0x08; break;
          default: break;
        }
        if (bit) {
          g_ram[0x0425] ^= bit;
          fprintf(stderr, "[cheat] %s %s ($0425=%02x)\n", label,
                  (g_ram[0x0425] & bit) ? "ON" : "OFF", g_ram[0x0425]);
        }
      }
    }
    if (s_mouse_enabled) {
      int mdx = 0, mdy = 0;
      SDL_GetRelativeMouseState(&mdx, &mdy);
      if (mdx || mdy) apply_mouse_delta(mdx, mdy);
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

    /* Fast-forward: hold Tab to simulate several SNES frames per rendered/
     * presented frame instead of just one. Only the last of the batch's
     * audio gets queued (skipping the rest, rather than speeding it up or
     * garbling it) and only its video is presented -- the frame pacer
     * below still targets normal 60fps, so this is a real Nx speed-up in
     * game-time per real second, not just a faster/choppier render. Also
     * applied automatically (no key needed) while the map/scenario
     * generation loop is active -- see s_gen_loop_active_frames above. */
    bool fast_forward = keys[SDL_SCANCODE_TAB] || s_gen_loop_active_frames > 0;
    int frames_this_iter = fast_forward ? 6 : 1;

    /* SC_FRAME_TIME=<ms threshold>: log (rate-limited, 500 hits) wall-clock
     * time for any run_one_frame() call slower than the threshold -- there's
     * no frame-pacing throttle in this loop other than vsync on the present
     * call, so if simulating a frame's worth of 65816 instructions takes
     * longer than ~16.67ms on some screen, the game visibly runs below
     * 60fps on that screen specifically, with no other symptom. */
    const char *frame_time_thresh_env = getenv("SC_FRAME_TIME");
    uint64_t frame_t0 = frame_time_thresh_env ? SDL_GetPerformanceCounter() : 0;

    bool guard_tripped = false;
    for (int ffi = 0; ffi < frames_this_iter; ffi++) {
      if (!run_one_frame()) {
        fprintf(stderr, "frame %llu: opcode guard tripped (hang/runaway) -- stopping\n",
                (unsigned long long)s_frames);
        guard_tripped = true;
        break;
      }
      /* Extra fast-forward frames still need input re-armed exactly like
       * the top of this loop does every iteration: apply_frame_input()
       * resets input1_currentState to 0 (or any scripted qualify-mode
       * input) before the live keyboard state is OR'd back in -- skipping
       * the reset here would let stale bits accumulate across frames. */
      if (ffi + 1 < frames_this_iter) {
        apply_frame_input(s_frames);
        g_snes->input1_currentState |= input;
      }
    }
    if (guard_tripped) break;

    /* REVERTED (see docs/ROM_MAP.md or git history for the attempt):
     * fast-forward's audio comment above ("only the last of the batch's
     * audio gets queued, skipping the rest") describes intent that was
     * never actually enforced -- during a fast-forward batch, the DSP
     * ring genuinely accumulates several frames' worth of undrained
     * audio, which plays back later as an audible delay. Two different
     * attempts to discard that backlog each frame (a hand-rolled
     * sampleRead assignment, then the shared runner's own
     * dsp_trimSamples()) both caused a complete, permanent audio freeze
     * in live testing instead of just fixing the delay -- root cause not
     * found. Reverted rather than ship a "fix" that's worse than the
     * original symptom; the delay remains a known issue (see the sound
     * investigation thread). */

    if (frame_time_thresh_env) {
      static uint32_t s_frame_time_hits;
      double ms = (double)(SDL_GetPerformanceCounter() - frame_t0) * 1000.0 /
                  (double)SDL_GetPerformanceFrequency();
      double thresh = atof(frame_time_thresh_env);
      if (ms >= thresh && s_frame_time_hits < 500) {
        fprintf(stderr, "[frametime f=%llu] %.2fms\n", (unsigned long long)s_frames, ms);
        s_frame_time_hits++;
      }
    }

    if (getenv("SC_DEBUG_LIVE")) {
      /* Self-triggering: don't start logging until $01ed first changes
       * (i.e. movement has actually begun), so a few seconds of imprecise
       * F1 timing before/after the user actually holds a direction don't
       * burn through the hit cap on dead frames. */
      static uint8_t s_dbg_live_last_ed = 0xff;
      static bool s_dbg_live_armed;
      if (!s_dbg_live_armed) {
        if (s_dbg_live_last_ed == 0xff) s_dbg_live_last_ed = g_ram[0x01ed];
        else if (g_ram[0x01ed] != s_dbg_live_last_ed) s_dbg_live_armed = true;
      }
      if (s_dbg_live_armed && s_dbg_live_hits < 2000) {
        fprintf(stderr, "[dbgl f=%llu] $01ed=%02x $00d7=%02x $01f3=%02x $01ff=%02x\n",
                (unsigned long long)s_frames, g_ram[0x01ed], g_ram[0xd7],
                g_ram[0x01f3], g_ram[0x01ff]);
        s_dbg_live_hits++;
      }
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
      static uint64_t s_audio_dbg_queued, s_audio_dbg_calls, s_audio_dbg_fails;
      if (available >= 534 && wantN > 0 && wantN <= 1024) {
        audio_acc -= (double)wantN;
        dsp_getSamples(dsp, audio_buf, wantN);
        int qrc = SDL_QueueAudio(audio_dev, audio_buf, (Uint32)(wantN * 2 * sizeof(int16_t)));
        s_audio_dbg_calls++;
        if (qrc != 0) s_audio_dbg_fails++;
        else s_audio_dbg_queued += (uint64_t)wantN;
      }
      /* SC_AUDIO_DEBUG: periodic drain-loop status, for diagnosing
       * windowed-only audio issues -- headless qualify mode shows healthy
       * DSP production (92% active frames) as a baseline, so if this
       * never fires or queued/drained stay at 0, the bug is specifically
       * in this drain loop or the SDL device, not the underlying audio
       * synthesis. Built while chasing a reported total-silence bug that
       * turned out to be self-inflicted (see the revert above) -- kept
       * for next time. */
      if (getenv("SC_AUDIO_DEBUG") && (s_frames % 180) == 0) {
        fprintf(stderr, "audio: f=%llu queued_dev=%u drained_total=%llu calls=%llu fails=%llu\n",
                (unsigned long long)s_frames, SDL_GetQueuedAudioSize(audio_dev),
                (unsigned long long)s_audio_dbg_queued, (unsigned long long)s_audio_dbg_calls,
                (unsigned long long)s_audio_dbg_fails);
      }
    }

    void *pixels; int pitch;
    SDL_LockTexture(texture, NULL, &pixels, &pitch);
    memcpy(pixels, s_video_pixels, sizeof(s_video_pixels));
    SDL_UnlockTexture(texture);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);

    next_frame_deadline += (uint64_t)(kTargetFrameSeconds * (double)SDL_GetPerformanceFrequency());
    uint64_t now = SDL_GetPerformanceCounter();
    if (now < next_frame_deadline) {
      double remaining_ms = (double)(next_frame_deadline - now) * 1000.0 /
                             (double)SDL_GetPerformanceFrequency();
      if (remaining_ms > 1.0) SDL_Delay((Uint32)(remaining_ms - 1.0));
      while (SDL_GetPerformanceCounter() < next_frame_deadline) { /* spin for the last <1ms */ }
    } else {
      /* Running behind (e.g. this frame's work overran budget) -- don't
       * try to catch up by presenting a burst of frames back-to-back;
       * just resync the deadline to now so pacing doesn't accumulate
       * drift after a one-off slow frame. */
      next_frame_deadline = now;
    }
    SDL_RenderPresent(renderer);

    fps_window_frames++;
    double fps_window_elapsed = (double)(SDL_GetPerformanceCounter() - fps_window_start) /
                                 (double)SDL_GetPerformanceFrequency();
    if (fps_window_elapsed >= 1.0) {
      char title[128];
      snprintf(title, sizeof(title), "SimCitySNESRecomp -- %.1f fps",
               (double)fps_window_frames / fps_window_elapsed);
      SDL_SetWindowTitle(window, title);
      fps_window_frames = 0;
      fps_window_start = SDL_GetPerformanceCounter();
    }
  }

  if (audio_dev) SDL_CloseAudioDevice(audio_dev);
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  write_pc_bitmap_dump();
  return 0;
}
