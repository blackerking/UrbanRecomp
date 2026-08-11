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
#include <ctype.h>

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
 *                           Mode 7 + HDMA (e.g. the tilted "View" map).
 * SC_DECOMP_TRACE=1         log every call to the LC_LZ5 decompressor at
 *                           00:90dd -- source pointer, destination, and the
 *                           true output extent measured off the bus. This is
 *                           what settled the scenario-map format; see the
 *                           long comment above bus_read and
 *                           docs/REFERENCE_map_format.md.
 * SC_MAP_WRITE_TRACE=1      log writes into the live 24000-byte map buffer at
 *                           $7F0200, with a distinct-PC histogram at exit --
 *                           finds what fills the map without assuming which
 *                           routine does it.
 * SC_FRAME_BANK_TRACE=<start-frame>,<end-frame>
 *                           log the CPU's bank:PC at every frame boundary
 *                           in that range, unconditionally (not gated on
 *                           reaching any particular PC) -- for finding
 *                           what's actually executing during frames a
 *                           lower-priority per-frame task doesn't get a
 *                           turn on. This is what found bank $03 (the city
 *                           simulation tick) cooperatively pre-empting the
 *                           bank $01 cursor dispatcher for several frames
 *                           at a time -- see docs/INVESTIGATION_cursor_
 *                           cadence.md. PC-history alone can't answer this
 *                           kind of question: its ring buffer is far too
 *                           shallow (128 opcodes) to span multiple whole
 *                           frames of execution. */
static bool s_gfx_trace;
static uint32_t s_gfx_trace_hits;
static bool s_view_watch; /* cached SC_VIEW_WATCH check -- see bus_read/bus_write;
                            * getenv() is NOT cheap enough to call unconditionally
                            * on every single memory access (this is what caused a
                            * qualify hang/severe slowdown before being cached). */
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

/* SC_DECOMP_TRACE=1: instrument the LC_LZ5 decompressor at 00:90dd, logging
 * one line per call with its source pointer, its output range, and how many
 * bytes it actually produced.
 *
 * This exists to settle the scenario-map compression question (see
 * docs/REFERENCE_map_format.md): the eight scenario map pointers in the
 * struct-of-arrays table at 03:ce70 are known, but the format they're stored
 * in is not, and two static guesses (LC_LZ5 at the pointer itself; a raw
 * 16-bit cell array) have already been tested and failed. Rather than guess a
 * third time, watch the load path do it. Correct output is known exactly --
 * 24000 bytes (120x100 cells x 2) of 10-bit indices -- so a call whose output
 * length is 24000 is the map load, whatever its source turns out to be.
 *
 * Calling convention, decoded by hand from 00:90dd (the disassembler
 * misaligns here: SEP #$20 makes A 8-bit, so `c9 ff` at 00:9102 is CMP #$ff,
 * the terminator test, not a 16-bit compare):
 *
 *   00:90dd  PHP / PHB / SEP #$20 / REP #$10
 *   00:90e3  LDA $000b ; PHA ; PLB     ; DB = source bank
 *   00:90eb  LDX $000e                 ; X   = output index
 *   00:90ef  LDY $0009                 ; Y   = source offset
 *   ...      STA $7e8000,X             ; output base is $7E8000, not $7E0000
 *   00:9106  PLB / PLP / RTS           ; reached on the $ff terminator
 *
 * so the source is $0b:$0009 and the destination is $7E8000 + $000e. Note
 * $7E8000 + a 16-bit X spans up to $7F7FFF, which does reach the live map
 * buffer at $7F0200 ($7E8000 + $8200).
 *
 * Entry is sampled at 00:90eb rather than 00:90dd so cpu->db is already the
 * source bank: `LDA $000b` at 00:90e3 runs under the *caller's* DB, so the
 * post-PLB register is the authoritative source bank, not our own read of
 * $000b. The bank-cross helper at 00:926d bumps the DB register (and resets Y
 * to $8000) without updating $000b, so the exit sample must come from cpu->db
 * too -- taken at 00:9106, before PLB restores the caller's bank. */
static bool s_decomp_trace;
static bool s_decomp_active;
static uint32_t s_decomp_src;       /* (bank<<16)|offset, sampled at entry */
static uint32_t s_decomp_out_base;  /* $7e8000 + $000e, sampled at entry */
static uint32_t s_decomp_wmin, s_decomp_wmax; /* observed output extent */
static uint32_t s_decomp_wcount;
static uint32_t s_decomp_calls;

/* SC_MAP_WRITE_TRACE=1: watch every write into the live map buffer
 * ($7F0200..$7F607F, 120x100x2 bytes) and report which PCs produce it.
 *
 * Complements the decompressor trace above rather than duplicating it: it
 * makes no assumption that 00:90dd is what fills the map. If the scenario
 * maps are unpacked by some other routine entirely, this finds it, and if
 * nothing writes the region at all then the map lives somewhere other than
 * where the Lua viewer reads it. Rate-limited to the first few writes with
 * full PCs, plus a distinct-PC histogram at exit, since a full map fill is
 * 24000 writes. */
#define kMapBufStart 0x7f0200u
#define kMapBufEnd   (0x7f0200u + 24000u)
static bool s_map_write_trace;
static uint32_t s_map_write_hits;
#define SC_MAP_WRITE_PCS 16
static struct { uint32_t pc; uint32_t count; } s_map_write_pcs[SC_MAP_WRITE_PCS];
static int s_map_write_pc_count;

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
  /* SC_VIEW_WATCH=1: live read watch on $7e21b4/$7e21b5 (the View screen's
   * D-pad-adjusted position -- write side confirmed working, but nothing
   * visibly renders; see docs/INVESTIGATION_dpad.md "Open item"). Catches
   * *every* addressing mode (including dynamic/indirect), unlike a static
   * opcode-pattern scan, which only found the value's own read-modify-
   * write increment, not a genuine external consumer. */
  if (s_view_watch && bank == 0x7e && (reg == 0x21b4 || reg == 0x21b5)) {
    static uint32_t s_view_watch_hits;
    if (s_view_watch_hits < 400) {
      fprintf(stderr, "[viewwatch f=%llu] pc=%02x:%04x READ $7e%04x = %02x\n",
              (unsigned long long)s_frames, g_cpu->k, g_cpu->pc, reg, v);
      s_view_watch_hits++;
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
  /* Output-extent tracking for SC_DECOMP_TRACE. Deliberately measured from
   * the bus rather than from X at the RTS: it captures what the routine
   * really wrote, including any bank-crossing past $7E:ffff, and needs no
   * assumption about which register holds the final output index. */
  if (s_decomp_active && (bank == 0x7e || bank == 0x7f)) {
    if (adr < s_decomp_wmin) s_decomp_wmin = adr;
    if (adr > s_decomp_wmax) s_decomp_wmax = adr;
    s_decomp_wcount++;
  }
  if (s_map_write_trace && adr >= kMapBufStart && adr < kMapBufEnd) {
    uint32_t pc = ((uint32_t)g_cpu->k << 16) | g_cpu->pc;
    if (s_map_write_hits < 12)
      fprintf(stderr, "[mapwrite f=%llu] pc=%02x:%04x $%06x = %02x\n",
              (unsigned long long)s_frames, g_cpu->k, g_cpu->pc, adr, v);
    s_map_write_hits++;
    int i = 0;
    for (; i < s_map_write_pc_count; i++)
      if (s_map_write_pcs[i].pc == pc) { s_map_write_pcs[i].count++; break; }
    if (i == s_map_write_pc_count && s_map_write_pc_count < SC_MAP_WRITE_PCS) {
      s_map_write_pcs[s_map_write_pc_count].pc = pc;
      s_map_write_pcs[s_map_write_pc_count].count = 1;
      s_map_write_pc_count++;
    }
  }
  if (s_view_watch && bank == 0x7e && (reg == 0x21b4 || reg == 0x21b5)) {
    static uint32_t s_view_write_hits;
    if (s_view_write_hits < 400) {
      fprintf(stderr, "[viewwatch f=%llu] pc=%02x:%04x WRITE $7e%04x = %02x\n",
              (unsigned long long)s_frames, g_cpu->k, g_cpu->pc, reg, v);
      s_view_write_hits++;
    }
  }
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
  /* Dedicated $011b/$011c write watch (separate counter from the mouse-
   * cursor watch above so the two don't compete for the same hit budget).
   * Added to find what, if anything, writes over the edge-detector's
   * (00:92c7) correct per-frame data in between its write and the
   * fast-travel modifier check (01:c01e) reading it as zero four frames
   * out of five -- see docs/INVESTIGATION_dpad.md "Fast travel". Paired
   * with the existing readwatch above (same two addresses), so a single
   * SC_CADENCE_WATCH run gives every read AND write to both bytes, in
   * order, across consecutive frames. */
  if (s_addr_trace_count && s_frames >= s_addr_trace_start_frame &&
      getenv("SC_CADENCE_WATCH") &&
      (reg == 0x011b || reg == 0x011c)) {
    static uint32_t s_011b_write_hits;
    if (s_011b_write_hits < 400) {
      fprintf(stderr, "[011bwrite f=%llu] pc=%02x:%04x $%04x = %02x\n",
              (unsigned long long)s_frames, g_cpu->k, g_cpu->pc, reg, v);
      s_011b_write_hits++;
    }
  }
  /* $d7 write watch: the dispatcher-select state machine (see ROM_MAP.md
   * `$00d7` entry) -- `01:8b3b` branches on it to pick the default
   * cursor-sprite dispatcher ($d7==0, only reaches the B/X reason-1 path)
   * vs. the second dispatcher at `01:8c55` ($d7==1, "the one that actually
   * handles Y+direction (fast travel)"). Every trace this session (bsnes
   * and this recomp) has shown $d7==0 throughout a held Y+direction --
   * this watch is to find whether/when anything ever writes it to 1. */
  if (s_addr_trace_count && s_frames >= s_addr_trace_start_frame &&
      getenv("SC_CADENCE_WATCH") && reg == 0x00d7) {
    static uint32_t s_d7_write_hits;
    if (s_d7_write_hits < 200) {
      fprintf(stderr, "[d7write f=%llu] pc=%02x:%04x $d7 = %02x\n",
              (unsigned long long)s_frames, g_cpu->k, g_cpu->pc, v);
      s_d7_write_hits++;
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
    if (snes->vPos == 262) {
      snes->vPos = 0;
      s_frames++;
      /* SC_FRAME_BANK_TRACE=<start>,<end>: print the CPU bank:PC at every
       * frame boundary in that range -- for finding what's actually
       * executing during "gap" frames a lower-priority per-frame task
       * (like the cursor dispatcher) doesn't get a turn on, instead of
       * guessing from PC-history (too shallow to span multiple frames). */
      { const char *fbt = getenv("SC_FRAME_BANK_TRACE");
        if (fbt && *fbt) {
          static uint64_t s_fbt_start, s_fbt_end;
          static bool s_fbt_parsed;
          if (!s_fbt_parsed) {
            sscanf(fbt, "%llu,%llu", (unsigned long long *)&s_fbt_start, (unsigned long long *)&s_fbt_end);
            s_fbt_parsed = true;
          }
          if (s_frames >= s_fbt_start && s_frames <= s_fbt_end)
            fprintf(stderr, "[framebank f=%llu] k=%02x pc=%04x\n",
                    (unsigned long long)s_frames, g_cpu->k, g_cpu->pc);
        }
      }
    }
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
 * see tools/extract_graphics.py and docs/REFERENCE_third_party_optimization_patch.md).
 *
 * DEFAULT OFF as of the settings-menu work -- this fired during ordinary
 * gameplay, not just the load screen, and the resulting intermittent 6x
 * bursts made the game feel rough and badly worsened the known
 * fast-forward audio-delay problem (see the revert note in the main loop).
 * Measured with SC_ADDR_TRACE on the three trigger PCs against real
 * gameplay save states: on the classic map screen it fired sporadically
 * (~4 times in 2000 frames, each arming a 20-frame boost), but on the
 * View screen it fired roughly every 10-13 frames -- i.e. that screen sat
 * in effectively *continuous* turbo, since each hit re-arms the holdoff
 * before the previous one expires.
 *
 * Root cause of the false positives: 00:824b is the shared checksum/hash
 * accumulator, not map-generation-specific code -- the game calls it
 * during normal simulation too. It's also redundant as a trigger, since
 * 03:d862 (the map-gen loop that calls it) is itself already a trigger,
 * so it's dropped from the trigger set entirely rather than merely gated.
 * The two remaining triggers are genuinely load-specific. Toggle the
 * feature from the F10 settings menu ("AUTO TURBO ON LOAD"); Tab-held
 * manual fast-forward is unaffected either way. */
static bool s_auto_turbo_enabled; /* off by default -- see above */
static int s_gen_loop_active_frames; /* counts down; >0 means "recently seen" */
#define SC_GEN_LOOP_HOLDOFF 20 /* frames to keep boosting after the last hit */

static bool run_one_frame(void) {
  Snes *snes = g_snes;
  Interp816 *cpu = g_cpu;
  uint64_t target = s_frames + 1;
  long guard = 20000000; /* runaway guard: caps opcodes/frame, mirrors ref_driver.c */
  while (s_frames < target && guard-- > 0) {
    if (cpu->k == 0x00 && cpu->pc == 0x80b2) s_nmi_serviced++;
    if (s_auto_turbo_enabled &&
        ((cpu->k == 0x03 && cpu->pc == 0xd862) || (cpu->k == 0x00 && cpu->pc == 0x90dd)))
      s_gen_loop_active_frames = SC_GEN_LOOP_HOLDOFF;
    /* LC_LZ5 decompressor instrumentation -- see the SC_DECOMP_TRACE comment
     * above bus_read for the decoded calling convention and why the samples
     * are taken at 00:90eb / 00:9106 rather than at the JSR and the RTS. */
    if (s_decomp_trace && cpu->k == 0x00) {
      if (cpu->pc == 0x90eb) {
        s_decomp_active = true;
        s_decomp_src = ((uint32_t)cpu->db << 16) |
                       g_ram[0x09] | ((uint32_t)g_ram[0x0a] << 8);
        s_decomp_out_base = 0x7e8000u + (g_ram[0x0e] | ((uint32_t)g_ram[0x0f] << 8));
        s_decomp_wmin = 0xffffffffu;
        s_decomp_wmax = 0;
        s_decomp_wcount = 0;
      } else if (s_decomp_active && cpu->pc == 0x9106) {
        uint32_t src_end = ((uint32_t)cpu->db << 16) |
                           g_ram[0x09] | ((uint32_t)g_ram[0x0a] << 8);
        /* Compressed size has to be measured in LoROM *file* offsets, not by
         * subtracting the 24-bit addresses: 00:926d advances the pointer by
         * one bank per 32KB window ($8000..$ffff), so plain address
         * subtraction over-counts a bank crossing by $8000 and reports a
         * source longer than its own output. */
        uint32_t src_file = ((s_decomp_src >> 16) * 0x8000u) +
                            ((s_decomp_src & 0xffff) - 0x8000u);
        uint32_t end_file = ((src_end >> 16) * 0x8000u) +
                            ((src_end & 0xffff) - 0x8000u);
        fprintf(stderr,
                "[decomp #%u f=%llu] src=%02x:%04x..%02x:%04x (file %06x, %u in) "
                "dst=%06x wrote=%u range=%06x..%06x\n",
                s_decomp_calls++, (unsigned long long)s_frames,
                (unsigned)(s_decomp_src >> 16), (unsigned)(s_decomp_src & 0xffff),
                (unsigned)(src_end >> 16), (unsigned)(src_end & 0xffff),
                src_file, (unsigned)(end_file - src_file), s_decomp_out_base,
                s_decomp_wcount,
                s_decomp_wcount ? s_decomp_wmin : 0,
                s_decomp_wcount ? s_decomp_wmax : 0);
        s_decomp_active = false;
      }
    }
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

/* Dumps the actual composited SDL renderer output (game frame + any host
 * overlay drawn on top, e.g. the settings menu) rather than just the raw
 * SNES framebuffer write_ppm() above captures -- used by SC_MENU_PREVIEW
 * (see main()) for headless visual verification of overlay UI, the same
 * kind of screenshot a windowed browser dev-tools check gives for web UI,
 * which this native SDL window otherwise has no equivalent of. */
static bool write_renderer_ppm(SDL_Renderer *renderer, const char *path) {
  int w = 0, h = 0;
  SDL_GetRendererOutputSize(renderer, &w, &h);
  if (w <= 0 || h <= 0) return false;
  uint32_t *buf = (uint32_t *)malloc((size_t)w * (size_t)h * 4);
  if (!buf) return false;
  bool ok = SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, buf, w * 4) == 0;
  if (ok) {
    FILE *f = fopen(path, "wb");
    if (f) {
      fprintf(f, "P6\n%d %d\n255\n", w, h);
      for (int i = 0; i < w * h; i++) {
        uint8_t rgb[3] = { (uint8_t)(buf[i] >> 16), (uint8_t)(buf[i] >> 8), (uint8_t)buf[i] };
        if (fwrite(rgb, 1, 3, f) != 3) { ok = false; break; }
      }
      ok = fclose(f) == 0 && ok;
    } else ok = false;
  }
  free(buf);
  return ok;
}

static bool write_wram_dump(const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  bool ok = fwrite(g_ram, 1, sizeof(g_ram), f) == sizeof(g_ram);
  return fclose(f) == 0 && ok;
}

/* SC_SRAM_DUMP_PATH=<file>: dump the 32KB cart SRAM window ($700000-$707fff)
 * at exit. SRAM is NOT part of g_ram -- it lives in the cart model -- so a
 * WRAM dump does not capture it and it has to be read back through the bus.
 * Wanted for the save-game/scenario-record work: the layout at $700000 is a
 * 14-byte header (magic "SIM", flags, checksum) followed by the per-city save
 * block, and the only practical way to check a field's meaning is to diff two
 * dumps. */
static bool write_sram_dump(const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  bool ok = true;
  for (uint32_t i = 0; i < 0x8000 && ok; i++)
    ok = fputc(snes_read(g_snes, 0x700000 + i), f) != EOF;
  return fclose(f) == 0 && ok;
}

/* Decode of the SRAM header, printed alongside the dump -- see
 * apply_unlock_all() for where each field comes from in the ROM. */
static void report_sram_header(const char *when) {
  uint8_t h[16];
  for (int i = 0; i < 16; i++) h[i] = snes_read(g_snes, 0x700000 + i);
  uint16_t sum = 0;
  for (int i = 0; i < 14; i++) sum = (uint16_t)(sum + h[i]);
  uint16_t flags = (uint16_t)(h[7] | (h[8] << 8));
  fprintf(stderr, "[sram %s] magic=%c%c%c win=%04x (scenarios", when,
          h[0] >= 32 ? h[0] : '?', h[1] >= 32 ? h[1] : '?',
          h[2] >= 32 ? h[2] : '?', flags);
  for (int i = 0; i < 8; i++) if (flags & (1u << i)) fprintf(stderr, " %d", i);
  fprintf(stderr, "%s) stored_sum=%04x computed=%04x%s\n",
          (flags & 0x8000) ? ", ALL" : "",
          (uint16_t)(h[14] | (h[15] << 8)), sum,
          (uint16_t)(h[14] | (h[15] << 8)) == sum ? "" : "  MISMATCH");
}

/* ── save states -- for reproducing a specific screen/input scenario (e.g.
 * "on the map, cursor visible, nothing else held") instantly and
 * deterministically, instead of re-navigating menus by hand or by guessed
 * --input timing every single test run. snes_saveload() already covers the
 * full device model (cpu/apu/dma/ppu/cart + WRAM, since g_ram is snes->ram);
 * interp816_saveload() separately covers the actual CPU registers this
 * Phase-1 host runs on (snes->cpu is an unused legacy AOT-tier struct, not
 * what interp816 drives). s_frames is saved too so frame-numbered tooling
 * (SC_ADDR_TRACE's @start-frame, --input's start:duration) stays meaningful
 * across a load instead of resetting to 0. Host-only UI state (mouse
 * toggle, etc.) is deliberately not saved -- reloading shouldn't change
 * host input mode out from under you. */
typedef struct { SaveLoadInfo base; FILE *f; bool ok; } FileSli;
static void file_sli_write(SaveLoadInfo *sli, void *data, size_t n) {
  FileSli *fs = (FileSli *)sli;
  if (fs->ok && fwrite(data, 1, n, fs->f) != n) fs->ok = false;
}
static void file_sli_read(SaveLoadInfo *sli, void *data, size_t n) {
  FileSli *fs = (FileSli *)sli;
  if (fs->ok && fread(data, 1, n, fs->f) != n) fs->ok = false;
}

static bool save_state(const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  FileSli fs;
  fs.base.func = file_sli_write;
  fs.f = f;
  fs.ok = true;
  snes_saveload(g_snes, &fs.base);
  interp816_saveload(g_cpu, &fs.base);
  fs.base.func(&fs.base, &s_frames, sizeof(s_frames));
  bool ok = fs.ok;
  return fclose(f) == 0 && ok;
}

static bool load_state(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  FileSli fs;
  fs.base.func = file_sli_read;
  fs.f = f;
  fs.ok = true;
  snes_saveload(g_snes, &fs.base);
  interp816_saveload(g_cpu, &fs.base);
  fs.base.func(&fs.base, &s_frames, sizeof(s_frames));
  bool ok = fs.ok;
  fclose(f);
  return ok;
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

/* SC_FREEZE=<addr>:<val>[,<addr>:<val>...] -- hold WRAM bytes at fixed
 * values every frame, the same thing a bsnes "freeze"/cheat does. Both
 * addresses and values are hex; addresses are WRAM offsets (so $7e01ed is
 * just `1ed`). Applied once per emulated frame, so it survives the ROM
 * rewriting the byte itself -- unlike a one-shot poke after --load-state.
 *
 * This exists because "is byte X the thing gating behaviour Y?" keeps
 * being the decisive question in this project's investigations, and until
 * now the only ways to answer it were a ROM byte-patch (changes the code,
 * not the state) or asking the user to drive bsnes by hand. */
#define SC_FREEZE_MAX 16
static struct { uint32_t addr; uint8_t val; } s_freezes[SC_FREEZE_MAX];
static int s_freeze_count;

static void parse_freezes(const char *spec) {
  char buf[256];
  snprintf(buf, sizeof(buf), "%s", spec);
  for (char *tok = strtok(buf, ","); tok && s_freeze_count < SC_FREEZE_MAX;
       tok = strtok(NULL, ",")) {
    unsigned a = 0, v = 0;
    if (sscanf(tok, "%x:%x", &a, &v) == 2 && a < sizeof(g_ram)) {
      s_freezes[s_freeze_count].addr = a;
      s_freezes[s_freeze_count].val = (uint8_t)v;
      s_freeze_count++;
      fprintf(stderr, "freeze: $%05x = %02x\n", a, v);
    } else {
      fprintf(stderr, "freeze: bad spec '%s' (want hexaddr:hexval)\n", tok);
    }
  }
}

/* Scenario override: 0 = off, otherwise hold $0040 (the scenario index)
 * at this value every frame. The Select Scenario screen can only produce
 * indices 0-5 -- 03:de0e computes `$40 = $54*3 + $52`, a 2x3 grid -- but
 * the ROM's own deadline table at $03c5b3 has EIGHT entries, and both
 * extra scenarios are genuinely present in this ROM: index 6 is Las Vegas
 * (start 2096, 10-year limit, deadline 2106), whose briefing screen,
 * artwork and full text all load correctly once the index is forced;
 * index 7 carries the sentinel deadline $ffff (no time limit) and the
 * win/lose evaluator at 03:c548 deliberately returns without writing a
 * result for it -- i.e. free play. Holding the index is exactly how the
 * hidden scenario was confirmed, and is far less invasive than
 * restructuring the selection grid. Set it, then start a scenario as
 * normal. */
static int s_scenario_override;
static const int kScenarioOverrides[] = { 0, 6, 7 };

/* Population override, for exercising the population milestone messages.
 * Population is 32-bit little-endian at $0BA5 (low word) + $0BA7 (high
 * word) -- both halves matter, since the city-class ladder at 03:81d8
 * tests the high word first. Held every frame while active, because the
 * simulation rewrites population continuously and a one-shot poke would
 * be overwritten before the milestone check next runs.
 *
 * -1 means off. 0 is a real selectable value (it is one of the levels
 * worth testing), which is why "off" cannot just be 0 here.
 *
 * The values are the class thresholds from 03:81d8 -- 2000/10000/50000/
 * 100000/500000 -- plus 600000, which is NOT a threshold in the ROM (no
 * such constant exists in any encoding) but is included precisely so the
 * claim can be tested in-game rather than argued from disassembly. */
static int s_pop_override = -1;
static const int kPopOverrides[] = { -1, 0, 2000, 10000, 50000, 100000, 500000, 600000 };

/* City class override ($0deb, 0-5 = Village/Town/City/Capital/Metropolis/
 * Megalopolis). This is what the milestone triggers actually test, and
 * setting population alone does NOT move it: measured, $0deb stayed at 3
 * for 2500 frames with population frozen at 500000. $0deb is persistent
 * state rather than a per-frame derivation -- 03:c96e loads it from SRAM
 * $700036 on save-load, and 03:ce94 seeds it per scenario from the table
 * at $03cee9. The ladder at 03:81d8 that derives it from population runs
 * only occasionally. So to exercise the milestone messages, drive this
 * directly. -1 = off. */
static int s_class_override = -1;
static const int kClassOverrides[] = { -1, 0, 1, 2, 3, 4, 5 };

/* The milestone messages are one-shot: each is guarded by a latch byte
 * that the trigger increments when it fires (03:c350 -> $0cbd, 03:c369 ->
 * $0cbf, 03:c3b4 -> $0cc1, 03:c396 -> $0cc3). Zeroing them re-arms every
 * milestone so a message can be made to fire again on demand -- otherwise
 * a population override only ever works once per session. */
static void menu_action_clear_milestones(void) {
  g_ram[0x0cbd] = 0; g_ram[0x0cbf] = 0;
  g_ram[0x0cc1] = 0; g_ram[0x0cc3] = 0;
  fprintf(stderr, "[menu] cleared milestone latches $0cbd/$0cbf/$0cc1/$0cc3\n");
}

/* Scenario completion ("win mark") flags, SRAM $700007.
 *
 * Setting these is what makes the SCENARIO OVR hack above redundant: the
 * scenarios stop needing to be reached by forcing $0040, because the game
 * itself offers them.
 *
 * The bitfield and everything around it were read off the ROM:
 *
 *   03:e30a  ORA $e334,Y     scenario index * 2 indexes a mask table at
 *                            03:e334 -- $0001, $0002, $0004 ... $0080, so
 *                            bit N marks scenario N complete
 *   03:e311  AND #$003f      ...and once the low SIX bits are all set,
 *   03:e315  CMP #$003f      03:e31c ORA #$8000 sets bit 15, the game's own
 *                            "every scenario beaten" flag
 *   03:e326  STA $700007     committed to SRAM
 *   03:e36c  STA $42         and read back the other way at init, SRAM into
 *                            the direct-page word $42
 *
 * Writing $700007 alone is not enough. SRAM carries a 14-byte header with a
 * magic and a checksum, both of which the game verifies at boot (03:e411's
 * sum loop and 03:e42d's 'S','I','M' test); a header that fails either is
 * restored from the backup copy at $707ff0 (03:e446), which would silently
 * undo this. So the full commit path from 03:e553 is reproduced here:
 * recompute the checksum over $700000-$70000d into $70000e, then mirror the
 * whole 16-byte header to $707ff0.
 *
 * Bits 0-6 are set, i.e. the six ordinary scenarios plus Las Vegas -- every
 * scenario, which is what makes reaching the hidden one a normal menu
 * selection. Indices 7 and 8 (Freeland and the tutorial) are deliberately
 * left alone: they are not scenarios and have nothing to win.
 *
 * SRAM lives in the cart model (`cart->ram`), not in g_ram, so it has to go
 * through the bus rather than a direct array write -- and it is not persisted
 * to disk by this host, so the unlock lasts for the session and is captured
 * by save states (which snapshot every device model), but does not survive a
 * fresh launch on its own. */
#define kWinMarkBits 0x007fu   /* scenarios 0-6 */
static bool s_unlock_all;

static void apply_unlock_all(void) {
  /* Only ever modify an SRAM the game has already formatted. A header failing
   * the magic test at 03:e42d gets restored from backup or rewritten, so
   * flags written into a blank SRAM would simply be undone -- and measured,
   * SRAM really is still blank well into a run: none of 03:e360/03:e40e/
   * 03:e45b executes at all in 3600 frames from a cold boot, because the
   * whole SRAM subsystem is only reached through a real game session. The
   * alternative -- fabricating the magic ourselves -- would mean claiming a
   * formatted save whose body is zeroed, so wait for the game instead. */
  if (snes_read(g_snes, 0x700000) != 'S' ||
      snes_read(g_snes, 0x700001) != 'I' ||
      snes_read(g_snes, 0x700002) != 'M')
    return;
  uint16_t flags = (uint16_t)(snes_read(g_snes, 0x700007) |
                              ((uint16_t)snes_read(g_snes, 0x700008) << 8));
  uint16_t want = (uint16_t)(flags | kWinMarkBits);
  if ((want & 0x003f) == 0x003f) want |= 0x8000;  /* 03:e31c */
  /* Idempotent: after the first application this is two bus reads a frame and
   * nothing else, so it never fights the game's own writes to the header. */
  if (want == flags) return;

  snes_write(g_snes, 0x700007, (uint8_t)want);
  snes_write(g_snes, 0x700008, (uint8_t)(want >> 8));

  uint16_t sum = 0;                                /* 03:e553 */
  for (int i = 0; i < 14; i++)
    sum = (uint16_t)(sum + snes_read(g_snes, 0x700000 + i));
  snes_write(g_snes, 0x70000e, (uint8_t)sum);
  snes_write(g_snes, 0x70000f, (uint8_t)(sum >> 8));

  for (int i = 0; i < 16; i++)                     /* 03:e484 */
    snes_write(g_snes, 0x707ff0 + i, snes_read(g_snes, 0x700000 + i));

  /* Keep the live direct-page copy in step, so this also takes effect after
   * 03:e36c has already run rather than only on a later re-read. */
  g_ram[0x42] = (uint8_t)want;
  g_ram[0x43] = (uint8_t)(want >> 8);

  fprintf(stderr, "[unlock] scenario win marks $700007: %04x -> %04x "
          "(checksum %04x)\n", flags, want, sum);
}

static void apply_freezes(void) {
  for (int i = 0; i < s_freeze_count; i++)
    g_ram[s_freezes[i].addr] = s_freezes[i].val;
  if (s_unlock_all) apply_unlock_all();
  if (s_scenario_override) g_ram[0x0040] = (uint8_t)s_scenario_override;
  if (s_pop_override >= 0) {
    uint32_t p = (uint32_t)s_pop_override;
    g_ram[0x0ba5] = (uint8_t)p;
    g_ram[0x0ba6] = (uint8_t)(p >> 8);
    g_ram[0x0ba7] = (uint8_t)(p >> 16);
    g_ram[0x0ba8] = (uint8_t)(p >> 24);
  }
  if (s_class_override >= 0) g_ram[0x0deb] = (uint8_t)s_class_override;
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

/* Fast D-pad cursor (opt-in, F9): rather than reverse-engineer and patch
 * the ROM's own throttled cursor cadence (see docs/INVESTIGATION_
 * cursor_cadence.md -- $01f3, $01ff, and the deeper bank-$03 simulation-
 * tick preemption that ultimately paces it), reuse the same host-side
 * bypass the mouse patch above already established: while a direction is
 * held, poke $01eb/$01ed directly via apply_mouse_delta() every frame,
 * completely independent of the ROM's own per-frame dispatcher. This is
 * strictly additive -- the normal D-pad bits are still sent to the game
 * as usual (menu navigation, edge-detected list movement, etc. are
 * untouched), this just adds extra host-driven displacement on top for
 * the main-map cursor specifically, so holding a direction moves it at
 * full host speed instead of whatever cadence the ROM's own cooperative
 * scheduler happens to allow it that frame. Same caveats as the mouse
 * patch it reuses (menu jank, no bounds-replication beyond the simple
 * clamp already in apply_mouse_delta) -- off by default. */
static bool s_fast_cursor_enabled;
/* Pixels/frame while a direction is held. Adjustable (F10 menu ->
 * "CURSOR SPEED") rather than fixed, because the stock cursor's real
 * pacing turns out to be the game's own cooperative scheduler and can't
 * be tuned ROM-side: traced live, bank $03 (the city simulation) holds
 * the CPU for ~4 consecutive frames at a time, during which the bank-1
 * cursor dispatcher never runs at all, so the cursor steps its 2 pixels
 * only on the bank-1 frames -- a 4-on/4-off duty cycle averaging ~1
 * px/frame (~4s to cross the screen). That is authentic behaviour, not a
 * recomp defect, and is presumably why the cartridge shipped with SNES
 * Mouse support. This host-side nudge is the practical remedy. */
static int s_fast_cursor_step = 4;
static const int kFastCursorSteps[] = { 2, 4, 8, 16 };

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

/* ── minimal in-game settings menu ───────────────────────────────────────
 * Follows the pattern researched from ar-recomp (ActRaiser recomp)'s
 * settings_overlay.c/settings.c/config.c (see task #20/#34): a single
 * descriptor table (SettingDesc[]) drives a generic renderer/input
 * handler instead of hand-coding a screen per toggle, and the menu is a
 * pure host-side SDL overlay drawn after the game's own frame is already
 * composited -- it never touches SNES VRAM/PPU state directly (only the
 * settings' own target fields, which the game already reads every frame
 * regardless of whether this menu exists). While open, run_one_frame() is
 * skipped and the last rendered game frame is simply re-presented every
 * host iteration, the same freeze-and-redraw approach ar-recomp's own
 * overlay uses.
 *
 * ar-recomp's own overlay decodes the ROM's actual dialog font/frame
 * graphics for an in-theme look -- skipped here as purely cosmetic
 * ActRaiser-specific work (not something SimCity's ROM has an equivalent
 * of anyway). This uses a small hand-authored 3x5 bitmap font instead,
 * the same kind of fallback ar-recomp itself falls back to when ROM font
 * decoding isn't available. It only covers the character set this menu's
 * own labels currently use -- add glyphs to kFont as new labels need
 * them. Not yet ported from ar-recomp's design: the Int/Enum setting
 * kinds (nothing here needs a ranged/enumerated value yet), the
 * apply-kind taxonomy (every setting here is effectively PASSIVE --  the
 * game already polls these fields itself every frame), and settings.ini
 * persistence (all of these already persist their own way, e.g. save
 * states, or are meant to be session-only toggles like the cheats). */

typedef enum { kSettingBool, kSettingBit, kSettingAction, kSettingCycle } SettingKind;

typedef struct {
  const char *label;
  SettingKind kind;
  void *field;           /* bool* (Bool), uint8_t* (Bit), int* (Cycle) */
  uint8_t mask;           /* kSettingBit only */
  void (*action)(void);   /* kSettingAction only */
  const int *values;      /* kSettingCycle only: allowed values, cycled in order */
  int value_count;
} SettingDesc;

static bool setting_get(const SettingDesc *d) {
  switch (d->kind) {
    case kSettingBool: return *(bool *)d->field;
    case kSettingBit:  return (*(uint8_t *)d->field & d->mask) != 0;
    default: return false;
  }
}

static void setting_activate(SettingDesc *d) {
  switch (d->kind) {
    case kSettingBool: *(bool *)d->field = !*(bool *)d->field; break;
    case kSettingBit:  *(uint8_t *)d->field ^= d->mask; break;
    case kSettingAction: if (d->action) d->action(); break;
    case kSettingCycle: {
      int *v = (int *)d->field;
      int i = 0;
      for (; i < d->value_count; i++) if (d->values[i] == *v) break;
      *v = d->values[(i + 1) % d->value_count]; /* not-found wraps to values[1] */
      break;
    }
  }
}

static void menu_action_save_slot1(void) {
  if (save_state("savestate_1.bin"))
    fprintf(stderr, "[menu] saved slot 1 -> savestate_1.bin at frame %llu\n",
            (unsigned long long)s_frames);
  else
    fprintf(stderr, "[menu] failed to save slot 1\n");
}

static void menu_action_load_slot1(void) {
  if (load_state("savestate_1.bin"))
    fprintf(stderr, "[menu] loaded slot 1 <- savestate_1.bin, now at frame %llu\n",
            (unsigned long long)s_frames);
  else
    fprintf(stderr, "[menu] failed to load slot 1 (not saved yet?)\n");
}

/* This table is the whole "extension" mechanism, mirroring ar-recomp's own
 * randomizer/HD-replacements pattern: each row is one self-contained
 * feature plugged in via a single field pointer or action callback, with
 * no separate plugin/registration system needed. Adding a new toggle or
 * action means adding one row here -- render_settings_menu() below never
 * needs to change. */
static SettingDesc s_settings[] = {
  /* Labels are kept short enough that the longest one plus its ON/OFF
   * value still fits the menu box at the current font size -- see
   * render_settings_menu()'s width math. */
  { "MOUSE CURSOR",          kSettingBool, &s_mouse_enabled,       0,    NULL, NULL, 0 },
  { "FAST CURSOR",           kSettingBool, &s_fast_cursor_enabled, 0,    NULL, NULL, 0 },
  { "CURSOR SPEED",          kSettingCycle, &s_fast_cursor_step,   0,    NULL,
    kFastCursorSteps, (int)(sizeof(kFastCursorSteps) / sizeof(kFastCursorSteps[0])) },
  { "SCENARIO OVR",          kSettingCycle, &s_scenario_override,  0,    NULL,
    kScenarioOverrides, (int)(sizeof(kScenarioOverrides) / sizeof(kScenarioOverrides[0])) },
  { "UNLOCK SCENARIOS",      kSettingBool, &s_unlock_all,          0,    NULL, NULL, 0 },
  { "AUTO TURBO",            kSettingBool, &s_auto_turbo_enabled,  0,    NULL, NULL, 0 },
  { "CHEAT NO DISASTER",     kSettingBit,  &g_ram[0x0425],         0x01, NULL, NULL, 0 },
  { "CHEAT MONEY",           kSettingBit,  &g_ram[0x0425],         0x02, NULL, NULL, 0 },
  { "CHEAT VALVE MAX",       kSettingBit,  &g_ram[0x0425],         0x04, NULL, NULL, 0 },
  { "CHEAT WATER",           kSettingBit,  &g_ram[0x0425],         0x08, NULL, NULL, 0 },
  { "SET POP",               kSettingCycle, &s_pop_override,        0,    NULL,
    kPopOverrides, (int)(sizeof(kPopOverrides) / sizeof(kPopOverrides[0])) },
  { "SET CLASS",             kSettingCycle, &s_class_override,      0,    NULL,
    kClassOverrides, (int)(sizeof(kClassOverrides) / sizeof(kClassOverrides[0])) },
  { "CLR MILESTONE",         kSettingAction, NULL, 0, menu_action_clear_milestones, NULL, 0 },
  { "SAVE STATE 1",          kSettingAction, NULL, 0, menu_action_save_slot1, NULL, 0 },
  { "LOAD STATE 1",          kSettingAction, NULL, 0, menu_action_load_slot1, NULL, 0 },
};
#define kSettingCount (sizeof(s_settings) / sizeof(s_settings[0]))

static bool s_menu_open;
static int s_menu_selected;

/* SC_MENU_PREVIEW=1: force the settings menu open from frame 1, let a
 * handful of iterations render (so the window/renderer is definitely
 * live), dump the composited output to menu_preview.ppm via
 * write_renderer_ppm(), then exit -- headless visual verification of the
 * overlay UI without needing to click into the actual window. */
static bool s_menu_preview;
static int s_menu_preview_countdown = 5;

/* 5x5 bitmap font, one row per byte (bit4=leftmost col .. bit0=rightmost).
 * Coarse but complete for A-Z/0-9, so a label can't silently render a
 * blank for a glyph nobody added yet.
 *
 * The width went 3 -> 4 -> 5 over three rounds of SC_MENU_PREVIEW
 * screenshot review, each time because letters with interior diagonal
 * strokes were unreadable at the narrower size and *only* the rendered
 * image showed it: at 3 wide 'N' read as an hourglass, and at 4 wide both
 * 'M' and 'W' collapsed into something indistinguishable from 'H' (so
 * "MOUSE" read as "HOUSE" and "MONEY" as "HONEY"). 5 is the first width
 * where M/N/W each get a real interior stroke with a blank column on
 * either side. Don't narrow this again without re-checking the preview. */
typedef struct { char ch; uint8_t rows[5]; } FontGlyph;
static const FontGlyph kFont[] = {
  {' ', {0,0,0,0,0}},
  {'0', {14,17,17,17,14}}, {'1', {4,12,4,4,14}},   {'2', {14,17,2,4,31}},
  {'3', {30,1,14,1,30}},   {'4', {17,17,31,1,1}},  {'5', {31,16,30,1,30}},
  {'6', {14,16,30,17,14}}, {'7', {31,1,2,4,8}},    {'8', {14,17,14,17,14}},
  {'9', {14,17,15,1,14}},
  {'A', {14,17,31,17,17}}, {'B', {30,17,30,17,30}}, {'C', {15,16,16,16,15}},
  {'D', {30,17,17,17,30}}, {'E', {31,16,30,16,31}}, {'F', {31,16,30,16,16}},
  {'G', {15,16,19,17,15}}, {'H', {17,17,31,17,17}}, {'I', {31,4,4,4,31}},
  {'J', {7,2,2,18,12}},    {'K', {17,18,28,18,17}}, {'L', {16,16,16,16,31}},
  {'M', {17,27,21,17,17}}, {'N', {17,25,21,19,17}}, {'O', {14,17,17,17,14}},
  {'P', {30,17,30,16,16}}, {'Q', {14,17,21,18,13}}, {'R', {30,17,30,18,17}},
  {'S', {15,16,14,1,30}},  {'T', {31,4,4,4,4}},     {'U', {17,17,17,17,14}},
  {'V', {17,17,17,10,4}},  {'W', {17,17,21,27,17}}, {'X', {17,10,4,10,17}},
  {'Y', {17,10,4,4,4}},    {'Z', {31,2,4,8,31}},
};
#define kFontCount (sizeof(kFont) / sizeof(kFont[0]))

static const uint8_t *font_glyph_rows(char c) {
  for (size_t i = 0; i < kFontCount; i++)
    if (kFont[i].ch == c) return kFont[i].rows;
  return kFont[0].rows; /* unknown char -> blank */
}

/* Draws text at (x,y) in real renderer pixels, each font pixel drawn as a
 * `px`x`px` filled square. Uppercases input so call sites can write labels
 * in whatever case is convenient. */
static void draw_text(SDL_Renderer *renderer, int x, int y, int px, const char *s) {
  int cx = x;
  for (const char *p = s; *p; p++) {
    char c = (char)toupper((unsigned char)*p);
    const uint8_t *rows = font_glyph_rows(c);
    for (int row = 0; row < 5; row++)
      for (int col = 0; col < 5; col++)
        if (rows[row] & (1 << (4 - col))) {
          SDL_Rect r = { cx + col * px, y + row * px, px, px };
          SDL_RenderFillRect(renderer, &r);
        }
    cx += 6 * px; /* 5 cols of glyph + 1 col of spacing */
  }
}

static int text_width(int px, const char *s) {
  int n = (int)strlen(s);
  return n > 0 ? n * 6 * px - px : 0;
}

static void render_settings_menu(SDL_Renderer *renderer) {
  int out_w = 0, out_h = 0;
  SDL_GetRendererOutputSize(renderer, &out_w, &out_h);

  const int px = 4;            /* font pixel size, in real screen pixels */
  const int line_h = 6 * px;   /* glyph height (5) + 1 row of spacing */
  const int pad = 3 * px;
  int menu_w = out_w * 3 / 4;
  int menu_h = pad * 2 + line_h * ((int)kSettingCount + 6 + (s_menu_preview ? 4 : 0));
  int menu_x = (out_w - menu_w) / 2;
  int menu_y = (out_h - menu_h) / 2;

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 200);
  SDL_Rect bg = { menu_x, menu_y, menu_w, menu_h };
  SDL_RenderFillRect(renderer, &bg);
  SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
  SDL_RenderDrawRect(renderer, &bg);

  int ty = menu_y + pad;
  draw_text(renderer, menu_x + pad, ty, px, "SETTINGS");
  ty += line_h * 2;

  for (size_t i = 0; i < kSettingCount; i++) {
    SettingDesc *d = &s_settings[i];
    bool selected = ((int)i == s_menu_selected);
    SDL_SetRenderDrawColor(renderer, 255, selected ? 255 : 255, selected ? 0 : 255, 255);
    draw_text(renderer, menu_x + pad, ty, px, d->label);
    if (d->kind != kSettingAction) {
      char numbuf[16];
      const char *val;
      if (d->kind == kSettingCycle) {
        int cv = *(int *)d->field;
        if (cv < 0) {
          val = "OFF"; /* negative sentinel, so 0 stays a real selectable value */
        } else {
          snprintf(numbuf, sizeof(numbuf), "%d", cv);
          val = numbuf;
        }
      } else {
        val = setting_get(d) ? "ON" : "OFF";
      }
      int label_w = text_width(px, d->label);
      draw_text(renderer, menu_x + pad + label_w + 8 * px, ty, px, val);
    }
    ty += line_h;
  }

  ty += line_h / 2;
  SDL_SetRenderDrawColor(renderer, 180, 180, 180, 255);
  draw_text(renderer, menu_x + pad, ty, px - 1, "UP DOWN SELECT");
  ty += line_h - px;
  draw_text(renderer, menu_x + pad, ty, px - 1, "ENTER TOGGLE");
  ty += line_h - px;
  draw_text(renderer, menu_x + pad, ty, px - 1, "F10 CLOSE");

  /* Under SC_MENU_PREVIEW only: render the full glyph set so a single
   * preview screenshot verifies every character, not just the ones the
   * current labels happen to use. Two missing glyphs ('P', then 'B')
   * already shipped as blanks precisely because nothing exercised them
   * until a label needed them. */
  if (s_menu_preview) {
    ty += line_h;
    draw_text(renderer, menu_x + pad, ty, px - 1, "ABCDEFGHIJKLM");
    ty += line_h - px;
    draw_text(renderer, menu_x + pad, ty, px - 1, "NOPQRSTUVWXYZ");
    ty += line_h - px;
    draw_text(renderer, menu_x + pad, ty, px - 1, "0123456789");
  }

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
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

/* Distinct-PC histogram for SC_MAP_WRITE_TRACE, emitted at exit alongside the
 * PC bitmap so both windowed and --qualify runs report it. */
static void write_map_trace_summary(void) {
  if (!s_map_write_trace) return;
  fprintf(stderr, "[mapwrite] %u total writes into $%06x..$%06x from %d distinct PCs\n",
          s_map_write_hits, kMapBufStart, kMapBufEnd - 1, s_map_write_pc_count);
  for (int i = 0; i < s_map_write_pc_count; i++)
    fprintf(stderr, "[mapwrite]   %02x:%04x  %u writes\n",
            (unsigned)(s_map_write_pcs[i].pc >> 16) & 0xff,
            (unsigned)(s_map_write_pcs[i].pc & 0xffff), s_map_write_pcs[i].count);
}

static int run_qualification(uint64_t frames) {
  uint64_t logic_changes = 0, video_changes = 0, audio_active_frames = 0;
  uint64_t last_ram_hash = 0, last_video_hash = 0;
  uint32_t last_sample_write = 0;
  int16_t audio_buf[1024 * 2];

  uint64_t stall_run = 0, stall_max = 0;
  for (uint64_t f = 0; f < frames; f++) {
    apply_frame_input(f);
    apply_freezes();
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
  write_map_trace_summary();
  { const char *p = getenv("SC_SRAM_DUMP_PATH");
    if (p && *p) {
      report_sram_header("exit");
      fprintf(stderr, write_sram_dump(p) ? "dumped SRAM to %s\n"
                                         : "failed to write SRAM dump to %s\n", p);
    } }
  return rc;
}

int main(int argc, char **argv) {
  const char *rom_path = "simcity.sfc";
  const char *load_state_path = NULL;
  uint64_t qualify_frames = 0;
  int scale = 3;
  { const char *e = getenv("SC_IO_TRACE"); if (e && *e) s_io_trace_until = strtoull(e, NULL, 0); }
  { const char *e = getenv("SC_PC_TRACE");
    if (e && *e) { s_pc_trace_at_frame = strtoull(e, NULL, 0); s_pc_capture_after = -2; } }
  { const char *e = getenv("SC_ADDR_TRACE"); if (e && *e) parse_addr_trace(e); }
  { const char *e = getenv("SC_GFX_TRACE"); if (e && *e) s_gfx_trace = true; }
  { const char *e = getenv("SC_DECOMP_TRACE"); if (e && *e) s_decomp_trace = true; }
  { const char *e = getenv("SC_UNLOCK_ALL"); if (e && *e) s_unlock_all = true; }
  { const char *e = getenv("SC_MAP_WRITE_TRACE"); if (e && *e) s_map_write_trace = true; }
  { const char *e = getenv("SC_VIEW_WATCH"); if (e && *e) s_view_watch = true; }
  { const char *e = getenv("SC_MENU_PREVIEW");
    if (e && *e) { s_menu_preview = true; s_menu_open = true; } }
  { const char *e = getenv("SC_FREEZE"); if (e && *e) parse_freezes(e); }
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
    } else if (!strcmp(argv[i], "--load-state") && i + 1 < argc) {
      load_state_path = argv[++i];
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

  /* REMOVED: the "D-pad bug family" ROM patches (11 sites, ~470 lines).
   *
   * They never should have existed. The runner returned the two halves of
   * the auto-joypad read transposed -- snes.c's $4218 case returned the
   * high byte and $4219 the low one. Per hardware the joypad word is:
   *     bits 15-8 : B, Y, Select, Start, Up, Down, Left, Right
   *     bits  7-0 : A, X, L, R, 0, 0, 0, 0     (low nibble = controller ID)
   * so $4218 carries A/X/L/R plus four always-zero bits and $4219 carries
   * the D-pad. The transposition put the D-pad in $011b and left $011c --
   * the byte the game actually tests -- reading zero.
   *
   * `LDA $011b (16-bit) / AND #$0f00` is simply how you read the D-pad on
   * this hardware: it tests $011c bits 0-3. It looked like a per-site bug
   * only because the emulator was feeding it the wrong byte. Every one of
   * the eleven patches here was compensating for that single defect, and
   * once snes.c was corrected the two compensations cancelled: with both
   * applied the input went dead again.
   *
   * Measured directly. On the bank-loan dialog (the cleanest discriminator,
   * since its handler depends on exactly one patched site) holding Right,
   * with the runner fixed:
   *     patches applied  -> $0b17 stays 0   (cursor stuck)
   *     patches removed  -> $0b17 goes 0->1 (cursor moves, stock ROM code)
   * And holding Left now yields $011b=00 / $011c=02 -- Left in $011c bit 1,
   * exactly as the hardware layout specifies.
   *
   * Credit for the root cause goes to the parallel Metal Marines work,
   * which hit the identical pattern and correctly refused to believe two
   * unrelated commercial games shipped the same input bug. That was the
   * tell here too and it was missed: eleven independent sites "sharing a
   * bug" is an anomaly to explain, not corroboration to lean on.
   *
   * The two ROM patches kept below are unrelated to input and still stand
   * on their own evidence: the $01f3 cursor step-delay tweak and the
   * 00:c0fb $7e21b5 stomp fix.
   *
   * Anything in docs/ that was reasoned from "$011b holds the D-pad" needs
   * re-deriving against the corrected runner. */

  /* Cursor step-cadence speed tweak (docs/INVESTIGATION_cursor_cadence.md)
   * -- NOT part of the D-pad bug family above, and unlike those, not
   * confirmed to differ from real hardware. This is a deliberate design
   * constant, not a "wrong nibble"-style bug: found via the same
   * deterministic --load-state/--input testing that cracked fast travel,
   * tracing the real cursor-position writer (`01:c2b1: STA $01eb`, not the
   * stale `01:c214` the old investigation notes guessed) back to a
   * countdown-delay gate at `01:c0dd`: `LDA $01f3 (16-bit); BEQ +4 (fall
   * through if zero); DEC $01f3; RTS (bail if nonzero)`. After a
   * successful step, two mirror-image sites (the increment and decrement
   * direction handlers) both reset it with the identical `LDA #$0003;
   * STA $01f3` -- confirmed live: 15/35 calls bail on a nonzero `$01f3`,
   * the other 20/35 fall through and write `$01eb`, exactly matching this
   * gate. Same constant in both symmetric sites reads as intentional
   * pacing, not a bug -- lowering it here is a speed tweak, matching the
   * spirit of the existing fast-forward feature and the "for a future
   * speed mod" framing the old investigation doc already used, not a
   * claim that real hardware behaves differently. */
  {
    static const uint32_t kCursorDelaySites[] = {
      0xc2e0, /* 01:c2df's immediate operand low byte -- increment handler */
      0xc3c9, /* 01:c3c8's immediate operand low byte -- decrement handler */
    };
    int patched = 0;
    for (size_t i = 0; i < sizeof(kCursorDelaySites) / sizeof(kCursorDelaySites[0]); i++) {
      uint32_t off = kCursorDelaySites[i];
      if (off < rom_size && rom_data[off] == 0x03) {
        rom_data[off] = 0x00;
        patched++;
      }
    }
    fprintf(stderr, "cursor cadence: patched %d/%d step-delay reset sites ($01f3: 3 -> 0)\n",
            patched, (int)(sizeof(kCursorDelaySites) / sizeof(kCursorDelaySites[0])));
  }

  /* Cursor cadence, part 2 -- INVESTIGATED, NOT APPLIED. The $01f3 delay
   * above turned out to be only the first of two gates in series. $01f3
   * ==0 unlocks a second flag, $01ff: a shared "step pending" lock across
   * both axes, set to a per-direction bitmask (e.g. 0x0800 for Up, 0x0100
   * for Right) by the direction handlers (01:c145 etc.) right after every
   * step, cleared only by the shared post-step tail at 01:c2d4
   * (`AND #$0007; BEQ` -- only unlocks once the cursor's new clamped
   * position is a multiple of 8, i.e. roughly 1-in-4 calls, since each
   * step moves 2 units). Tried the same fix idea as $01f3 (widen the
   * AND mask so it always unlocks) and confirmed live it's a *regression*:
   * $01ff isn't purely wasted time -- while it's set, a second ladder at
   * 01:c195 bypasses it entirely by re-testing whichever direction bit is
   * still set in $01ff and re-issuing that same step directly, which is
   * *also* real, useful step throughput (measured: holding Right alone,
   * removing the lock dropped the total step rate from 48/100 frames down
   * to 25/100 -- the bypass path stops firing once $01ff no longer holds
   * a pending direction to re-issue, and the primary path alone doesn't
   * make up the difference). Left unpatched; the two gates interact in a
   * way that isn't a simple "remove the delay" fix like $01f3 was. */

  /* View screen D-pad fix (docs/INVESTIGATION_dpad.md "View screen's
   * D-pad: FIXED"): the write side ($7e21b4 for Left/Right, $7e21b5 for
   * Up/Down, both driven by 01:f189/f190 and 01:f19a/f1bf) was already
   * confirmed working in an earlier session, but nothing visibly moved.
   * Root cause, found via deterministic --load-state/--input testing plus
   * a live memory watch (SC_VIEW_WATCH=1) that catches every addressing
   * mode -- unlike a static opcode scan, which only turned up $7e21b5's
   * own read-modify-write increment in unrelated bank 5 code, not a
   * genuine consumer: $7e21b4 (Left/Right) already works correctly
   * end-to-end (confirmed live: cleanly decrements frame over frame while
   * held, e.g. 7d->7a->77->74->...). $7e21b5 (Up/Down) does not: a
   * universal, always-on per-frame routine at 00:c0fb (`SEP #$20;
   * LDA #$e0; STA $7e21b5`, part of a loop at 00:8aa8 that rebuilds a
   * whole row of UI icon sprites into OAM via DMA every frame, on every
   * screen -- not View-specific, confirmed also firing on the classic
   * map) unconditionally resets $7e21b5 to a fixed $e0 every single
   * frame, stomping whatever 01:f1bf just wrote before anything can read
   * the new value. Confirmed live holding Down: 01:f1bf computes a real
   * new value (e.g. $dc), but 01:f1a7 (the only reader of $7e21b5
   * anywhere in the ROM -- verified via the same live watch, on every
   * screen, not just View) only ever observes the reset value $e0, never
   * the update.
   *
   * Fix: NOP out just this one STA (4 bytes, EA EA EA EA), leaving its
   * six sibling table-slot writes ($7e21b9/bd/c1/d5/d9/dd, part of the
   * same per-frame icon rebuild) completely untouched -- as surgical as a
   * byte patch gets. Safe because $7e21b5 has exactly one consumer in the
   * entire ROM (the View screen's own Up/Down check); nothing else reads
   * it, on any screen, so skipping this one reset can't leave stale data
   * visible anywhere else. (A first attempt at this looked like it hung
   * the full 10800-frame --qualify baseline -- turned out to be an
   * unrelated false alarm from heavy host system load that session, not
   * this patch: a 90s-timeout retest completed in 40s with byte-identical
   * baseline output. Re-verify with a generous timeout if this is ever
   * in doubt again.) */
  {
    uint32_t off = 0x40fb; /* 00:c0fb's STA $7e21b5 (long), file offset = addr-0x8000 (bank 0) */
    if (off + 3 < rom_size && rom_data[off] == 0x8f && rom_data[off+1] == 0xb5 &&
        rom_data[off+2] == 0x21 && rom_data[off+3] == 0x7e) {
      rom_data[off] = rom_data[off+1] = rom_data[off+2] = rom_data[off+3] = 0xea; /* NOP x4 */
      fprintf(stderr, "view fix: patched 00:c0fb STA $7e21b5 -> NOP (stop stomping the Up/Down View cursor)\n");
    } else {
      fprintf(stderr, "view fix: 00:c0fb site NOT patched (byte mismatch)\n");
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

  /* --load-state <path>: overwrite the just-reset boot state with a
   * previously captured save state (see save_state/load_state and the
   * numbered-slot hotkeys below) -- lets a --qualify run, or the windowed
   * host, start already positioned on a specific screen/scenario instead
   * of from cold boot, so a fixed --input sequence can be replayed against
   * it deterministically. Applied after reset/loadRom so it fully
   * supersedes them rather than racing anything. */
  if (load_state_path) {
    if (!load_state(load_state_path)) {
      fprintf(stderr, "cannot load state '%s'\n", load_state_path);
      return 1;
    }
    fprintf(stderr, "loaded state '%s', now at frame %llu\n",
            load_state_path, (unsigned long long)s_frames);
  }

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
      /* F9: toggle the fast D-pad cursor (see apply_mouse_delta/
       * s_fast_cursor_enabled above). Off by default -- same "opt-in,
       * not authentic ROM behavior" reasoning as F3. */
      if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_F9 && !ev.key.repeat) {
        s_fast_cursor_enabled = !s_fast_cursor_enabled;
        fprintf(stderr, "[F9] fast D-pad cursor %s\n", s_fast_cursor_enabled ? "ON" : "OFF");
      }
      /* F10: settings menu (see the "minimal in-game settings menu" block
       * above) -- toggles a host-side overlay listing this project's
       * existing toggles/actions in one generic, table-driven list instead
       * of each needing its own memorized hotkey. */
      if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_F10 && !ev.key.repeat) {
        s_menu_open = !s_menu_open;
        fprintf(stderr, "[F10] settings menu %s\n", s_menu_open ? "OPEN" : "CLOSED");
      }
      if (s_menu_open && ev.type == SDL_KEYDOWN) {
        switch (ev.key.keysym.scancode) {
          case SDL_SCANCODE_UP:
            s_menu_selected = (s_menu_selected - 1 + (int)kSettingCount) % (int)kSettingCount;
            break;
          case SDL_SCANCODE_DOWN:
            s_menu_selected = (s_menu_selected + 1) % (int)kSettingCount;
            break;
          case SDL_SCANCODE_RETURN:
          case SDL_SCANCODE_LEFT:
          case SDL_SCANCODE_RIGHT:
            setting_activate(&s_settings[s_menu_selected]);
            break;
          default: break;
        }
      }
      /* F4: dump WRAM to a fixed path right now, on demand -- for pinning
       * down exact WRAM byte values at a precise live moment (e.g. hold a
       * button combo, press F4, inspect $7e011b/$7e011c directly) instead
       * of inferring values from instruction traces. */
      if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_F4 && !ev.key.repeat) {
        const char *path = "wram_snapshot.bin";
        if (write_wram_dump(path))
          fprintf(stderr, "[F4] dumped WRAM to %s at frame %llu\n", path, (unsigned long long)s_frames);
        else
          fprintf(stderr, "[F4] failed to write WRAM dump to %s\n", path);
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
      /* Numbered save-state slots: Shift+1..Shift+0 saves slot 1-9/0,
       * plain 1..0 loads it -- 10 slots so a specific scenario (e.g. "on
       * the map screen, cursor visible, nothing held") can be captured
       * once interactively and then reloaded instantly and deterministically
       * for repeated testing, instead of re-navigating menus (or guessing
       * --input timing) every run. SNES Select is bound to B (not Shift)
       * below specifically so holding Shift for a save/load never also
       * feeds a Select press into the game at that exact moment -- a save
       * state should capture "nothing else held," not "Select held". */
      if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
        static const struct { SDL_Scancode sc; char digit; } kSlotKeys[] = {
          { SDL_SCANCODE_1, '1' }, { SDL_SCANCODE_2, '2' }, { SDL_SCANCODE_3, '3' },
          { SDL_SCANCODE_4, '4' }, { SDL_SCANCODE_5, '5' }, { SDL_SCANCODE_6, '6' },
          { SDL_SCANCODE_7, '7' }, { SDL_SCANCODE_8, '8' }, { SDL_SCANCODE_9, '9' },
          { SDL_SCANCODE_0, '0' },
        };
        for (size_t i = 0; i < sizeof(kSlotKeys) / sizeof(kSlotKeys[0]); i++) {
          if (ev.key.keysym.scancode != kSlotKeys[i].sc) continue;
          char path[32];
          snprintf(path, sizeof(path), "savestate_%c.bin", kSlotKeys[i].digit);
          if (ev.key.keysym.mod & KMOD_SHIFT) {
            if (save_state(path))
              fprintf(stderr, "[state] saved slot %c -> %s at frame %llu\n",
                      kSlotKeys[i].digit, path, (unsigned long long)s_frames);
            else
              fprintf(stderr, "[state] failed to save slot %c -> %s\n", kSlotKeys[i].digit, path);
          } else {
            if (load_state(path))
              fprintf(stderr, "[state] loaded slot %c <- %s, now at frame %llu\n",
                      kSlotKeys[i].digit, path, (unsigned long long)s_frames);
            else
              fprintf(stderr, "[state] failed to load slot %c <- %s (not saved yet?)\n",
                      kSlotKeys[i].digit, path);
          }
          break;
        }
      }
    }
    const uint8_t *keys = SDL_GetKeyboardState(NULL);
    if (s_mouse_enabled) {
      int mdx = 0, mdy = 0;
      SDL_GetRelativeMouseState(&mdx, &mdy);
      if (mdx || mdy) apply_mouse_delta(mdx, mdy);
    }
    if (s_fast_cursor_enabled) {
      /* Host-driven, independent of the ROM's own cadence -- see
       * s_fast_cursor_enabled's comment above. Diagonal holds add both
       * axes, same as the ROM's own D-pad would. */
      int fdx = 0, fdy = 0;
      if (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_H]) fdx -= s_fast_cursor_step;
      if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_K]) fdx += s_fast_cursor_step;
      if (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_U]) fdy -= s_fast_cursor_step;
      if (keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_J]) fdy += s_fast_cursor_step;
      if (fdx || fdy) apply_mouse_delta(fdx, fdy);
    }
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
    /* Select is bound to B (not Shift) so Shift is free for the
     * save-state slot hotkeys (Shift+1..Shift+0) without also feeding a
     * Select press into the game every time a state is saved/loaded. */
    if (keys[SDL_SCANCODE_B]) input |= kPad_Select;
    /* Left mouse button = SNES X -- lets host-mouse cursor control (F3)
     * actually select/interact with things, not just move the cursor.
     * Not gated on s_mouse_enabled: useful as a plain extra binding
     * regardless (one hand on the mouse for pointing, click to act,
     * without reaching for the keyboard). Was bound to B originally; the
     * user asked for X after playing with it. */
    if (SDL_GetMouseState(NULL, NULL) & SDL_BUTTON(SDL_BUTTON_LEFT)) input |= kPad_X;
    /* Don't feed the keyboard to the game while the settings menu is open:
     * the menu navigates with Up/Down/Left/Right/Enter, which are also the
     * SNES D-pad and Start bindings. The game is frozen so nothing acts on
     * them immediately, but whatever is held on the frame the menu closes
     * would otherwise leak straight through as a real button press. */
    if (s_menu_open) input = 0;
    apply_frame_input(s_frames);
    apply_freezes();
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

    /* While the settings menu is open, freeze the game -- skip advancing
     * the emulator entirely and just keep re-presenting the last rendered
     * frame every host iteration, same freeze-and-redraw approach ar-recomp's
     * own settings overlay uses (see the menu block above). s_video_pixels
     * (and therefore `texture` below) simply isn't touched this iteration,
     * so whatever was last rendered stays on screen underneath the overlay. */
    bool guard_tripped = false;
    if (!s_menu_open) {
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
      /* Don't accrue playback debt for wall-clock time the emulator wasn't
       * actually running: while the settings menu is open no frames are
       * simulated, so no samples are produced and there is nothing to pace
       * against. (This is the immediate half of the fix below.) */
      if (!s_menu_open) audio_acc += (double)have.freq / 60.0988;
      /* Hard-clamp the accumulator to exactly the drain condition's upper
       * bound, which is also audio_buf's capacity. Without this, ANY stall
       * of two or more host iterations where the ring hasn't refilled --
       * menu open, a slow frame, an ordinary underrun -- pushes audio_acc
       * past 1024 and the `wantN <= 1024` test below then fails forever,
       * since audio_acc is only ever decremented *inside* that branch.
       * That's a self-latching permanent-silence trap: two bad frames and
       * sound never returns for the rest of the session, with no error and
       * no recovery path. Clamping converts it into what an underrun
       * should be -- a brief dropout that self-corrects on the next frame.
       * (Found via the F10 menu, which reproduced it every single time by
       * construction; it is a pre-existing bug the menu merely made
       * trivial to hit, and a strong candidate for the long-standing
       * intermittent audio complaints tracked separately.) */
      if (audio_acc > 1024.0) audio_acc = 1024.0;
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
    if (s_menu_open) render_settings_menu(renderer);

    if (s_menu_preview && --s_menu_preview_countdown <= 0) {
      if (write_renderer_ppm(renderer, "menu_preview.ppm"))
        fprintf(stderr, "[SC_MENU_PREVIEW] dumped menu_preview.ppm\n");
      else
        fprintf(stderr, "[SC_MENU_PREVIEW] failed to dump menu_preview.ppm\n");
      quit = true;
    }

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
  write_map_trace_summary();
  return 0;
}
