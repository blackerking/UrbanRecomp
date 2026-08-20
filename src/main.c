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

/* Shared SDL2/SDL3 include boundary. SNESRECOMP_SDL3 is set by
 * snesrecomp_target_sdl() in CMakeLists.txt; the shim pulls in the right
 * SDL and turns on the transitional old-name aliases so constants and types
 * keep their SDL2 spellings. Calls whose SIGNATURES changed are handled
 * explicitly at their call sites -- see runner/src/desktop/mmx23_host_main.inc
 * for how upstream does each one. */
#include "sc_sdl_compat.h"
/* SDL3 switched the renderer rect APIs from SDL_Rect (int) to SDL_FRect
 * (float). SDL_ENABLE_OLD_NAMES preserves the NAMES but not the signatures,
 * so passing an SDL_Rect* to SDL_RenderFillRect under SDL3 reinterprets four
 * ints as two floats: the rect lands somewhere meaningless and the overlay
 * silently does not appear. That is what hid the F10 settings menu -- it was
 * toggling (the OPEN/CLOSED log proves it) and drawing off-screen. */
#if SNESRECOMP_SDL3
typedef SDL_FRect ScRect;
#define SC_RECT(x, y, w, h) ((ScRect){ (float)(x), (float)(y), (float)(w), (float)(h) })
#else
typedef SDL_Rect ScRect;
#define SC_RECT(x, y, w, h) ((ScRect){ (int)(x), (int)(y), (int)(w), (int)(h) })
#endif

/* SDL3 returns true on success where SDL2 returned 0. */
#if SNESRECOMP_SDL3
#define SC_SDL_OK
#else
#define SC_SDL_OK == 0
#endif

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
/* SIMCITY_AOT_TIER: built as part of the AOT/CpuState migration (see
 * src/aot_probe.c and the SimCitySNESRecompAOT target). In that build the
 * shared runtime is linked in, and it already defines several of the symbols
 * this file provides for the standalone interpreter build. They are the same
 * objects with the same meaning -- common_rtl.c's g_ram is a 0x20000 array
 * with identical $7E/$7F semantics, and this host passes g_ram straight to
 * snes_init(), so both tiers end up sharing one WRAM array rather than
 * needing any copying between them. Defining them here too would just be a
 * duplicate symbol, so the AOT build defers to the runtime's copies. */
#ifdef SIMCITY_AOT_TIER
#include "common_rtl.h"
#include "common_rtl.h"          /* extern uint8 g_ram[0x20000]; */
#include "simcity_fiberdrive.h"
#else
uint8_t    g_ram[0x20000];
#endif
/* OUTSIDE the AOT guard, deliberately. Placed inside it, the plain build never
 * saw the prototype, implicitly declared ScMapView_Render as returning `int`,
 * and read all of EAX where the callee had only set AL -- so a function that
 * returned false was observed as true, and the US-only gate silently passed on
 * a German ROM. It compiled and linked without a word. */
#include "simcity_mapview.h"
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
#ifndef SIMCITY_AOT_TIER
/* All three are provided by the shared runtime in the AOT build:
 * g_interp_apu_driving and ppudma_record_dma by common_rtl.c /
 * ppu_dma_trace.c, interp816_opcode_hook by interp_bridge.c. */
int g_interp_apu_driving = 0;
void ppudma_record_dma(int ch, int fromB, uint8_t aBank, uint16_t aAdr,
                       uint8_t bAdr, uint16_t size) {
  (void)ch; (void)fromB; (void)aBank; (void)aAdr; (void)bAdr; (void)size;
}
int interp816_opcode_hook(uint32_t addr) { (void)addr; return 0; }
#else
/* Conversely, the runtime expects the GAME to supply these. The desktop
 * hosts define them in their host_main include; this host defines them here.
 * The APU locks are real work once the AOT tier drives audio -- for now this
 * host still owns the DSP drain loop single-threaded, so no-ops are correct
 * and must be revisited when that changes. */
/* Only g_spc_player is genuinely missing: common_rtl.h and debug_server.h
 * already carry inline bodies for the APU locks and the debug write hooks,
 * so defining those here is a redefinition, not a fill-in. (The aot_probe
 * target does need them, because it does not include those headers.) */
#include "spc_player.h"
SpcPlayer *g_spc_player = NULL;
/* debug_server.h declares but does not define this one (unlike the wram
 * write hooks); interp_bridge.c calls it per interpreted block. */
void debug_on_block_enter(uint32_t pc, uint32_t a, uint32_t x, uint32_t y)
  { (void)pc; (void)a; (void)x; (void)y; }
#endif
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
 * SC_AOT_VARIANTS=<file>   with SC_FIBER, list the compiled variants the run
 *                           ENTERED, as `pc24:MmXn hits=N` -- the same key
 *                           the program manifest uses, so the two join
 *                           directly. An entry is NOT an extent: the body
 *                           then runs an unknown number of opcodes without
 *                           reporting them, so this must never be expanded
 *                           into an executed-PC bitmap.
 * SC_WRAM_DUMP_PC=<pc24>   with SC_WRAM_DUMP_DIR, fire each periodic WRAM
 *                           dump when the guest reaches that PC instead of at
 *                           the host frame boundary. Two hosts park the guest
 *                           in different places, so a host-frame-aligned dump
 *                           compares different guest moments -- see
 *                           docs/MIGRATION_step3.md 23.1.
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
 * SC_WRAM_MAP=<file>        map WRAM usage over a session: per byte, whether
 *                           it was read/written, the LAST PC to write it, and
 *                           a saturating write count. The data counterpart to
 *                           SC_PC_BITMAP_BANK -- see docs/ROM_MAP.md "WRAM
 *                           usage map".
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

/* SC_MX_BITMAP=<path>: the same executed-PC bitmap, but split four ways by the
 * live (m,x) width flags. The point is the exit-M/X fixpoint that blocks the
 * remaining ~5% of AOT coverage (docs/OPEN_QUESTIONS.md A1): the analyzer
 * cannot prove what widths a callee returns in, but the machine knows -- the
 * widths observed at a call's *return address* are exactly the callee's exit
 * widths. Recording (pc, m, x) turns that from a proof obligation into a
 * measurement, which is the same move the execution bitmap already made for
 * "is this address code".
 *
 * Deliberately separate from s_pc_bitmap_all rather than replacing it: the
 * existing bitmaps are the basis of every coverage number in the README, and
 * silently changing their format would invalidate the ones already on disk. */
static uint8_t (*s_mx_bitmap)[64][4096];   /* [mx][bank][byte], mx = m<<1 | x */
static const char *s_mx_bitmap_path;
static uint64_t s_banks_seen; /* bit N set if bank N ever held cpu->k (diagnostic only) */

/* SC_WRAM_MAP=<file>: build a live map of WRAM usage over a play session --
 * which of the 128KB is read, which is written, and *which PC first wrote
 * each byte*.
 *
 * The PC-execution bitmap (SC_PC_BITMAP_BANK) answers "which routines run".
 * This is its counterpart for data: it answers "which variables exist, and
 * who owns them". docs/ROM_MAP.md's WRAM table was built one address at a
 * time from targeted investigations; this produces the whole picture in one
 * session, and the first-writer PC turns an anonymous address into a lead --
 * find the routine, and you have the variable's meaning.
 *
 * Records the LAST writer, not the first. Measured: the boot path writes all
 * 131072 bytes of WRAM (it clears the lot), so a first-writer map is entirely
 * owned by the clear loop and carries no signal whatsoever. The last writer
 * after a play session is the routine that actually maintains the byte.
 * A saturating write count comes along too, which separates hot per-frame
 * state from something touched once at init.
 *
 * Dumped at exit as a flat binary: 0x20000 flag bytes (bit0 read, bit1
 * written), then 0x20000 little-endian uint32 last-writer PCs (0xFFFFFFFF
 * where never written), then 0x20000 little-endian uint16 write counts.
 * Cheap enough to leave on for a whole session: a few array stores per bus
 * access. */
/* SC_WRAM_DUMP_PC=<pc24>: take the periodic WRAM dump at a GUEST-defined
 * moment rather than a host-defined one.
 *
 * Comparing two hosts at "the same frame" is confounded, because they park the
 * guest in different places. Measured at frame 600: the fiber host leaves the
 * guest at 00:9311 with S=$1FF5 -- its yield point, by construction -- while
 * the per-opcode host is at 00:8F1F with S=$1FEA. Eleven bytes of call depth
 * apart and in unrelated code, so stack residue and direct-page scratch differ
 * for reasons that have nothing to do with either host being wrong.
 *
 * Arming the dump on a PC makes both hosts sample the same guest moment. Fiber
 * mode ignores it and dumps as usual: it is already parked at 00:9311 when the
 * frame ends. */
static bool write_wram_dump(const char *path);
static uint32_t s_dump_pc24 = 0xffffffffu;
static bool     s_dump_pc_armed;
static uint64_t s_dump_pc_frame;
static char     s_dump_pc_dir[400];

static bool s_wram_map;
static uint8_t *s_wram_flags;      /* [0x20000] bit0 = read, bit1 = written */
static uint32_t *s_wram_last_pc;   /* [0x20000] last writer, (bank<<16)|pc */
static uint16_t *s_wram_wcount;    /* [0x20000] saturating write count */

/* WRAM offset for a 24-bit bus address, or -1 if it is not WRAM.
 * $7E/$7F are direct; banks $00-$3F and $80-$BF mirror $7E0000-$7E1FFF at
 * $0000-$1FFF, which is where nearly every variable this project has named
 * actually lives. */
static inline int wram_offset(uint32_t adr) {
  uint8_t bank = (uint8_t)(adr >> 16);
  uint16_t off = (uint16_t)adr;
  if (bank == 0x7e) return off;
  if (bank == 0x7f) return 0x10000 + off;
  if ((bank < 0x40 || (bank >= 0x80 && bank < 0xc0)) && off < 0x2000) return off;
  return -1;
}

static void wram_map_note(uint32_t adr, bool write) {
  int o = wram_offset(adr);
  if (o < 0) return;
  if (!write) { s_wram_flags[o] |= 0x01; return; }
  s_wram_flags[o] |= 0x02;
  s_wram_last_pc[o] = ((uint32_t)g_cpu->k << 16) | g_cpu->pc;
  if (s_wram_wcount[o] != 0xffff) s_wram_wcount[o]++;
}

static void write_wram_map(const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) { fprintf(stderr, "failed to open %s\n", path); return; }
  fwrite(s_wram_flags, 1, 0x20000, f);
  fwrite(s_wram_last_pc, 4, 0x20000, f);
  fwrite(s_wram_wcount, 2, 0x20000, f);
  fclose(f);
  uint32_t r = 0, w = 0, hot = 0;
  for (int i = 0; i < 0x20000; i++) {
    if (s_wram_flags[i] & 1) r++;
    if (s_wram_flags[i] & 2) w++;
    if (s_wram_wcount[i] > 16) hot++;
  }
  fprintf(stderr, "[wrammap] read=%u written=%u hot(>16 writes)=%u of 131072 -> %s\n",
          r, w, hot, path);
}

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
  if (s_wram_map) wram_map_note(adr, false);
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
  if (s_wram_map) wram_map_note(adr, true);
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

/* SC_WIDESCREEN=<pixels per side>: widen the rendered picture.
 *
 * The runner's PPU already supports this -- PpuSetExtraSpace() sets a
 * symmetric border and the internal render width becomes 256 + 2*extra, up to
 * kPpuExtraLeftRight (96) per side. Nothing here reimplements a renderer; the
 * guest still draws every pixel, so all the existing verification stays valid
 * (see docs/PLAN_renderer.md, Stage 1).
 *
 * Motivation is the map-scroll complaint: showing more map at once is a
 * different answer to "panning is slow" than making the pan faster, and unlike
 * the pan work it costs no authenticity.
 *
 * Buffers are sized for the maximum so the allocation never depends on the
 * runtime value; only the active width does. */
enum { kVideoWidth = 256, kVideoHeight = 224, kVideoPitch = kVideoWidth * 4 };
enum { kVideoWidthMax = kVideoWidth + 96 * 2,
       kVideoPitchMax = kVideoWidthMax * 4 };
static int s_ws_extra;                 /* pixels per side; 0 = authentic 256 */
static int s_video_w = kVideoWidth;    /* active render width */
static int s_video_pitch = kVideoPitch;
static uint8_t s_video_pixels[kVideoPitchMax * kVideoHeight];

/* `input1_currentState`'s bit layout is NOT the plain hardware joypad
 * register layout. The shared runner's auto-joy-read path (snes.c) does
 * `$4218 = SwapInputBits(input1_currentState) & 0xff; $4219 = ... >> 8`,
 * where SwapInputBits reverses all 16 bits. So bit i of input1_currentState
 * ends up at bit (15-i) of the value $4218/4219 are split from. Working
 * backwards from the real hardware layout the game actually reads --
 *
 *     $4218 (JOY1L)  bit7=A  bit6=X  bit5=L  bit4=R, bits3-0 = pad ID
 *     $4219 (JOY1H)  bit7=B  bit6=Y  bit5=Select bit4=Start
 *                    bit3=Up bit2=Down bit1=Left bit0=Right
 *
 * -- the constants below are what must be set in input1_currentState;
 * verified empirically against SimCity itself (holding each direction moves
 * exactly one cursor axis in the right direction: Left drives $01EB down,
 * Right up, Up drives $01ED down, Down up). Do not "simplify" these to the
 * naive hardware bit order -- that was the original bug.
 *
 * An earlier revision of this comment named the two registers the other way
 * round ("$4218 bit7=B..bit0=Right / $4219 bit7=A,6=X,5=L,4=R"), which
 * contradicted the enum three lines below it and is simply wrong: $4218 is
 * the LOW byte, so it carries A/X/L/R. The enum was always right. Same class
 * of defect as the stale keybinds.h comment corrected upstream in
 * mstan/snesrecomp#17, and worth the same care -- a wrong comment next to
 * right code is how the original transposition survived as long as it did. */
enum {
  /* Serial order, LSB first -- the order the pad shifts out of $4016:
   * B, Y, Select, Start, Up, Down, Left, Right, A, X, L, R. The runner
   * reverses all 16 bits and splits the result, so this convention lands
   * $4218 = A,X,L,R,0,0,0,0 and $4219 = B,Y,Select,Start,Up,Down,Left,Right
   * -- the real hardware layout -- with the runner UNMODIFIED. */
  kPad_B = 0x0001, kPad_Y = 0x0002, kPad_Select = 0x0004, kPad_Start = 0x0008,
  kPad_Up = 0x0010, kPad_Down = 0x0020, kPad_Left = 0x0040, kPad_Right = 0x0080,
  kPad_A = 0x0100, kPad_X = 0x0200, kPad_L = 0x0400, kPad_R = 0x0800,
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
/* Defined with the host-map block far below; used from the frame loop here. */
static void host_map_arm_captures(void);
static void host_map_compose(void);
static bool     s_host_map;
static uint8_t *s_hud_pixels;

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
    if (snes->vPos <= kVideoHeight) {
      /* Host-map mode renders each visible line TWICE: once with the layer
       * mask limited to BG3|OBJ into a scratch buffer, once normally. That
       * gets the HUD and sprites in isolation without the overlay export,
       * which arms cleanly but exports nothing. Needs no cooperation from the
       * runner beyond retargeting PpuBeginDrawing between the two calls. */
      if (s_host_map && s_hud_pixels && snes->vPos > 0) {
        /* Everything EXCEPT BG2, not just BG3|OBJ.
         *
         * BG2 is the map -- the only layer being replaced. Capturing every
         * other layer means anything the guest draws wins over the host map
         * automatically: the HUD, sprites, AND any menu, including the ones
         * that open *inside* the city view without changing $01df. Reported
         * from play: savestate_3 opens such a menu and $01df stays 3
         * throughout, so no screen-mode gate could ever have caught it.
         *
         * Self-correcting by construction, which is why it beats hunting for
         * a "menu is open" flag -- a search through the WRAM delta across the
         * B press turned up only transient direct-page scratch. */
        g_snes_ppu_dbg_layer_mask = (uint8_t)~0x02;   /* all but BG2 */
        PpuBeginDrawing(g_ppu, s_hud_pixels, (size_t)s_video_pitch, 0);
        ppu_runLine(g_ppu, snes->vPos);
        g_snes_ppu_dbg_layer_mask = 0xff;
        PpuBeginDrawing(g_ppu, s_video_pixels, (size_t)s_video_pitch, 0);
      }
      ppu_runLine(g_ppu, snes->vPos);
    }
    if (snes->vPos == 0) {
      /* Clamp the BG layers out of the widescreen margins on EVERY screen,
       * not only where the host map composes.
       *
       * Tied to the host map it only helped the city view, and a menu screen
       * ($01df 0/1/2/4) still tiled its background sideways -- reported from
       * play on a clean start, which sits at $01df == 4. The clamp belongs to
       * widescreen itself; whether the map is being replaced is a separate
       * question. Re-applied per frame, as the API requires. */
      if (s_ws_extra > 0) PpuSetWidescreenLayerClamp(g_ppu, 0x0F);
      host_map_arm_captures();
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
      host_map_compose();   /* all 224 visible lines are drawn by now */
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
 * calling the PRNG at 00:824b/00:824f, after seeding it from the map seed
 * ($0b27-$0b29) into a rolling pair of WRAM state words ($59/$5b/$5d).
 * Confirmed procedural map generation: 01:f1f1 draws a random byte from
 * that PRNG and branches ~34%/66% into five distinct terrain-feature
 * routines. (The iteration count is seed-derived, 1-32, not the fixed 10
 * this comment used to claim.) This is genuine, real computation, not a dumb
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
 * Root cause of the false positives: 00:824b is the game's PRNG (see the
 * ROM_MAP entry -- an additive generator over $59/$5b, taking no input,
 * which is what rules out the "checksum" reading these comments used to
 * carry), so it is called constantly throughout ordinary simulation, not
 * just during map generation. Same conclusion as before -- it is a bad
 * trigger -- but for a much more obvious reason. The older wording below is
 * kept only because the decision it justified still stands:
 * 00:824b is the shared checksum/hash
 * accumulator, not map-generation-specific code -- the game calls it
 * during normal simulation too. It's also redundant as a trigger, since
 * 03:d862 (the map-gen loop that calls it) is itself already a trigger,
 * so it's dropped from the trigger set entirely rather than merely gated.
 * The two remaining triggers are genuinely load-specific. Toggle the
 * feature from the F10 settings menu ("AUTO TURBO ON LOAD"); Tab-held
 * manual fast-forward is unaffected either way. */
/* AUTO TURBO is DELETED, not merely defaulted off.
 *
 * Reported from play: it makes the sound laggy. That matches the long note
 * above -- intermittent 6x bursts during ordinary gameplay made the game feel
 * rough and worsened the fast-forward audio delay. A setting whose only honest
 * advice is "leave it alone" is worse than no setting, so the row and the flag
 * are gone.
 *
 * What it existed for survives as MAPGEN TURBO, which uses the same two
 * load-specific triggers but only inside the generation/decompression window,
 * where nothing is being listened to either. */
static bool s_generating;
static unsigned long s_gen_trigger_hits;
static unsigned long s_gen_boost_frames;
static int s_gen_loop_active_frames; /* counts down; >0 means "recently seen" */

/* Guest frames per host frame while the map-generation / decompression loop is
 * active. This is how the generation wait is removed WITHOUT moving generation
 * host-side.
 *
 * Generating the map host-side would cross the line docs/PLAN_renderer.md sets
 * out: the map is state, not presentation. It is genuinely procedural --
 * 03:d840 seeds the PRNG a seed-dependent 1-32 times, then JSL $01f1ed runs
 * five distinct terrain-feature routines -- so a host implementation would have
 * to match it bit-for-bit, and any divergence would produce a different city
 * with nothing to detect it. The guest stays authoritative; it just runs
 * faster while nobody is looking at the screen.
 *
 * Separate from AUTO TURBO, which stays off by default for its own reasons
 * (it fired during ordinary gameplay and made the game feel rough). This only
 * engages on the two load-specific triggers. */
static int s_mapgen_turbo = 16;   /* 1 = off */
static const int kMapgenTurbos[] = { 1, 4, 8, 16, 32, 64 };
#define SC_GEN_LOOP_HOLDOFF 20 /* frames to keep boosting after the last hit */

/* Post-load power dropout fix.
 *
 * Stock-ROM bug. Found and fixed by **Truttle1** (https://www.youtube.com/@Truttle1),
 * whose `PowerBugPatch.bps` is what identified bit 15 as the power bit; the
 * analysis below is a re-derivation against our own ROM, and the patch itself
 * is not redistributed here. After loading a saved game the city reads as
 * unpowered for a couple of seconds, and because the decline logic runs during
 * that window the load actively costs population.
 *
 * Bit 15 ($8000) of each 16-bit map cell at $7F0200 is the **power** bit. The
 * tile index is the low 10 bits (see docs/REFERENCE_map_format.md), and the
 * upper bits are the simulation's. Corroboration from the ROM: `03:99a0`
 * writes a building into the map as `AND #$8000 ; ... ; ORA $00`, i.e. it
 * deliberately *preserves* bit 15 of whatever was in the cell -- exactly what
 * you do to a flag another subsystem owns.
 *
 * The fix is to mark every cell powered once, immediately after a load, and
 * let the game's own power scan clear whatever is genuinely unpowered on its
 * next pass. `03:c8dd` is the point to do it: `03:c8c8` has just run the map
 * unpacker (`JSR $d15f`) and `03:c8cb` the SRAM load (`JSR $c8e1`), so the map
 * is in place.
 *
 * Implemented host-side rather than by porting Truttle1's bytes. That patch
 * injects a routine into free ROM at `00:fb4c` and redirects `03:c8dd` to it;
 * reproducing its code here would be redistributing someone else's work, which
 * this repo does not do (same reason the Lua map viewer and the Sylt hack's
 * data are referenced but never vendored). Doing it from C
 * needs no free ROM space and avoids a quirk of that patch: because it
 * replaces `STZ $003a ; RTS` with a 4-byte `JSL`, its `RTL` lands on `03:c8e1`
 * and runs the SRAM loader a second time. That is harmless -- the loader is an
 * idempotent copy and does not touch $7F0200-$7F5FBF -- but it is not
 * something worth reproducing.
 *
 * One deliberate difference: Truttle1's patch skips the fix when `$0421`
 * is 1. `$0421` selects the save slot (`03:c8e6` uses it to pick base
 * `$700000` vs `$703ff0`), so that guard appears to exclude the second save
 * slot from the fix. This applies to both slots. If that turns out to matter,
 * this is the line to revisit. */
static bool s_power_fix = true;
static uint32_t s_power_fix_hits;

static void apply_power_fix(void) {
  for (int i = 0; i < 12000; i++)
    g_ram[0x10201 + i * 2] |= 0x80;   /* $7F0200 + i*2 + 1, bit 7 = cell bit 15 */
  if (s_power_fix_hits++ < 8)
    fprintf(stderr, "[powerfix] marked 12000 cells powered after load "
            "(hit #%u, frame %llu)\n", s_power_fix_hits,
            (unsigned long long)s_frames);
}

/* Record one executed guest PC into the coverage bitmaps.
 *
 * Factored out of the per-opcode loop so the fiber host can feed the same
 * bitmaps through the bridge PC hook. Takes the PC and widths as arguments
 * rather than reading g_cpu, because in fiber mode g_cpu never executes. */
static void sc_note_executed_pc(uint32_t pc24, int mf, int xf) {
  const uint8_t bank = (uint8_t)((pc24 >> 16) & 0xff);
  const uint16_t pc  = (uint16_t)(pc24 & 0xffff);
  if (s_pc_bitmap_bank >= 0 && bank == s_pc_bitmap_bank && pc >= 0x8000 &&
      s_frames >= s_pc_bitmap_start_frame) {
    uint32_t idx = pc - 0x8000;
    s_pc_bitmap[idx >> 3] |= (uint8_t)(1u << (idx & 7));
  }
  if (s_pc_bitmap_bank == -2 && pc >= 0x8000 && bank < 64 &&
      s_frames >= s_pc_bitmap_start_frame) {
    uint32_t idx = pc - 0x8000;
    s_pc_bitmap_all[bank][idx >> 3] |= (uint8_t)(1u << (idx & 7));
  }
  if (s_mx_bitmap && pc >= 0x8000 && bank < 64) {
    uint32_t idx = pc - 0x8000;
    int mx = ((mf ? 1 : 0) << 1) | (xf ? 1 : 0);
    s_mx_bitmap[mx][bank][idx >> 3] |= (uint8_t)(1u << (idx & 7));
  }
  if (bank < 64) s_banks_seen |= (1ULL << bank);
}

/* Compiled bodies ENTERED, as manifest-style keys (pc24:MmXn).
 *
 * Deliberately not folded into the bitmaps. A bounce reports an entry, not an
 * extent -- the body then runs an unknown number of opcodes silently. The
 * manifest min_pc24/max_pc24 cannot fill that in either: those bounds swallow
 * nested routines and stop short of a truncated return (docs/OPEN_QUESTIONS.md
 * F5), so expanding them would manufacture coverage that never executed --
 * the same class of error as contaminating a union with SC_FREEZE runs.
 *
 * Reported separately so a tool can JOIN on the key, which is exactly how the
 * program manifest is indexed. */
#define SC_AOT_VARIANTS_MAX 4096
static uint32_t s_aot_variant[SC_AOT_VARIANTS_MAX];  /* (pc24<<2)|(m<<1)|x */
static uint32_t s_aot_variant_hits[SC_AOT_VARIANTS_MAX];
static int      s_aot_variant_count;
static uint64_t s_aot_variant_overflow;

static void sc_note_aot_entry(uint32_t pc24, int mf, int xf) {
  uint32_t key = (pc24 << 2) | ((mf ? 1u : 0u) << 1) | (xf ? 1u : 0u);
  for (int i = 0; i < s_aot_variant_count; i++)
    if (s_aot_variant[i] == key) { s_aot_variant_hits[i]++; return; }
  if (s_aot_variant_count >= SC_AOT_VARIANTS_MAX) { s_aot_variant_overflow++; return; }
  s_aot_variant[s_aot_variant_count] = key;
  s_aot_variant_hits[s_aot_variant_count] = 1;
  s_aot_variant_count++;
}

#ifdef SIMCITY_AOT_TIER
/* ── SC_FIBER=1: run the guest inside the fiber (migration step 3d) ───────
 *
 * The driver itself lives in src/simcity_fiberdrive.c, because it needs
 * cpu_state.h and that header declares a global `CpuState g_cpu` which
 * collides with this file's `Interp816 *g_cpu`. Keeping it in its own
 * translation unit is cheaper than renaming a symbol used several hundred
 * times here.
 *
 * Strictly opt-in. Without SC_FIBER the interpreter path below is untouched,
 * because that path is this project's correctness baseline and every
 * `--qualify` number rests on it. */
static bool s_fiber_mode;

/* One host frame in the fiber model: advance the PPU and devices for a whole
 * frame (so raster effects still work line by line, per MIGRATION_step3 §4),
 * release the vblank wait the way the NMI handler would, then let the guest
 * run until its vblank HLE hands the frame back. */
/* Beam advance, exposed to the frame driver (src/simcity_fiberdrive.c).
 * handle_pos_stuff() is static and deeply tied to this file, so the driver
 * calls in rather than duplicating the device model. */
/* One beam step, catching the APU up on the same cadence the per-opcode path
 * uses.
 *
 * snes_catchupApu() clamps apuCatchupCycles to 10000 SPC cycles as a runaway
 * guard (snes.c). The interpreter never trips it, because it catches up after
 * every opcode -- ~48 master cycles, about 2 SPC cycles. A beam advance that
 * runs a whole frame and then catches up once does trip it, hard: one frame is
 * 357368 master, i.e. ~17046 SPC cycles, so the clamp silently discarded 41%
 * of every frame's audio. That was the entire fiber-host audio shortfall --
 * 220 samples/frame measured against the real 534, failing --qualify's
 * >=500/frame bar while logic and video were both fine.
 *
 * Catching up every 24 steps (48 master) reproduces an average opcode's
 * cadence, which keeps the accumulator three orders of magnitude below the
 * clamp. */
unsigned long g_beam_steps;
static void sc_beam_step(void) {
  static unsigned steps;
  g_beam_steps++;
  handle_pos_stuff();
  g_snes->apuCatchupCycles += 2.0 * kApuCyclesPerMaster;
  if (++steps >= 24) { steps = 0; snes_catchupApu(g_snes); }
}
void sc_advance_beam_one_frame(void) {
  uint64_t before = s_frames;
  unsigned guard = 0;
  while (s_frames == before && guard++ < 400000) {
    sc_beam_step();
  }
  snes_catchupApu(g_snes);
}

/* Advance until the auto-joypad read has finished.
 *
 * This is what makes a per-frame host viable for this ROM at all. $4212 bit 0
 * is literally `autoJoyTimer > 0` (snes.c), armed with 4224 at vblank start
 * and counted down by the beam. The game waits on it every frame at 00:9280
 * (`LDA $4212 ; AND #$01 ; BNE`), from both 00:8151 and 00:8201. If the guest
 * gets the frame while that timer is still running, it spins forever, because
 * nothing advances the beam while the guest holds the CPU -- which is exactly
 * how the first fiber attempt deadlocked.
 *
 * Draining it here costs ~4224 master cycles at the top of the frame and needs
 * no HLE and no per-routine knowledge: the host simply does not hand over a
 * frame whose input latch is still busy. */
void sc_advance_until_input_ready(void) {
  unsigned guard = 0;
  while (g_snes->autoJoyTimer && guard++ < 40000) {
    sc_beam_step();
  }
  snes_catchupApu(g_snes);
}

/* Catch a vblank entry that happened inside the bridge rather than in this
 * host's beam loop.
 *
 * Both sides move snes->hPos/vPos (see MIGRATION_step3 21), and only one of
 * them raises NMI. handle_pos_stuff() detects the entry by SAMPLING the beam at
 * hPos==0 on the overscan line; snes_advance_beam() (snes.c) just assigns
 * `inVblank = v >= 225` as it goes. So whenever the guest's own execution
 * carried the beam across line 225, the host's sample for that frame never
 * occurred: no ppu_handleVblank, no NMI, no auto-joypad arm. Measured cost --
 * 425 NMIs against the per-opcode host's 592 over 600 frames, a 28% shortfall,
 * which is the whole of the two hosts' phase divergence.
 *
 * inNmi is the discriminator: handle_pos_stuff() sets it on a processed entry
 * and snes_advance_beam() never touches it, so `inVblank && !inNmi` means
 * exactly "the beam is in vblank and nobody processed getting there". */
static void sc_catch_missed_vblank(void) {
  Snes *snes = g_snes;
  if (!snes->inVblank) { snes->inNmi = false; return; }
  if (snes->inNmi) return;
  ppu_handleVblank(g_ppu);
  snes->inNmi = true;
  if (snes->nmiEnabled) { g_cpu->nmiWanted = true; s_nmi_requests++; }
  if (snes->autoJoyRead) snes->autoJoyTimer = 4224;
}
static bool run_one_frame_fiber(void) {
  uint64_t before = s_frames;

  unsigned guard = 0;
  while (s_frames == before && guard++ < 400000) {
    sc_beam_step();
  }
  snes_catchupApu(g_snes);

  /* Hand the host's NMI request to the guest instead of faking its effect.
   * handle_pos_stuff() raises NMI on g_cpu, the Interp816 -- which never
   * executes in fiber mode, so the request used to sit there unconsumed
   * while this line forged the handler's INC $b9:
   *
   *     g_ram[0xb9] = 1;
   *
   * That released 00:930d's wait and skipped the rest of 00:80B2, i.e. the
   * per-frame PPU work. The driver now delivers a real interrupt. */
  sc_catch_missed_vblank();
  bool nmi_pending = false;
  if (g_cpu->nmiWanted) { g_cpu->nmiWanted = false; nmi_pending = true; }

  {
    static uint64_t last_guest_master;
    bool ok = SimCityFiberDrive_RunGuestFrame(s_frames, nmi_pending);
    s_nmi_serviced = SimCityFiberDrive_NmiDelivered();
    /* Mirror the guest clock into the host counter the qualify bar and the
     * APU pacing read. Without this the frame path reports master=0 and every
     * cycle-derived check reads as dead. */
    uint64_t now = SimCityFiberDrive_MasterCycles();
    if (now > last_guest_master) {
      uint64_t delta = now - last_guest_master;
      g_master_cycles += delta;
      /* Pace the APU off GUEST time as well as host beam time.
       *
       * The beam is advanced by both sides: this host's sc_beam_step(), and
       * the bridge's per-opcode snes_sync_master_clock() (interp_bridge.c),
       * which moves the same snes->hPos/vPos by the guest's master delta. So
       * a frame is split between them in a ratio that varies per frame, and
       * whichever side moves the beam, the elapsed wall time is the same.
       *
       * Pacing the SPC off host beam steps alone therefore starved it exactly
       * in proportion to how much of the frame the guest had consumed. It was
       * not a small effect: measured per-frame DSP production was ~540 on
       * host-heavy frames and *0* on guest-heavy ones, about 30% of frames,
       * dragging the average to 418/frame against --qualify's >=500 bar.
       * (SC_APU_DIAG=1 prints that ledger.)
       *
       * The bridge does not cover this itself: bridge_apu_flush() takes the
       * absolute-timeline early-out, which clears its pending master count
       * without advancing the SPC because it expects an RtlRunFrame host to
       * do the sync. This host does not call RtlRunFrame, so the guest's
       * share of the frame reached the APU from nowhere at all.
       *
       * Chunked because snes_catchupApu() clamps the accumulator at 10000 SPC
       * cycles as a runaway guard, and a whole frame is ~17046 -- the same
       * clamp sc_beam_step() has to stay under. 4096 master is ~195 SPC. */
      while (delta) {
        uint32_t chunk = delta > 4096u ? 4096u : (uint32_t)delta;
        g_snes->apuCatchupCycles += (double)chunk * kApuCyclesPerMaster;
        snes_catchupApu(g_snes);
        delta -= chunk;
      }
    }
    last_guest_master = now;
    sc_catch_missed_vblank();
    return ok;
  }
}
#endif /* SIMCITY_AOT_TIER */

/* s_fiber_mode only exists in the AOT build. */
#ifdef SIMCITY_AOT_TIER
static bool sc_fiber_active(void) { return s_fiber_mode; }
#else
static bool sc_fiber_active(void) { return false; }
#endif

static int      s_disaster_bit = -1;
static uint64_t s_disaster_frame;

static void scenario_event_tick(void);            /* defined with the menu */
static void arm_scenario_event(unsigned idx, uint16_t countdown, const char *what);
static void service_disaster_menu8(void);

/* SC_SCENARIO_EVENT=<meltdown|ufo>@<frame>: the headless twin of the F10
 * MELTDOWN / UFO rows, so the trigger can be verified without a human at the
 * window. */
static int      s_scen_event_idx = -1;
static uint16_t s_scen_event_cd;
static uint64_t s_scen_event_frame;
static const char *s_scen_event_name;

static void sc_maybe_trigger_scenario_event(void) {
  if (s_scen_event_idx < 0 || s_frames < s_scen_event_frame) return;
  arm_scenario_event((unsigned)s_scen_event_idx, s_scen_event_cd, s_scen_event_name);
  s_scen_event_idx = -1;
}

/* Zero the cursor step-delay countdown every frame.
 *
 * 01:c0dd gates the cursor step on $01f3: `LDA $01f3 ; BEQ +4 ; DEC $01f3 ;
 * RTS` -- while it is nonzero the step is skipped, which is what makes holding
 * a button act once per few frames instead of continuously (bulldozing "only
 * step by step").
 *
 * The US build fixes this by patching the two `STA $01f3` immediates from 3 to
 * 0 in ROM. $01f3 is WRAM, so zeroing it here does the same thing without a
 * byte patch -- and therefore works on EVERY region, including the ones whose
 * ROM sites we have never located. 16-bit, so both halves. */
static bool s_fast_ticks = true;

/* Guest frames per host frame while a mouse button is held -- see DRAG TURBO
 * in the main loop. 1 = off. */
static int s_drag_turbo = 1;
static const int kDragTurbos[] = { 1, 2, 3, 4, 6 };

/* Fire the scripted SC_DISASTER trigger once, at its frame. */
static void sc_maybe_trigger_disaster(void) {
  if (s_disaster_bit < 0 || s_frames < s_disaster_frame) return;
  g_ram[0x0197] |= (uint8_t)(1u << s_disaster_bit);
  fprintf(stderr, "[disaster] set $0197 bit %d -> $0197=%02x at frame %llu\n",
          s_disaster_bit, g_ram[0x0197], (unsigned long long)s_frames);
  s_disaster_bit = -1;
}

static bool run_one_frame(void) {
#ifdef SIMCITY_AOT_TIER
  if (s_fiber_mode) return run_one_frame_fiber();
#endif
  if (s_fast_ticks) { g_ram[0x01f3] = 0; g_ram[0x01f4] = 0; }
  sc_maybe_trigger_disaster();
  sc_maybe_trigger_scenario_event();
  scenario_event_tick();
  service_disaster_menu8();
  Snes *snes = g_snes;
  Interp816 *cpu = g_cpu;
  uint64_t target = s_frames + 1;
  long guard = 20000000; /* runaway guard: caps opcodes/frame, mirrors ref_driver.c */
  while (s_frames < target && guard-- > 0) {
    if (cpu->k == 0x00 && cpu->pc == 0x80b2) s_nmi_serviced++;
    /* Unconditional now that AUTO TURBO is gone. This only opens the
     * generation/decompression window; whether anything speeds up is MAPGEN
     * TURBO's decision, and 1 means off. */
    /* Hold the boost for the WHOLE generation, not a fixed window after the
     * trigger.
     *
     * 03:d862 is the PRNG seeding loop -- 1 to 32 iterations, over in an
     * instant. The actual work is the two JSLs after it, 01:f1ed (terrain
     * features) and 02:923f. Measured with a 20-frame holdoff: 4 trigger hits,
     * 20 boosted frames, and the wait untouched, because the holdoff expired
     * long before the generator finished.
     *
     * 03:d871 is where execution resumes once both JSLs have returned, so the
     * pair bounds the generation exactly. The decompressor at 00:90dd keeps a
     * plain holdoff -- it has no equivalent end marker and is short. */
    if (cpu->k == 0x03 && cpu->pc == 0xd862) { s_gen_trigger_hits++; s_generating = true; }
    if (cpu->k == 0x03 && cpu->pc == 0xd871) s_generating = false;
    if (cpu->k == 0x00 && cpu->pc == 0x90dd) {
      s_gen_trigger_hits++;
      s_gen_loop_active_frames = SC_GEN_LOOP_HOLDOFF;
    }
    /* Post-load power fix -- see apply_power_fix(). 03:c8dd is reached with
     * the map already unpacked and SRAM already restored. */
    /* TWO hook points, because there are two ways a map arrives.
     *
     * 03:c8dd is the save-load path: 03:c8c8 has unpacked the map and
     * 03:c8cb has restored SRAM. Scenarios never go through it -- they load
     * via 03:ce2e, which unpacks with its own JSR $d15f at 03:ce5e and
     * returns to 03:ce61. So the power fix has been firing on loaded cities
     * and never on scenarios, which is exactly the "scenarios lose power at
     * start" report: the same post-load dropout, unpatched.
     *
     * 03:ce61 is the scenario equivalent -- map in place, about to return. */
    if (s_power_fix && cpu->k == 0x03 &&
        (cpu->pc == 0xc8dd || cpu->pc == 0xce61)) apply_power_fix();
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
    sc_note_executed_pc(((uint32_t)cpu->k << 16) | cpu->pc,
                        cpu->mf ? 1 : 0, cpu->xf ? 1 : 0);
    if (s_pc_capture_after > 0) {
      fprintf(stderr, "[pctrace] pc=%02x:%04x a=%04x x=%04x y=%04x p=%02x%s%s\n",
              cpu->k, cpu->pc, cpu->a, cpu->x, cpu->y,
              interp816_getFlags(cpu), cpu->mf ? " m8" : " m16", cpu->xf ? " x8" : " x16");
      s_pc_capture_after--;
    }
    if (s_dump_pc_armed &&
        ((uint32_t)cpu->k << 16 | cpu->pc) == s_dump_pc24) {
      char path[512];
      snprintf(path, sizeof(path), "%s/wram_%010llu.bin", s_dump_pc_dir,
               (unsigned long long)s_dump_pc_frame);
      if (!write_wram_dump(path))
        fprintf(stderr, "failed to write WRAM dump to %s\n", path);
      s_dump_pc_armed = false;
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
  if (s_generating || s_gen_loop_active_frames > 0) s_gen_boost_frames++;
  if (s_gen_loop_active_frames > 0) s_gen_loop_active_frames--;
  return guard > 0;
}

/* SC_HOST_MAP_DUMP=<file>: render the map host-side and write it as a PPM,
 * at whatever frame SC_DUMP_AT names.
 *
 * A verification hook, not a feature. tools/render_map.py was validated
 * against real play first; dumping the C port the same way lets the two be
 * diffed, so the port is checked against a known-good implementation rather
 * than only against itself. Nothing here touches presentation yet. */
/* SC_HOST_MAP=1: draw the map host-side and composite the game's own HUD and
 * sprites back on top.
 *
 * BG3 and OBJ are captured into transparent ARGB surfaces via the overlay
 * export (snesrecomp/docs/HOST_OVERLAY_EXTRACTION.md, ported from the ActRaiser
 * fork) with RemoveFromGame, so the guest frame comes out carrying only the
 * layers we are replacing. Real alpha, not a black key.
 *
 * The guest still computes everything -- this only draws the map differently,
 * which is the state/presentation line docs/PLAN_renderer.md sets out.
 *
 * Opt-in: with nothing bound and no capture configured the export is a
 * documented no-op, so the default build stays byte-identical. */
static uint8_t *s_ov_bg3, *s_ov_obj;
/* HUD-only pass: BG3 + OBJ rendered into their own buffer, independent of the
 * overlay export. The export arms cleanly and reports success but writes no
 * pixels, and every check on this side came back correct, so this route stops
 * depending on it entirely -- it needs nothing from the runner but the public
 * layer mask and a retargeted PpuBeginDrawing. */
static uint32_t s_backdrop_argb;
static int s_ov_pitch;

static void host_map_init(void) {
  if (!s_host_map || !g_ppu || s_ov_bg3) return;
  /* Pitch MUST match the render width, not the maximum allocation.
   *
   * PpuWriteOverlayRenderLine centres the authentic 256-wide capture inside
   * whatever surface it is given:
   *
   *     width         = pitch / 4
   *     texture_extra = max((width - 256) / 2, 0)
   *     dst[x + texture_extra] = ...
   *
   * A 448-wide surface therefore receives the HUD at columns 96..351 while a
   * composite reading from column 0 sees only the transparent left margin.
   * That is exactly why both surfaces came back with zero non-transparent
   * pixels while binding and arming reported success. */
  s_ov_pitch = s_video_pitch;
  s_hud_pixels = (uint8_t *)calloc((size_t)s_video_pitch, kVideoHeight);
  s_ov_bg3 = (uint8_t *)calloc((size_t)s_ov_pitch, kVideoHeight);
  s_ov_obj = (uint8_t *)calloc((size_t)s_ov_pitch, kVideoHeight);
  if (!s_ov_bg3 || !s_ov_obj) { s_host_map = false; return; }
  PpuClearOverlayBindings(g_ppu);
  bool a = PpuBindOverlaySurface(g_ppu, kPpuOverlaySource_Bg3, s_ov_bg3, (size_t)s_ov_pitch);
  bool b = PpuBindOverlaySurface(g_ppu, kPpuOverlaySource_Obj, s_ov_obj, (size_t)s_ov_pitch);
  fprintf(stderr, "host map: overlay bind bg3=%d obj=%d, bgmode=%d\n",
          (int)a, (int)b, (int)PPU_mode(g_ppu));
}

/* Per frame, before any line renders. */
static void host_map_arm_captures(void) {
  if (!s_host_map || !g_ppu || !s_ov_bg3) return;
  /* Keep the UI layers out of the widescreen margins.
   *
   * BG3 is a tilemap like BG2, so widening the picture tiles the toolbar and
   * status bar sideways exactly as it did the map -- reported from play as
   * "the UI seems repeated too". Clamping pins them to the authentic 256
   * columns; the composite below then anchors that block to the left edge.
   *
   * BG2 is clamped too and costs nothing: it is the layer being replaced.
   * Must be re-applied every frame, per the API contract. */
  memset(s_ov_bg3, 0, (size_t)s_ov_pitch * kVideoHeight);
  memset(s_ov_obj, 0, (size_t)s_ov_pitch * kVideoHeight);
  PpuClearOverlayCaptures(g_ppu);
  bool c3 = PpuSetOverlayCapture(g_ppu, kPpuOverlaySource_Bg3, 0, 0, s_video_w,
                                 kVideoHeight, kPpuOverlayFlag_RemoveFromGame);
  bool co = PpuSetOverlayCapture(g_ppu, kPpuOverlaySource_Obj, 0, 0, s_video_w,
                                 kVideoHeight, kPpuOverlayFlag_RemoveFromGame);
  { static int shown = 0;
    if (shown < 2) { shown++;
      fprintf(stderr, "host map: capture armed bg3=%d obj=%d mode=%d\n",
              (int)c3, (int)co, (int)PPU_mode(g_ppu)); } }
}

/* After the guest frame: replace the picture with our map, then put the
 * captured HUD and sprites back over it using their real alpha. */
static void host_map_compose(void) {
  if (!s_host_map || !s_ov_bg3) return;
  /* Only on the main map screen. $01df is the screen-mode index: 3 is the
   * city view, while 0/1/2 are the menu pages (measured across the save
   * states). Without this the map painted over the scenario select, the
   * disaster page and everything else -- reported from play as "menu broken",
   * and entirely my omission rather than a renderer fault. */
  /* NO screen-mode gate.
   *
   * $01df was read as "3 means the city view". It is not: the same city view
   * with a tool palette open has been observed at 3, at 4 AND at 0. Gating on
   * it made the map vanish and left the widescreen margins black -- reported
   * from play.
   *
   * No gate is needed. The capture pass takes every layer except BG2, so
   * whatever the guest draws on any other layer covers the map by itself.
   * That is the same property that made in-view menus work without a special
   * case, applied consistently. */
  { static int shown = 0;
    if (shown < 3) { shown++;
      int n3 = 0, no = 0;
      for (int y = 0; y < kVideoHeight; y++) {
        const uint32_t *b3 = (const uint32_t *)(s_ov_bg3 + (size_t)y * s_ov_pitch);
        const uint32_t *ob = (const uint32_t *)(s_ov_obj + (size_t)y * s_ov_pitch);
        for (int x = 0; x < s_video_w; x++) { if (b3[x] >> 24) n3++; if (ob[x] >> 24) no++; }
      }
      /* KNOWN ISSUE: both counts are 0. The captures arm successfully and the
       * mode is 1, which HOST_OVERLAY_EXTRACTION.md lists as covered, yet the
       * surfaces stay empty -- so the HUD and sprites do not come back and the
       * frame is bare map. Reported from play as "map works, no overlay".
       * Whatever the reason is, it is inside the runner's export path rather
       * than this wiring. Diagnostic kept until it is understood. */
      fprintf(stderr, "host map: composing, bgmode=%d bg3px=%d objpx=%d\n",
              (int)PPU_mode(g_ppu), n3, no); } }
  int sx = 0, sy = 0;
  ScMapView_GetScroll(&sx, &sy);
  const int cols = (s_video_w + 7) / 8, rows = (kVideoHeight + 7) / 8;
  if (!ScMapView_Render(s_video_pixels, s_video_pitch, cols, rows, sx, sy)) return;
  /* Composite the HUD-only pass over the map.
   *
   * An isolated render still paints the backdrop, so "not black" is the wrong
   * test -- the whole scratch buffer would count as opaque. Key on the actual
   * backdrop colour instead, taken from CGRAM entry 0 through the same
   * brightness the PPU applies, so it matches whatever the pass produced. */
  if (s_hud_pixels) {
    uint16_t bd = g_ppu->cgram[0];
    s_backdrop_argb = 0xFF000000u
        | ((uint32_t)g_ppu->brightnessMult[bd & 0x1f] << 16)
        | ((uint32_t)g_ppu->brightnessMult[(bd >> 5) & 0x1f] << 8)
        | (uint32_t)g_ppu->brightnessMult[(bd >> 10) & 0x1f];
    /* Anchor the UI to the upper-left rather than leaving it centred.
     *
     * With the layers clamped, the guest draws its UI into the authentic 256
     * columns, which sit centred at x = s_ws_extra .. s_ws_extra+255 in a
     * widened frame. Reading with that offset lands the block flush against
     * the left edge, so the toolbar and status bar stay where they belong and
     * the extra width goes entirely to map. */
    const int ui_shift = s_ws_extra;
    for (int y = 0; y < kVideoHeight; y++) {
      uint32_t *dst = (uint32_t *)(s_video_pixels + (size_t)y * s_video_pitch);
      const uint32_t *hud = (const uint32_t *)(s_hud_pixels + (size_t)y * s_video_pitch);
      for (int x = 0; x < s_video_w; x++) {
        int sxp = x + ui_shift;
        if (sxp >= s_video_w) break;
        uint32_t p = hud[sxp];
        if (p != s_backdrop_argb && (p & 0x00FFFFFFu) != 0) dst[x] = p;
      }
    }
  }
}

static bool write_host_map_ppm(const char *path, int cols, int rows) {
  const int w = cols * 8, h = rows * 8;
  const int pitch = w * 4;
  uint8_t *buf = (uint8_t *)malloc((size_t)pitch * h);
  if (!buf) return false;
  { const char *z = getenv("SC_MAP_ZOOM");
    if (z && *z) ScMapView_SetCellPx(atoi(z)); }
  int sx = 0, sy = 0;
  ScMapView_GetScroll(&sx, &sy);
  if (!ScMapView_Render(buf, pitch, cols, rows, sx, sy)) {
    fprintf(stderr, "host map: render refused (non-US ROM, or PPU/ROM not ready)\n");
    free(buf); return false;
  }
  FILE *f = fopen(path, "wb");
  if (!f) { free(buf); return false; }
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int y = 0; y < h; y++) {
    const uint32_t *row = (const uint32_t *)(buf + (size_t)y * pitch);
    for (int x = 0; x < w; x++) {
      uint8_t rgb[3] = { (uint8_t)(row[x] >> 16), (uint8_t)(row[x] >> 8),
                         (uint8_t)row[x] };
      if (fwrite(rgb, 1, 3, f) != 3) { fclose(f); free(buf); return false; }
    }
  }
  fprintf(stderr, "host map: %dx%d from cell (%d,%d) -> %s\n",
          w, h, sx, sy, path);
  free(buf);
  return fclose(f) == 0;
}

static bool write_ppm(const char *path) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  /* Row-wise at the ACTIVE width, using the buffer's real stride. The buffer
   * is allocated for the widescreen maximum, so a flat index over
   * s_video_w * height would walk diagonally through it. */
  fprintf(f, "P6\n%d %d\n255\n", s_video_w, kVideoHeight);
  for (int y = 0; y < kVideoHeight; y++) {
    const uint32_t *row = (const uint32_t *)(s_video_pixels + (size_t)y * s_video_pitch);
    for (int x = 0; x < s_video_w; x++) {
      uint8_t rgb[3] = { (uint8_t)(row[x] >> 16), (uint8_t)(row[x] >> 8), (uint8_t)row[x] };
      if (fwrite(rgb, 1, 3, f) != 3) { fclose(f); return false; }
    }
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
  bool ok = false;
#if SNESRECOMP_SDL3
  /* SDL3 allocates and returns a surface instead of filling a caller buffer,
   * and its format follows the renderer rather than what this dumper wants,
   * so convert before copying. Row by row: the pitch is not necessarily w*4.
   *
   * This must run BEFORE SDL_RenderPresent -- on SDL3 the backbuffer contents
   * are undefined after present, so reading afterwards yields black. That is
   * the trap this function fell into. */
  { SDL_Surface *shot = SDL_RenderReadPixels(renderer, NULL);
    if (shot) {
      SDL_Surface *conv = SDL_ConvertSurface(shot, SDL_PIXELFORMAT_ARGB8888);
      if (conv) {
        for (int y = 0; y < h && y < conv->h; y++)
          memcpy(buf + (size_t)y * w,
                 (const uint8_t *)conv->pixels + (size_t)y * conv->pitch,
                 (size_t)w * 4);
        SDL_DestroySurface(conv);
        ok = true;
      }
      SDL_DestroySurface(shot);
    } }
#else
  ok = SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888,
                            buf, w * 4) == 0;
#endif
  if (ok) {
    FILE *f = fopen(path, "wb");
    if (f) {
      fprintf(f, "P6\n%d %d\n255\n", w, h);
      for (int i = 0; i < w * h; i++) {
        uint8_t rgb[3] = { (uint8_t)(buf[i] >> 16), (uint8_t)(buf[i] >> 8),
                           (uint8_t)buf[i] };
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
static const char *const kScenarioOverrideNames[] = { "OFF", "LAS VEGAS", "FREE PLAY" };

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
      snes_read(g_snes, 0x700002) != 'M') {
    /* Say so once. This used to return in silence, which makes the toggle
     * look broken rather than pending: switching it on at the title screen
     * does nothing at all until a real session has formatted SRAM, and there
     * was no way to tell that from "it didn't work". */
    static bool warned;
    if (!warned) {
      warned = true;
      fprintf(stderr, "[unlock] waiting: SRAM not formatted yet (no SIM magic "
              "at $700000). Start a game once, then this applies by itself.\n");
    }
    return;
  }
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
/* Mouse sensitivity, as a percentage applied after the window-scale divide.
 * 100 = one SNES pixel per SNES pixel of pointer travel. */
static int s_mouse_sensitivity = 100;
/* Direction the host mouse last moved, fed to the pad while a mouse button is
 * held so the ROM runs its own cursor/drag path instead of only seeing a
 * teleported cursor. */
/* Map tiles the right-drag pan may advance per frame, per axis. */

static int s_pan_max_tiles = 1;
static const int kPanMaxTiles[] = { 1, 2, 3, 4, 6, 8 };

static uint16_t s_pan_dir;
static int      s_pan_dir_frames;
static uint16_t s_mouse_dir;
static int      s_mouse_dir_frames;
static const int kMouseSensitivities[] = { 50, 75, 100, 150, 200 };

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

/* kSettingHeader is a non-interactive section label. It carries no field and
 * no action; navigation skips over it so Up/Down still lands only on real
 * rows. Sections exist because the list grew past the point where a flat
 * column of twenty-odd entries reads as one undifferentiated block. */
typedef enum { kSettingBool, kSettingBit, kSettingAction, kSettingCycle,
               kSettingHeader } SettingKind;

typedef struct {
  const char *label;
  SettingKind kind;
  void *field;           /* bool* (Bool), uint8_t* (Bit), int* (Cycle) */
  uint8_t mask;           /* kSettingBit only */
  void (*action)(void);   /* kSettingAction only */
  const int *values;      /* kSettingCycle only: allowed values, cycled in order */
  int value_count;
  /* kSettingCycle only, optional: one label per entry of `values`. A bare
   * number is fine for a step size, but not for an index whose meaning is
   * arbitrary -- SCENARIO OVR showing "6" and "7" cost a whole play session,
   * because 7 is free play and looks like a perfectly reasonable next value
   * after Las Vegas. Name them and the mistake is unavailable. */
  const char *const *value_names;
} SettingDesc;

static bool setting_get(const SettingDesc *d) {
  switch (d->kind) {
    case kSettingHeader: return false;
    case kSettingBool: return *(bool *)d->field;
    case kSettingBit:  return (*(uint8_t *)d->field & d->mask) != 0;
    default: return false;
  }
}

static void setting_activate(SettingDesc *d) {
  switch (d->kind) {
    case kSettingHeader: break;
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

/* Disaster triggers: $0197 is the pending-disaster bitfield serviced by the
 * six-arm ladder at 03:b8ae, which calls one handler per bit and then masks
 * that bit off. Setting a bit here is exactly what the game's own
 * disaster-selection page does, so these fire the real code path rather than
 * simulating anything.
 *
 * Named only where a single-disaster recording has actually attributed the
 * bit (see docs/ROM_MAP.md); bits 0 and 1 are still unidentified and are
 * labelled by number so the menu never asserts something unproven. Naming
 * them after a guess is how the $0199 mistake happened. */
/* ARM TRIGGERS: a safety catch in front of the disaster rows.
 *
 * Requested after a stray selection set one off mid-game. The rows sit right
 * under the cheats in a menu navigated with the D-pad, and firing an
 * earthquake by accident is not recoverable without a save state. Off by
 * default, so the triggers do nothing until deliberately armed. */
static bool s_disaster_armed;

static bool disaster_triggers_armed(const char *what) {
  if (s_disaster_armed) return true;
  fprintf(stderr, "[menu] %s ignored -- ARM TRIGGERS is off\n", what);
  return false;
}

static void trigger_disaster_bit(unsigned bit, const char *what) {
  g_ram[0x0197] |= (uint8_t)(1u << bit);
  fprintf(stderr, "[menu] set $0197 bit %u (%s) -> $0197=%02x, frame %llu\n",
          bit, what, g_ram[0x0197], (unsigned long long)s_frames);
}
/* Scenario events: MELTDOWN and UFO are NOT $0197 bits.
 *
 * They are dispatched from 03:b96f on the per-scenario countdown $0c0d,
 * gated on $003e == 3 (scenario mode) and keyed on $0040 (scenario index):
 *
 *   $0040 == 4 (Boston)     and $0c0d == 1          -> JSR $bac1  meltdown
 *   $0040 == 6 (Las Vegas)  and ($0c0d & 15) == 0   -> JSR $bcb8  UFO
 *
 * So a trigger has to set three words together, and two of them identify the
 * city -- leaving $003e/$0040 changed would tell the game it is playing a
 * different scenario, which would corrupt the win check and the next save.
 * Arm them, then restore as soon as the ROM has taken the countdown to zero
 * with its own DEC $0c0d at 03:b9c9. That is the ROM reporting the event has
 * fired, so the restore is self-timing rather than a guessed frame delay --
 * simulation ticks are many frames apart and vary with game speed.
 *
 * Deliberately NOT a freeze: the values are set once and the ROM is left to
 * consume them, so execution stays on paths the game really takes. */
/* The loaded ROM image, so the UFO population gate can be lifted for the
 * duration of a triggered event. Set in main() once the ROM is read. */
static bool s_rom_is_us = true;
static uint8_t *s_rom_data;
static uint32_t s_rom_size;

static struct {
  bool        armed;
  uint16_t    saved_3e, saved_40;
  uint16_t    armed_cd;
  bool        gate_lifted;
  const char *what;
} s_scenario_event;

static uint16_t ram_w(uint32_t a) { return (uint16_t)(g_ram[a] | (g_ram[a+1] << 8)); }
static void ram_set_w(uint32_t a, uint16_t v) {
  g_ram[a] = (uint8_t)(v & 0xff); g_ram[a+1] = (uint8_t)(v >> 8);
}

/* The UFO checks the city population before it will appear:
 *
 *   03:b9b3  LDA $0ba5 ; CMP #$4c08 ; LDA $0ba7 ; SBC #$0001
 *   03:b9bf  BCC $b9c4          ; under 84,488 -> skip the UFO
 *   03:b9c1  JSR $bcb8
 *
 * NOP the branch (90 03 -> EA EA) so the call is reached regardless. Patching
 * the CODE rather than writing a fake population is the conservative choice:
 * $0ba5/$0ba7 are live simulation state that taxes, milestones and the win
 * check all read, so faking them even for one tick would change the game in
 * ways nothing here could bound. Two bytes of branch, by contrast, affect
 * exactly this decision.
 *
 * Scoped to the armed window and reverted with the rest of the trigger, so a
 * real Las Vegas game still has its gate. Byte-checked before writing, the
 * same as the boot-time patches.
 *
 * Interpreter-tier only: a compiled body for 03:b96f would already have the
 * branch baked in, so this has no effect in the AOT build. The windowed build
 * this menu lives in is the interpreter, so that is not a limitation here. */
#define SC_UFO_GATE_OFF 0x1b9bfu    /* 03:b9bf, headerless LoROM file offset */

/* cart_init() does `cart->rom = malloc(); memcpy(...)`, so the cart holds its
 * OWN copy and the buffer read_file() returned is not what executes. The
 * boot-time patches above work only because they run before the cart is
 * built. Anything patched later has to go to cart->rom, or it silently does
 * nothing -- which is exactly what the first version of this did. */
static uint8_t *sc_live_rom(void) {
  if (g_snes && g_snes->cart && g_snes->cart->rom) return g_snes->cart->rom;
  return s_rom_data;
}

static bool lift_ufo_population_gate(void) {
  uint8_t *rom = sc_live_rom();
  if (!rom || SC_UFO_GATE_OFF + 1 >= s_rom_size) return false;
  uint8_t *p = rom + SC_UFO_GATE_OFF;
  if (p[0] != 0x90 || p[1] != 0x03) {
    fprintf(stderr, "[menu] UFO gate: unexpected bytes %02x %02x at 03:b9bf, not patching\n", p[0], p[1]);
    return false;
  }
  p[0] = 0xea; p[1] = 0xea;
  fprintf(stderr, "[menu] UFO gate lifted (03:b9bf BCC -> NOP NOP)\n");
  return true;
}

static void restore_ufo_population_gate(void) {
  uint8_t *rom = sc_live_rom();
  if (!rom || SC_UFO_GATE_OFF + 1 >= s_rom_size) return;
  uint8_t *p = rom + SC_UFO_GATE_OFF;
  p[0] = 0x90; p[1] = 0x03;
  fprintf(stderr, "[menu] UFO gate restored\n");
}

static void arm_scenario_event(unsigned idx, uint16_t countdown, const char *what) {
  if (s_scenario_event.armed) {
    fprintf(stderr, "[menu] %s: a scenario event is already armed\n", what);
    return;
  }
  s_scenario_event.saved_3e = ram_w(0x3e);
  s_scenario_event.saved_40 = ram_w(0x40);
  s_scenario_event.what     = what;
  s_scenario_event.armed    = true;
  ram_set_w(0x3e, 3);
  ram_set_w(0x40, (uint16_t)idx);
  ram_set_w(0x0c0d, countdown);
  s_scenario_event.armed_cd = countdown;
  s_scenario_event.gate_lifted = (idx == 6) ? lift_ufo_population_gate() : false;
  fprintf(stderr, "[menu] armed %s: $3e=3 $0040=%u $0c0d=%u (saved $3e=%u $0040=%u), frame %llu\n",
          what, idx, (unsigned)countdown,
          (unsigned)s_scenario_event.saved_3e, (unsigned)s_scenario_event.saved_40,
          (unsigned long long)s_frames);
}

/* Restore once the ROM has counted the event out. Called once per frame. */
static void scenario_event_tick(void) {
  if (!s_scenario_event.armed) return;
  /* Restore on the ROM's first DEC $0c0d, not on the countdown reaching
   * zero. Both arms decrement on the tick they fire, so this is one tick
   * either way for the meltdown (1 -> 0) but sixteen for the UFO
   * (16 -> 0), and leaving $0040 forced for sixteen ticks would have the
   * game think it is in the wrong scenario for most of a minute. */
  if (ram_w(0x0c0d) == s_scenario_event.armed_cd) return;
  ram_set_w(0x3e, s_scenario_event.saved_3e);
  ram_set_w(0x40, s_scenario_event.saved_40);
  if (s_scenario_event.gate_lifted) {
    restore_ufo_population_gate();
    s_scenario_event.gate_lifted = false;
  }
  s_scenario_event.armed = false;
  fprintf(stderr, "[menu] %s fired; restored $3e=%u $0040=%u at frame %llu\n",
          s_scenario_event.what, (unsigned)s_scenario_event.saved_3e,
          (unsigned)s_scenario_event.saved_40, (unsigned long long)s_frames);
}

/* SC_DISASTER_MENU8=1: put the meltdown and the UFO on the GAME'S OWN
 * disaster page, not just the F10 menu.
 *
 * The page (01:aa39, screen mode $01df == 2) walks $0197 as a checkbox list:
 *
 *   01:aa3e  ASL A ; ASL A     ; 2 shifts, so only bits 5..0 reach the walker
 *   01:aa45  LDY #$0005        ; 6 rows
 *   01:aa77  LDA $01a95c,X     ; bit-mask table
 *
 * Two things make this cheap. The mask table at 01:a95c already runs to
 * $0200, so bits 6 and 7 have masks sitting there unused; and the input path
 * does SBC #$0008 with only a negative check, so row indices 0-7 are already
 * accepted. Only the render side is capped at six.
 *
 * Dropping the two shifts lets all eight bits reach the walker, and bumping
 * the count to 8 draws two more checkboxes.
 *
 * The new bits are serviced HERE rather than by extending 03:b8ae. That
 * ladder is a fixed chain ending in PLD/RTS at 03:b914 with no room for two
 * more arms, and the meltdown and UFO are not ladder disasters anyway -- they
 * are the $0c0d scenario events, which already have a verified trigger above.
 * So the ROM patch only has to make the bits SETTABLE; the host reads them.
 *
 * OPT-IN because two things about it are unverified: whether rows 6 and 7 land
 * inside the menu box or on top of whatever is below it, and that they will
 * have no LABELS -- the row text comes from the page-setup dispatch
 * (01:aabf JSR ($9d1a,X)), not from the checkbox renderer, so the two new rows
 * draw a checkbox with nothing beside it until that is extended too. */
static bool s_disaster_menu8;

/* The disaster page cannot be widened in place -- slots 6 and 7 are IN USE.
 *
 * Attempted and reverted: raise the row count, then position the two new rows
 * by writing their bytes in the buffer at $7e2063 + row*16. Both failed, and
 * the second failed destructively.
 *
 * A clean dump shows slots 6/7 holding `e0 00 32 80` -- byte 3 a palette or
 * attribute byte -- and slot 8 holding different tiles again ($35/$33). The
 * buffer is shared with other UI elements, so writing checkbox tiles and
 * palettes there corrupts them wherever they appear. Reported from play as
 * "Speed, Options and Disasters are all colourful even when not selected",
 * which is exactly that.
 *
 * Two methodology notes, both of which cost real time here:
 *
 *   - Sweeping the position byte over $60-$88 rendered NOTHING at any value.
 *     Verifying that a WRAM write landed is not verifying a pixel changed.
 *   - Replaying a save state already parked on a page never re-runs that
 *     page setup, so it is blind to any setup-time change. Several
 *     screenshots taken that way proved nothing either way.
 *
 * The page is drawn by 01:d94f, which blits four 16-word rows from ROM tables
 * at 01:d8af/d8cf/d8ef/... into the tilemap at $7e2440. Adding entries means
 * authoring new table rows there, not moving bytes in the sprite buffer. */


static void service_disaster_menu8(void) {
  if (!s_disaster_menu8) return;
  uint8_t v = g_ram[0x0197];
  if (v & 0x40) { g_ram[0x0197] = (uint8_t)(v & ~0x40u);
                  arm_scenario_event(4, 1,  "nuclear meltdown (in-game menu)"); }
  else if (v & 0x80) { g_ram[0x0197] = (uint8_t)(v & ~0x80u);
                       arm_scenario_event(6, 16, "UFO (in-game menu)"); }
}

static void menu_trigger_meltdown(void) {
  if (disaster_triggers_armed("nuclear meltdown")) arm_scenario_event(4, 1, "nuclear meltdown");
}
/* The UFO additionally passes a population gate at 03:b9b3 -- a 32-bit
 * compare of ($0ba7:$0ba5) against $0001_4c08 -- so it will not appear in a
 * city under 84,488 people. Measured: on a small free-play city the arm is
 * reached and the gate rejects it, so the menu row is not broken, the city is
 * just too small. */
static void menu_trigger_ufo(void) {
  if (disaster_triggers_armed("UFO")) arm_scenario_event(6, 16, "UFO");
}

static void menu_trigger_fire(void) { if (disaster_triggers_armed("fire")) trigger_disaster_bit(0, "fire"); }
static void menu_trigger_flood(void) { if (disaster_triggers_armed("flood")) trigger_disaster_bit(1, "flood"); }
static void menu_trigger_plane(void) { if (disaster_triggers_armed("plane crash")) trigger_disaster_bit(2, "plane crash"); }
static void menu_trigger_tornado(void) { if (disaster_triggers_armed("tornado")) trigger_disaster_bit(3, "tornado"); }
static void menu_trigger_quake(void) { if (disaster_triggers_armed("earthquake")) trigger_disaster_bit(4, "earthquake"); }
static void menu_trigger_monster(void) { if (disaster_triggers_armed("monster")) trigger_disaster_bit(5, "monster"); }

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
  { "QOL",                   kSettingHeader, NULL, 0, NULL, NULL, 0 },
  { "MOUSE CURSOR",          kSettingBool, &s_mouse_enabled,       0,    NULL, NULL, 0 },
  { "FAST TICKS",            kSettingBool, &s_fast_ticks,          0,    NULL, NULL, 0 },
  { "DRAG TURBO",            kSettingCycle, &s_drag_turbo,          0,    NULL,
    kDragTurbos, (int)(sizeof(kDragTurbos) / sizeof(kDragTurbos[0])) },
  { "PAN SPEED",             kSettingCycle, &s_pan_max_tiles,       0,    NULL,
    kPanMaxTiles, (int)(sizeof(kPanMaxTiles) / sizeof(kPanMaxTiles[0])) },
  { "MOUSE SPEED",           kSettingCycle, &s_mouse_sensitivity,   0,    NULL,
    kMouseSensitivities, (int)(sizeof(kMouseSensitivities) / sizeof(kMouseSensitivities[0])) },
  { "FAST CURSOR",           kSettingBool, &s_fast_cursor_enabled, 0,    NULL, NULL, 0 },
  { "CURSOR SPEED",          kSettingCycle, &s_fast_cursor_step,   0,    NULL,
    kFastCursorSteps, (int)(sizeof(kFastCursorSteps) / sizeof(kFastCursorSteps[0])) },
  { "UNLOCK SCENARIOS",      kSettingBool, &s_unlock_all,          0,    NULL, NULL, 0 },
  { "FIX POWER ON LOAD",     kSettingBool, &s_power_fix,           0,    NULL, NULL, 0 },
  { "MAPGEN TURBO",          kSettingCycle, &s_mapgen_turbo,        0,    NULL,
    kMapgenTurbos, (int)(sizeof(kMapgenTurbos) / sizeof(kMapgenTurbos[0])) },
  { "CHEATS",                kSettingHeader, NULL, 0, NULL, NULL, 0 },
  { "CHEAT NO DISASTER",     kSettingBit,  &g_ram[0x0425],         0x01, NULL, NULL, 0 },
  { "CHEAT MONEY",           kSettingBit,  &g_ram[0x0425],         0x02, NULL, NULL, 0 },
  { "CHEAT VALVE MAX",       kSettingBit,  &g_ram[0x0425],         0x04, NULL, NULL, 0 },
  { "CHEAT WATER",           kSettingBit,  &g_ram[0x0425],         0x08, NULL, NULL, 0 },
  { "SET POP",               kSettingCycle, &s_pop_override,        0,    NULL,
    kPopOverrides, (int)(sizeof(kPopOverrides) / sizeof(kPopOverrides[0])) },
  { "SET CLASS",             kSettingCycle, &s_class_override,      0,    NULL,
    kClassOverrides, (int)(sizeof(kClassOverrides) / sizeof(kClassOverrides[0])) },
  { "CLR MILESTONE",         kSettingAction, NULL, 0, menu_action_clear_milestones, NULL, 0 },
  { "DISASTER TRIGGER",      kSettingHeader, NULL, 0, NULL, NULL, 0 },
  { "ARM TRIGGERS",          kSettingBool, &s_disaster_armed,      0,    NULL, NULL, 0 },
  { "FIRE",                  kSettingAction, NULL, 0, menu_trigger_fire,     NULL, 0 },
  { "FLOOD",                 kSettingAction, NULL, 0, menu_trigger_flood,    NULL, 0 },
  { "PLANE CRASH",           kSettingAction, NULL, 0, menu_trigger_plane,    NULL, 0 },
  { "TORNADO",               kSettingAction, NULL, 0, menu_trigger_tornado,  NULL, 0 },
  { "EARTHQUAKE",            kSettingAction, NULL, 0, menu_trigger_quake,    NULL, 0 },
  { "MONSTER",               kSettingAction, NULL, 0, menu_trigger_monster,  NULL, 0 },
  { "MELTDOWN",              kSettingAction, NULL, 0, menu_trigger_meltdown, NULL, 0 },
  { "UFO",                   kSettingAction, NULL, 0, menu_trigger_ufo,      NULL, 0 },
  { "STATE",                 kSettingHeader, NULL, 0, NULL, NULL, 0 },
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
          ScRect r = SC_RECT(cx + col * px, y + row * px, px, px);
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

  /* Font pixel size, in real screen pixels. Adaptive rather than a fixed 4:
   * the box is pad*2 + line_h*lines tall with line_h = 6*px and pad = 3*px,
   * i.e. 6*px*(lines+1), so a long enough list pushes menu_y negative and
   * silently clips the title off the top of the window. That is exactly what
   * adding the six disaster triggers did. Shrink to fit instead, capped at
   * the original 4 so short lists look unchanged. */
  const int lines = (int)kSettingCount + 6 + (s_menu_preview ? 4 : 0);
  int px = out_h / (6 * (lines + 1));
  if (px > 4) px = 4;
  if (px < 1) px = 1;
  const int line_h = 6 * px;   /* glyph height (5) + 1 row of spacing */
  const int pad = 3 * px;
  int menu_w = out_w * 3 / 4;
  int menu_h = pad * 2 + line_h * ((int)kSettingCount + 6 + (s_menu_preview ? 4 : 0));
  int menu_x = (out_w - menu_w) / 2;
  int menu_y = (out_h - menu_h) / 2;

  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 200);
  ScRect bg = SC_RECT(menu_x, menu_y, menu_w, menu_h);
  SDL_RenderFillRect(renderer, &bg);
  SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
  SDL_RenderDrawRect(renderer, &bg);

  int ty = menu_y + pad;
  draw_text(renderer, menu_x + pad, ty, px, "SETTINGS");
  ty += line_h * 2;

  for (size_t i = 0; i < kSettingCount; i++) {
    SettingDesc *d = &s_settings[i];
    bool selected = ((int)i == s_menu_selected);
    bool header = (d->kind == kSettingHeader);
    if (header) {
      /* Section labels sit flush left in a dimmer grey; the rows under them
       * are indented, so the grouping is visible without needing rules or a
       * second font. */
      SDL_SetRenderDrawColor(renderer, 150, 150, 255, 255);
      draw_text(renderer, menu_x + pad, ty, px, d->label);
      ty += line_h;
      continue;
    }
    SDL_SetRenderDrawColor(renderer, 255, selected ? 255 : 255, selected ? 0 : 255, 255);
    draw_text(renderer, menu_x + pad + 4 * px, ty, px, d->label);
    if (d->kind != kSettingAction) {
      char numbuf[16];
      const char *val;
      if (d->kind == kSettingCycle) {
        int cv = *(int *)d->field;
        int idx = -1;
        for (int k = 0; k < d->value_count; k++) if (d->values[k] == cv) idx = k;
        if (d->value_names && idx >= 0) {
          val = d->value_names[idx];
        } else if (cv < 0) {
          val = "OFF"; /* negative sentinel, so 0 stays a real selectable value */
        } else {
          snprintf(numbuf, sizeof(numbuf), "%d", cv);
          val = numbuf;
        }
      } else {
        val = setting_get(d) ? "ON" : "OFF";
      }
      int label_w = text_width(px, d->label);
      draw_text(renderer, menu_x + pad + 4 * px + label_w + 8 * px, ty, px, val);
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
static void write_mx_bitmap_dump(void) {
  if (!s_mx_bitmap || !s_mx_bitmap_path) return;
  FILE *f = fopen(s_mx_bitmap_path, "wb");
  if (!f) { fprintf(stderr, "SC_MX_BITMAP: cannot write %s\n", s_mx_bitmap_path); return; }
  fwrite(s_mx_bitmap, 1, 4 * sizeof(s_pc_bitmap_all), f);
  fclose(f);
  unsigned n[4] = {0,0,0,0};
  for (int mx = 0; mx < 4; mx++)
    for (int b = 0; b < 64; b++)
      for (int i = 0; i < 4096; i++)
        for (int k = 0; k < 8; k++)
          if (s_mx_bitmap[mx][b][i] & (1u << k)) n[mx]++;
  fprintf(stderr, "[mxbitmap] m0x0=%u m0x1=%u m1x0=%u m1x1=%u -> %s\n",
          n[0], n[1], n[2], n[3], s_mx_bitmap_path);
}

/* SC_AOT_VARIANTS=<file>: compiled variants entered during the run, one per
 * line as `pc24:MmXn hits=N` -- the same key the program manifest uses, so a
 * tool can join the two directly. Empty for a per-opcode run, which enters no
 * compiled bodies at all. */
static void write_aot_variants(void) {
  const char *path = getenv("SC_AOT_VARIANTS");
  if (!path || !s_aot_variant_count) return;
  FILE *f = fopen(path, "wb");
  if (!f) { fprintf(stderr, "SC_AOT_VARIANTS: cannot write %s\n", path); return; }
  fprintf(f, "# compiled bodies ENTERED. An entry is not an extent: the body\n");
  fprintf(f, "# then runs an unknown number of opcodes without reporting them,\n");
  fprintf(f, "# so this must NOT be expanded into an executed-PC bitmap.\n");
  for (int i = 0; i < s_aot_variant_count; i++) {
    uint32_t k = s_aot_variant[i];
    fprintf(f, "%06x:M%dX%d hits=%u\n", (unsigned)(k >> 2),
            (int)((k >> 1) & 1), (int)(k & 1), (unsigned)s_aot_variant_hits[i]);
  }
  fclose(f);
  fprintf(stderr, "[aotvariants] %d distinct compiled variants entered\n",
          s_aot_variant_count);
  if (s_aot_variant_overflow)
    fprintf(stderr, "[aotvariants] WARNING: %llu entries dropped (table full)\n",
            (unsigned long long)s_aot_variant_overflow);
}

/* SC_PPU_DUMP_DIR=<dir> [+ SC_PPU_DUMP_INTERVAL, SC_PPU_DUMP_START]: dump the
 * PPU side of the machine -- VRAM, CGRAM and OAM -- so two runs can be
 * compared on what is actually available to draw with, not just on which code
 * executed. Written for the UFO question: the renderer runs identically on a
 * practice map and on Las Vegas, so the difference has to be in this data. */
static void write_ppu_dump(uint64_t frame) {
  const char *dir = getenv("SC_PPU_DUMP_DIR");
  if (!dir || !g_ppu) return;
  char path[512];
  snprintf(path, sizeof(path), "%s/ppu_%010llu.bin", dir, (unsigned long long)frame);
  FILE *f = fopen(path, "wb");
  if (!f) { fprintf(stderr, "SC_PPU_DUMP_DIR: cannot write %s\n", path); return; }
  fwrite(g_ppu->vram,  2, 0x8000, f);   /* 64KB VRAM  */
  fwrite(g_ppu->cgram, 2, 0x100,  f);   /* 512B CGRAM */
  fwrite(g_ppu->oam,   2, 0x100,  f);   /* 512B OAM   */
  fclose(f);
}

static void write_pc_bitmap_dump(void) {
  write_mx_bitmap_dump();
  write_aot_variants();
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
    /* Active area only -- the buffer is sized for the widescreen maximum, and
     * hashing the unused tail would dilute the signal. */
    for (size_t i = 0; i < (size_t)s_video_pitch * kVideoHeight; i++)
      { vh ^= s_video_pixels[i]; vh *= 1099511628211ULL; }
    if (f > 1 && vh != last_video_hash) video_changes++;
    last_video_hash = vh;

    {
      const char *dump_at = getenv("SC_DUMP_AT");
      const char *dump_path = getenv("SC_DUMP_PATH");
      if (dump_at && dump_path && f == strtoull(dump_at, NULL, 0)) {
        { const char *hm = getenv("SC_HOST_MAP_DUMP");
          if (hm && *hm) {
            int hc = 32, hr = 28;
            { const char *e = getenv("SC_HOST_MAP_CELLS");
              if (e && *e) sscanf(e, "%d,%d", &hc, &hr); }
            write_host_map_ppm(hm, hc, hr);
          } }
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
          if (s_dump_pc24 != 0xffffffffu && !sc_fiber_active()) {
            /* Defer: fire at the guest PC instead. See SC_WRAM_DUMP_PC. */
            s_dump_pc_armed = true; s_dump_pc_frame = f;
            snprintf(s_dump_pc_dir, sizeof(s_dump_pc_dir), "%s", wram_dir);
          } else {
            char path[512];
            snprintf(path, sizeof(path), "%s/wram_%010llu.bin", wram_dir, (unsigned long long)f);
            if (!write_wram_dump(path))
              fprintf(stderr, "failed to write WRAM dump to %s\n", path);
          }
          write_ppu_dump(f);
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
    /* SC_APU_DIAG=1: per-frame audio ledger -- what the DSP produced, what
     * was queued, and what the drain took. Cheap and env-gated. */
    { static int diag = -1;
      if (diag < 0) diag = getenv("SC_APU_DIAG") ? 1 : 0;
      if (diag) fprintf(stderr, "[apu f=%llu] write=%u avail=%u produced=%d\n",
                        (unsigned long long)f, dsp->sampleWrite, available,
                        (int)(dsp->sampleWrite - last_sample_write));
#ifdef SIMCITY_AOT_TIER
      /* Beam-step ledger: only the fiber host has a host-side beam loop.
       * This is what showed the beam being advanced from two places at
       * once -- guest-heavy frames need only ~300 host steps instead of
       * 178684, because the bridge already moved hPos/vPos itself. */
      if (diag) { extern unsigned long g_beam_steps;
                  static unsigned long prev_steps;
                  fprintf(stderr, "[beam f=%llu] steps=%lu vPos=%d hPos=%d sf=%llu joy=%d\n",
                          (unsigned long long)f, g_beam_steps - prev_steps,
                          (int)g_snes->vPos, (int)g_snes->hPos,
                          (unsigned long long)s_frames,
                          (int)g_snes->autoJoyTimer);
                  prev_steps = g_beam_steps; }
#endif
    }
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
#ifdef SIMCITY_AOT_TIER
  /* Did the guest actually run COMPILED code? Without this the wall-clock
   * comparison in OPEN_QUESTIONS B2 is unreadable: a fiber run that quietly
   * interpreted everything would look exactly like a slow AOT tier. Tier-downs
   * are only reachable FROM a compiled body, so a nonzero count is positive
   * evidence that compiled code executed. */
  { extern long interp_tier_hit_count(void);
    extern void interp_tier2_stats(int *sites, unsigned long long *clean,
                                   unsigned long long *bail);
    int sites = 0; unsigned long long clean = 0, bail = 0;
    interp_tier2_stats(&sites, &clean, &bail);
    extern unsigned long long g_interp_bridge_bounces;
    extern unsigned long long g_interp_bridge_steps;
    extern unsigned SimCityFiberDrive_GuestS(void);
    extern unsigned SimCityFiberDrive_ResumePC(void);
    if (s_fiber_mode) fprintf(stderr, "guest: S=%04X resume=%06X\n",
                              SimCityFiberDrive_GuestS(), SimCityFiberDrive_ResumePC());
    else fprintf(stderr, "guest: S=%04X pc=%02X:%04X\n",
                         (unsigned)g_cpu->sp, (unsigned)g_cpu->k, (unsigned)g_cpu->pc);
    fprintf(stderr, "aot: bounces=%llu interp_steps=%llu tier_downs=%ld gap_sites=%d clean=%llu bail=%llu",
            g_interp_bridge_bounces, g_interp_bridge_steps,
            interp_tier_hit_count(), sites, clean, bail);
    fprintf(stderr, "\n"); }
#endif
  fprintf(stderr, "gen: trigger_hits=%lu boosted_frames=%lu turbo=%d\n",
          s_gen_trigger_hits, s_gen_boost_frames, s_mapgen_turbo);
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
  { const char *p = getenv("SC_WRAM_MAP"); if (s_wram_map && p) write_wram_map(p); }
  { const char *p = getenv("SC_SRAM_DUMP_PATH");
    if (p && *p) {
      report_sram_header("exit");
      fprintf(stderr, write_sram_dump(p) ? "dumped SRAM to %s\n"
                                         : "failed to write SRAM dump to %s\n", p);
    } }
  return rc;
}

int main(int argc, char **argv) {
  /* SC_LANG=U|E|F|G|J -- pick the regional ROM.
   *
   * All five regions are 512KB and all five pass --qualify 600 unchanged on
   * the interpreter tier, which is ROM-agnostic: it interprets whatever bytes
   * are there. So a language selector costs nothing but the file choice.
   *
   * The AOT tier is a different matter -- see the fingerprint guard below.
   *
   * Candidate filenames per region, tried in order, because the No-Intro names
   * carry decorations ("[!]") that vary by dump. An explicit ROM argument
   * always wins over SC_LANG. */
  const char *rom_path = "simcity.sfc";
  { const char *lang = getenv("SC_LANG");
    if (lang && *lang) {
      static const struct { char code; const char *names[3]; } kRoms[] = {
        { 'U', { "simcity.sfc", "Sim City (U) [!].sfc", NULL } },
        { 'E', { "Sim City (E) [!].sfc", "Sim City (E).sfc", NULL } },
        { 'F', { "Sim City (F).sfc", "Sim City (F) [!].sfc", NULL } },
        { 'G', { "Sim City (G) [!].sfc", "Sim City (G).sfc", NULL } },
        { 'J', { "Sim City (J).sfc", "Sim City (J) [!].sfc", NULL } },
      };
      char want = (char)toupper((unsigned char)lang[0]);
      const char *picked = NULL;
      for (size_t i = 0; i < sizeof(kRoms)/sizeof(kRoms[0]) && !picked; i++) {
        if (kRoms[i].code != want) continue;
        for (int n = 0; n < 3 && kRoms[i].names[n]; n++) {
          FILE *f = fopen(kRoms[i].names[n], "rb");
          if (f) { fclose(f); picked = kRoms[i].names[n]; break; }
        }
        if (!picked)
          fprintf(stderr, "SC_LANG=%c: no ROM file found for that region\n", want);
      }
      if (picked) { rom_path = picked; }
      else if (!strchr("UEFGJ", want))
        fprintf(stderr, "SC_LANG: want one of U E F G J\n");
    } }
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
  { const char *e = getenv("SC_WRAM_MAP");
    if (e && *e) {
      s_wram_flags = (uint8_t *)calloc(0x20000, 1);
      s_wram_last_pc = (uint32_t *)malloc(0x20000 * sizeof(uint32_t));
      s_wram_wcount = (uint16_t *)calloc(0x20000, sizeof(uint16_t));
      if (s_wram_flags && s_wram_last_pc && s_wram_wcount) {
        memset(s_wram_last_pc, 0xff, 0x20000 * sizeof(uint32_t));
        s_wram_map = true;
      }
    } }
  { const char *e = getenv("SC_WRAM_DUMP_PC");
    if (e && *e) {
      s_dump_pc24 = (uint32_t)strtoul(e, NULL, 16);
      fprintf(stderr, "SC_WRAM_DUMP_PC: dumps fire at guest %02X:%04X\n",
              (unsigned)(s_dump_pc24 >> 16), (unsigned)(s_dump_pc24 & 0xffff));
    } }
#ifdef SIMCITY_AOT_TIER
  /* SC_FIBER=1: drive the guest inside the fiber instead of interpreting it
   * per opcode (migration step 3d). Only meaningful in the AOT build, and
   * deliberately opt-in -- see run_one_frame_fiber(). */
  { const char *e = getenv("SC_FIBER");
    if (e && *e && *e != '0') {
      if (!SimCityFiberDrive_Init()) {
        fprintf(stderr, "SC_FIBER: could not start the game fiber\n");
        return 1;
      }
      s_fiber_mode = true;
      /* Feed the coverage bitmaps from the bridge, or a fiber run records
       * nothing at all and every tool in tools/ silently sees an empty
       * bitmap. Interpreted opcodes go into the bitmaps exactly as the
       * per-opcode host records them; compiled-body ENTRIES are collected
       * separately, because a bounce is not an extent. */
      { extern void (*g_interp_bridge_pc_hook)(uint32_t, int, int);
        extern void (*g_interp_bridge_bounce_hook)(uint32_t, int, int);
        g_interp_bridge_pc_hook = sc_note_executed_pc;
        g_interp_bridge_bounce_hook = sc_note_aot_entry; }
      fprintf(stderr, "[fiber] driving the guest inside the fiber "
                      "(entry I_RESET_M1X1)\n");
    } }
#endif
  { const char *e = getenv("SC_SCENARIO_EVENT");
    if (e && *e) {
      unsigned long long fr = 0; const char *at = strchr(e, 0x40);
      if (at) fr = strtoull(at + 1, NULL, 0);
      if (!strncmp(e, "meltdown", 8)) {
        s_scen_event_idx = 4; s_scen_event_cd = 1;  s_scen_event_name = "nuclear meltdown";
      } else if (!strncmp(e, "ufo", 3)) {
        s_scen_event_idx = 6; s_scen_event_cd = 16; s_scen_event_name = "UFO";
      } else {
        fprintf(stderr, "SC_SCENARIO_EVENT: expected meltdown|ufo\n");
      }
      s_scen_event_frame = fr;
    } }

  /* SC_DISASTER=<bit>@<frame>: set one $0197 disaster bit at a given frame,
   * headlessly. Exactly what the F10 menu does interactively -- the game's own
   * disaster-selection page sets these bits and 03:b8ae services them -- but
   * scriptable, so a single-disaster run can be recorded per bit without a
   * human driving menus. That is what docs/ROM_MAP.md asks for to attribute
   * the four unidentified arms.
   *
   * Not a freeze: the bit is set once and the ROM clears it itself after its
   * handler runs, so execution stays on paths the game really takes. */
  { const char *e = getenv("SC_DISASTER");
    if (e && *e) {
      unsigned bit = 0; unsigned long long at = 0;
      /* 0-5 are the ROM ladder arms; 6 and 7 are the meltdown and UFO rows
       * added by SC_DISASTER_MENU8, serviced host-side. */
      if (sscanf(e, "%u@%llu", &bit, &at) == 2 && bit < 8) {
        s_disaster_bit = (int)bit; s_disaster_frame = at;
      } else {
        fprintf(stderr, "SC_DISASTER: want <bit 0-7>@<frame>\n");
      }
    } }
  { const char *e = getenv("SC_MAP_WRITE_TRACE"); if (e && *e) s_map_write_trace = true; }
  { const char *e = getenv("SC_VIEW_WATCH"); if (e && *e) s_view_watch = true; }
  { const char *e = getenv("SC_MENU_PREVIEW");
    if (e && *e) { s_menu_preview = true; s_menu_open = true; } }
  { const char *e = getenv("SC_FREEZE"); if (e && *e) parse_freezes(e); }
  { const char *e = getenv("SC_PC_BITMAP_BANK");
    if (e && *e) s_pc_bitmap_bank = !strcmp(e, "all") ? -2 : (int)strtol(e, NULL, 16); }
  { const char *e = getenv("SC_MX_BITMAP");
    if (e && *e) {
      s_mx_bitmap = calloc(4, sizeof(s_pc_bitmap_all));
      if (s_mx_bitmap) s_mx_bitmap_path = e;
      else fprintf(stderr, "SC_MX_BITMAP: out of memory\n");
    } }
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
  s_rom_data = rom_data; s_rom_size = rom_size;
  /* Region report + AOT fingerprint guard.
   *
   * The generated AOT code is compiled against the US ROM: its addresses, its
   * dispatch table, every cfg directive. Point it at another region and it
   * would execute US code offsets over foreign bytes -- silently, and wrongly.
   *
   * The default path is safe today because it reports bounces=0, i.e. it is
   * pure interpreter; only SC_FIBER actually enters compiled bodies. So the
   * guard refuses SC_FIBER on a non-US image rather than refusing to run at
   * all, which keeps every region playable on the interpreter.
   *
   * FNV-1a over the whole file. US = 0xec01686a; E/F/G/J are 0xb76b1a0d,
   * 0xe1f99069, 0xaeca7623, 0xccb8c347. */
  { uint32_t fp = 2166136261u;
    for (uint32_t i = 0; i < rom_size; i++) { fp ^= rom_data[i]; fp *= 16777619u; }
    const uint8_t region = rom_size > 0x7fd9 ? rom_data[0x7fd9] : 0xff;
    const char *name = region == 0x00 ? "Japan" : region == 0x01 ? "USA"
                     : region == 0x02 ? "Europe" : region == 0x06 ? "France"
                     : region == 0x09 ? "Germany" : "unknown";
    s_rom_is_us = (fp == 0xec01686au);
    ScMapView_SetRomIsUs(s_rom_is_us);
    fprintf(stderr, "rom: %s  region=%s (%02x)  fnv=%08x%s\n",
            rom_path, name, region, fp, s_rom_is_us ? "  [AOT-compatible]" : "");
  }
#ifdef SIMCITY_AOT_TIER
  /* Checked HERE, not where SC_FIBER is parsed: env parsing runs before the ROM
   * is read, so the fingerprint is not known yet. The first version of this
   * guard sat at the parse site and did nothing at all -- a German ROM ran 60
   * compiled bounces straight past it. */
  if (s_fiber_mode && !s_rom_is_us) {
    fprintf(stderr,
            "SC_FIBER refused: the AOT code is compiled against the US ROM and "
            "this image is a different region.\n"
            "  Run without SC_FIBER -- the interpreter tier handles every "
            "region.\n");
    return 1;
  }
#endif
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
      /* Gated on the US fingerprint: this checks a SINGLE byte, which cannot
       * identify a site in a different build. Measured: without the gate, both
       * US patches "applied" cleanly to all of E/F/G/J -- i.e. they were
       * patching foreign ROMs on coincidental byte matches. */
      if (s_rom_is_us && off < rom_size && rom_data[off] == 0x03) {
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
  /* SC_DISASTER_MENU8=1 -- see service_disaster_menu8(). Two byte patches:
   * drop the pair of ASLs so all eight $0197 bits reach the row walker, and
   * raise the row count from 6 to 8. Byte-checked, and applied here because
   * cart_init() copies the ROM -- a patch after that lands in a buffer nobody
   * reads. */
  if (getenv("SC_DISASTER_MENU8")) {
    /* The row-count patch is GONE. Raising LDY #$0005 to #$0007 and dropping
     * the two ASLs did give the page eight bits to walk, and it wrecked the
     * colours on the Speed, Options and Disasters pages: slots 6 and 7 of the
     * buffer at $7e2063 are not free, so the extra rows wrote checkbox tiles
     * and palettes over other UI elements. See the note above.
     *
     * What is left is only the host-side servicing of bits 6 and 7, which
     * touches no ROM and keeps SC_DISASTER=6/7 usable as a headless trigger.
     * The F10 MELTDOWN and UFO rows remain the working way to fire these. */
    s_disaster_menu8 = true;
    fprintf(stderr, "disaster bits 6/7 serviced host-side (no ROM patch)\n");
  }
  {
    uint32_t off = 0x40fb; /* 00:c0fb's STA $7e21b5 (long), file offset = addr-0x8000 (bank 0) */
    if (s_rom_is_us && off + 3 < rom_size && rom_data[off] == 0x8f && rom_data[off+1] == 0xb5 &&
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
  { const char *e = getenv("SC_WIDESCREEN");
    if (e && *e) {
      int v = atoi(e);
      if (v < 0) v = 0;
      if (v > 96) v = 96;
      s_ws_extra = v;
    } }
  { const char *e = getenv("SC_MAPGEN_TURBO");
    if (e && *e) { int v = atoi(e); if (v >= 1 && v <= 256) s_mapgen_turbo = v; } }
  { const char *e = getenv("SC_HOST_MAP");
    if (e && *e && *e != '0') s_host_map = true; }
  if (s_ws_extra > 0) {
    s_video_w = kVideoWidth + s_ws_extra * 2;
    s_video_pitch = s_video_w * 4;
    PpuSetExtraSpace(g_ppu, (uint8_t)s_ws_extra);
    fprintf(stderr, "widescreen: %d px per side -> %dx%d\n",
            s_ws_extra, s_video_w, kVideoHeight);
  }
  PpuBeginDrawing(g_ppu, s_video_pixels, (size_t)s_video_pitch, 0);
  host_map_init();

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

  /* SDL3 returns true on success where SDL2 returned 0, so a bare `!= 0`
   * reads a successful init as a failure -- with an empty SDL_GetError(),
   * because nothing actually went wrong. Caught only by launching the window:
   * --qualify never initialises video, so the headless verification that
   * cleared the SDL3 migration could not have found this. */
#if SNESRECOMP_SDL3
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
#else
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
#endif
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window *window = snesrecomp_sdl_create_window(
      "SimCitySNESRecomp", s_video_w * scale, kVideoHeight * scale, 0);
  if (!window) { fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError()); return 1; }
  /* No SDL_RENDERER_PRESENTVSYNC: on some hosts (observed under a VM) the
   * driver's vsync wait blocks for longer than one real display refresh
   * (e.g. ~33ms instead of ~16.67ms), silently halving the whole loop's
   * rate -- since simulation advancement here is 1:1 with each present,
   * that drags the SNES-side "logical" game down to half speed too (every
   * animation, not just this cursor), while the interpreter itself was
   * never the bottleneck (SC_FRAME_TIME showed zero frames exceeding the
   * 16.67ms budget). Pace manually against the wall clock instead below. */
  /* vsync off deliberately -- see the comment above; pacing is manual. */
  SDL_Renderer *renderer = snesrecomp_sdl_create_renderer(window, false, false);
  if (!renderer) { fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError()); return 1; }
  SDL_Texture *texture = SDL_CreateTexture(
      renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
      s_video_w, kVideoHeight);

  /* The framebuffer is ARGB8888 but the PPU never writes an alpha byte, so
   * every pixel carries A=0. Under SDL2 that was harmless: a texture defaults
   * to SDL_BLENDMODE_NONE and alpha is ignored. SDL3 defaults the same texture
   * to blending, so A=0 renders it fully transparent -- a black window, with a
   * frame loop, blit and present that all report success. Pin the mode. */
  SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);

  /* Queued (pushed) audio, not a pull callback: this host owns the DSP drain
   * loop and hands over finished samples. SDL3 removed SDL_QueueAudio and
   * folded the same behaviour into SDL_AudioStream, so the backend difference
   * lives in sc_sdl_compat.h rather than here. */
  ScAudio audio; SDL_memset(&audio, 0, sizeof(audio));
  bool audio_dev = sc_audio_open(&audio, 32040, 2, 1024);
  if (audio_dev) {
    fprintf(stderr, "audio: opened freq=%d channels=%d samples=%d\n",
            audio.freq, audio.channels, audio.samples);
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
      if (ev.type == SDL_KEYDOWN && SNESRECOMP_SDL_EVENT_KEY(ev) == SDLK_ESCAPE) quit = true;
      if (ev.type == SDL_KEYDOWN && SC_EVENT_SCANCODE(ev) == SDL_SCANCODE_F1) {
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
      if (ev.type == SDL_KEYDOWN && SC_EVENT_SCANCODE(ev) == SDL_SCANCODE_F2 && !ev.key.repeat) {
        queue_debug_menu_code(s_frames + 1);
      }
      /* +/- : zoom the host-rendered map.
       *
       * Only possible because the map is drawn host-side; the guest's own
       * renderer is fixed at 8 pixels per cell. Steps through a small set of
       * cell sizes rather than free-scaling, so every step stays an exact
       * nearest-neighbour ratio and the tiles keep their shape.
       *
       * The HUD is unaffected -- it comes from the guest at 1:1 and is
       * composited after, so it stays crisp at every zoom level. */
      if (ev.type == SDL_KEYDOWN && !ev.key.repeat && s_host_map) {
        static const int kCellSizes[] = { 2, 4, 8, 16, 32 };
        const int n = (int)(sizeof(kCellSizes) / sizeof(kCellSizes[0]));
        SDL_Scancode sc = SC_EVENT_SCANCODE(ev);
        int dir = 0;
        if (sc == SDL_SCANCODE_EQUALS || sc == SDL_SCANCODE_KP_PLUS) dir = +1;
        if (sc == SDL_SCANCODE_MINUS  || sc == SDL_SCANCODE_KP_MINUS) dir = -1;
        if (dir) {
          int cur = ScMapView_GetCellPx(), idx = 2;
          for (int i = 0; i < n; i++) if (kCellSizes[i] == cur) idx = i;
          idx += dir;
          if (idx < 0) idx = 0;
          if (idx >= n) idx = n - 1;
          ScMapView_SetCellPx(kCellSizes[idx]);
          fprintf(stderr, "[zoom] %d px per map cell (%s)\n", kCellSizes[idx],
                  kCellSizes[idx] == 8 ? "native" :
                  kCellSizes[idx] > 8 ? "zoomed in" : "zoomed out");
        }
      }

      /* F3: toggle host-mouse cursor control (see apply_mouse_delta below).
       * Off by default -- it's a ported experimental community patch, and
       * incidental OS mouse movement over the window shouldn't silently
       * steer the game cursor unless asked for. */
      if (ev.type == SDL_KEYDOWN && SC_EVENT_SCANCODE(ev) == SDL_SCANCODE_F3 && !ev.key.repeat) {
        s_mouse_enabled = !s_mouse_enabled;
        if (s_mouse_enabled) SDL_GetRelativeMouseState(NULL, NULL); /* discard stale accumulated delta */
        fprintf(stderr, "[F3] mouse cursor control %s\n", s_mouse_enabled ? "ON" : "OFF");
      }
      /* F9: toggle the fast D-pad cursor (see apply_mouse_delta/
       * s_fast_cursor_enabled above). Off by default -- same "opt-in,
       * not authentic ROM behavior" reasoning as F3. */
      if (ev.type == SDL_KEYDOWN && SC_EVENT_SCANCODE(ev) == SDL_SCANCODE_F9 && !ev.key.repeat) {
        s_fast_cursor_enabled = !s_fast_cursor_enabled;
        fprintf(stderr, "[F9] fast D-pad cursor %s\n", s_fast_cursor_enabled ? "ON" : "OFF");
      }
      /* F10: settings menu (see the "minimal in-game settings menu" block
       * above) -- toggles a host-side overlay listing this project's
       * existing toggles/actions in one generic, table-driven list instead
       * of each needing its own memorized hotkey. */
      if (ev.type == SDL_KEYDOWN && SC_EVENT_SCANCODE(ev) == SDL_SCANCODE_F10 && !ev.key.repeat) {
        s_menu_open = !s_menu_open;
        fprintf(stderr, "[F10] settings menu %s\n", s_menu_open ? "OPEN" : "CLOSED");
      }
      if (s_menu_open && ev.type == SDL_KEYDOWN) {
        switch (SC_EVENT_SCANCODE(ev)) {
          case SDL_SCANCODE_UP:
            /* Step until a non-header lands under the cursor. Bounded by
             * kSettingCount so an all-header table cannot spin forever. */
            for (size_t n = 0; n < kSettingCount; n++) {
              s_menu_selected =
                  (s_menu_selected - 1 + (int)kSettingCount) % (int)kSettingCount;
              if (s_settings[s_menu_selected].kind != kSettingHeader) break;
            }
            break;
          case SDL_SCANCODE_DOWN:
            for (size_t n = 0; n < kSettingCount; n++) {
              s_menu_selected = (s_menu_selected + 1) % (int)kSettingCount;
              if (s_settings[s_menu_selected].kind != kSettingHeader) break;
            }
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
      if (ev.type == SDL_KEYDOWN && SC_EVENT_SCANCODE(ev) == SDL_SCANCODE_F4 && !ev.key.repeat) {
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
        switch (SC_EVENT_SCANCODE(ev)) {
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
          if (SC_EVENT_SCANCODE(ev) != kSlotKeys[i].sc) continue;
          char path[32];
          snprintf(path, sizeof(path), "savestate_%c.bin", kSlotKeys[i].digit);
          if (SC_EVENT_KEYMOD(ev) & KMOD_SHIFT) {
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
      /* SDL reports the pointer delta in HOST SCREEN pixels; the cursor lives
       * in SNES pixels. Feeding one straight into the other made the cursor
       * move `scale` times too fast -- 3x at the default window size. Reported
       * from play as "the mouse input seems way too fast", on every region,
       * not just the one it was noticed on.
       *
       * Divide by the live window scale rather than the `scale` variable, so a
       * resized or fullscreened window stays correct. The remainder is carried
       * rather than truncated, or slow movement below one SNES pixel per frame
       * would be silently dropped and the cursor would feel sticky. */
      /* SDL3 changed this to float* -- SDL_GetRelativeMouseState(float*,float*).
       * Passing int* is not an error in MSVC C, only warning C4133, so it built
       * clean under a grep that matched "error C" and the SDL3 build spent the
       * whole time reinterpreting float bits as ints. The deltas were garbage,
       * clamped to +/-127, which is why the cursor was "way too fast in every
       * setting" -- no sensitivity could scale a nonsense number.
       *
       * Third time this family of change has bitten: SDL_Init returning bool,
       * SDL_Rect becoming SDL_FRect, and now this. SDL_ENABLE_OLD_NAMES keeps
       * the NAME working, which is exactly what makes it dangerous. */
      int mdx = 0, mdy = 0;
#if SNESRECOMP_SDL3
      { float fx = 0.0f, fy = 0.0f;
        SDL_GetRelativeMouseState(&fx, &fy);
        mdx = (int)fx; mdy = (int)fy; }
#else
      SDL_GetRelativeMouseState(&mdx, &mdy);
#endif
      if (mdx || mdy) {
        int ow = 0, oh = 0;
#if SNESRECOMP_SDL3
        SDL_GetRenderOutputSize(renderer, &ow, &oh);
#else
        SDL_GetRendererOutputSize(renderer, &ow, &oh);
#endif
        double sx = ow > 0 ? (double)ow / (double)s_video_w  : (double)scale;
        double sy = oh > 0 ? (double)oh / (double)kVideoHeight : (double)scale;
        if (sx < 1.0) sx = 1.0;
        if (sy < 1.0) sy = 1.0;
        const double sens = (double)s_mouse_sensitivity / 100.0;
        static double acc_x, acc_y;
        acc_x += (double)mdx * sens / sx;
        acc_y += (double)mdy * sens / sy;
        int step_x = (int)acc_x, step_y = (int)acc_y;
        acc_x -= step_x; acc_y -= step_y;
        /* Right button held = pan the map, not move the cursor.
         *
         * That mirrors what the ROM itself does: holding A deactivates the
         * cursor and turns the D-pad into a map scroll (01:afbe's ladder over
         * $01bd/$01bf). Driving $01eb/$01ed while the game is trying to scroll
         * fought that routine -- the direction arrows appeared but only the
         * cursor moved.
         *
         * $01bd/$01bf are the confirmed scroll pair, clamped by 01:a0c4
         * against bounds in $01c5-$01cb, which are read here rather than
         * assumed. Scroll is in map tiles, so the SNES-pixel delta is divided
         * down; SC_PAN_DIV tunes it. */
        if (SDL_GetMouseState(NULL, NULL) & SDL_BUTTON(SDL_BUTTON_RIGHT)) {
          /* Pan by driving the ROM's OWN scroll, not by poking $01bd/$01bf.
           *
           * Writing the scroll pair directly tore the map even at one tile per
           * frame, because the ROM updates its tilemap in step with that value
           * during its own frame work -- a host write lands at an arbitrary
           * point and the map redraws half-updated.
           *
           * Holding A is exactly the game's own "deactivate cursor, move the
           * map" mode (01:8d8a -> 01:afbe). Synthesising A plus a direction
           * makes the ROM scroll itself, so the update is coordinated and
           * tear-free by construction, at whatever rate the game supports. */
          static double pacc_x, pacc_y;
          static int pan_div = -1;
          if (pan_div < 0) {
            const char *e = getenv("SC_PAN_DIV");
            pan_div = (e && *e) ? atoi(e) : 8;
            if (pan_div < 1) pan_div = 1;
          }
          pacc_x += (double)step_x / pan_div;
          pacc_y += (double)step_y / pan_div;
          int px = (int)pacc_x, py = (int)pacc_y;
          pacc_x -= px; pacc_y -= py;
          s_pan_dir = 0;
          if (px < 0) s_pan_dir |= kPad_Left;
          if (px > 0) s_pan_dir |= kPad_Right;
          if (py < 0) s_pan_dir |= kPad_Up;
          if (py > 0) s_pan_dir |= kPad_Down;
          if (s_pan_dir) s_pan_dir_frames = 2;
        } else if (step_x || step_y) {
          apply_mouse_delta(step_x, step_y);
          /* Remember the direction of travel. While a button is held the ROM
           * needs to see the cursor MOVE through its own path -- poking
           * $01eb/$01ed behind its back moves the sprite but never raises the
           * "cursor moved" event its drag handling keys off, which is why
           * holding the button only acted once instead of continuously. The
           * synthesised d-pad below closes that gap. */
          s_mouse_dir = 0;
          if (step_x < 0) s_mouse_dir |= kPad_Left;
          if (step_x > 0) s_mouse_dir |= kPad_Right;
          if (step_y < 0) s_mouse_dir |= kPad_Up;
          if (step_y > 0) s_mouse_dir |= kPad_Down;
          s_mouse_dir_frames = 2;   /* survive a frame the pointer did not move */
        }
      }
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
    /* Letter bindings are resolved by *keycode*, not scancode, so they follow
     * the labels on the keyboard rather than QWERTY positions.
     *
     * This used to accept SDL_SCANCODE_Z and SDL_SCANCODE_Y together, which
     * papered over the QWERTZ/QWERTY swap only because both fed the same
     * button. They are separate buttons now (Y = SNES Y, X = SNES B), so the
     * positional approach would put them on the wrong buttons on a German
     * layout -- there the key labelled Y sits where QWERTY has Z.
     * SDL_GetScancodeFromKey maps "the key that types this character" to its
     * scancode under the active layout, which is what keys[] is indexed by.
     * Resolved once: the layout can change at runtime, but re-querying every
     * frame for every button buys nothing here. */
    static SDL_Scancode sc_l, sc_r, sc_x, sc_a, sc_y, sc_b, sc_select;
    static bool binds_ready = false;
    if (!binds_ready) {
      sc_l      = sc_scancode_from_key(SDLK_q);   /* Q -> L      */
      sc_r      = sc_scancode_from_key(SDLK_w);   /* W -> R      */
      sc_x      = sc_scancode_from_key(SDLK_a);   /* A -> X      */
      sc_a      = sc_scancode_from_key(SDLK_s);   /* S -> A      */
      sc_y      = sc_scancode_from_key(SDLK_y);   /* Y -> Y      */
      sc_b      = sc_scancode_from_key(SDLK_x);   /* X -> B      */
      sc_select = sc_scancode_from_key(SDLK_b);   /* B -> Select */
      binds_ready = true;
    }
    if (keys[sc_l]) input |= kPad_L;
    if (keys[sc_r]) input |= kPad_R;
    if (keys[sc_x]) input |= kPad_X;
    if (keys[sc_a]) input |= kPad_A;
    if (keys[sc_y]) input |= kPad_Y;
    if (keys[sc_b]) input |= kPad_B;
    if (keys[SDL_SCANCODE_RETURN]) input |= kPad_Start;
    /* Select is bound to B (not Shift) so Shift is free for the
     * save-state slot hotkeys (Shift+1..Shift+0) without also feeding a
     * Select press into the game every time a state is saved/loaded. */
    if (keys[sc_select]) input |= kPad_Select;
    /* Mouse buttons: LEFT = SNES B, RIGHT = SNES A.
     *
     * Lets host-mouse cursor control (F3) actually select and interact, not
     * just move the cursor. Not gated on s_mouse_enabled: useful as a plain
     * extra binding regardless -- one hand on the mouse for pointing, click to
     * act, without reaching for the keyboard.
     *
     * Binding history, since it has moved twice on request: B originally, then
     * X after play-testing, now B for left with A added on right.
     *
     * Note these are the SERIAL-order pad bits (kPad_B = $0001, kPad_A =
     * $0100), not the $4218/$4219 hardware layout -- see
     * docs/HANDOVER_metal_marines.md #1. */
    { const uint32_t mb = SDL_GetMouseState(NULL, NULL);
      if (s_mouse_enabled && s_mouse_dir_frames > 0 &&
          (mb & SDL_BUTTON(SDL_BUTTON_LEFT))) {
        input |= s_mouse_dir;
        s_mouse_dir_frames--;
      } else if (s_mouse_dir_frames > 0) {
        s_mouse_dir_frames--;
      }
      if (mb & SDL_BUTTON(SDL_BUTTON_LEFT))  input |= kPad_B;
      /* Right button drives the map pan directly (see the pan block in the
       * mouse handler) rather than feeding A, so it does not also trigger the
       * ROM's own hold-A scroll and double up. */
      if (mb & SDL_BUTTON(SDL_BUTTON_RIGHT)) {
        input |= kPad_A;                      /* the game's own pan modifier */
        if (s_pan_dir_frames > 0) { input |= s_pan_dir; s_pan_dir_frames--; }
      } }
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
    /* DRAG TURBO: run extra guest frames while a mouse button is held.
     *
     * The cursor and the map scroll are not slow because their routines are
     * slow -- they are STARVED. Traced live: bank $03, the city simulation,
     * holds the CPU for ~4 consecutive frames at a time, and the bank-1 cursor
     * dispatcher does not run at all during those, so input steps only on the
     * bank-1 frames. A 4-on/4-off duty cycle. That is authentic behaviour, not
     * a recomp defect, and it is why the cartridge shipped with SNES Mouse
     * support.
     *
     * Nothing host-side can make the dispatcher run during a frame the ROM
     * spends elsewhere. What the host CAN do is give it more frames: running
     * N guest frames per host frame while dragging multiplies the number of
     * turns it gets, so bulldozing and panning proceed N times faster.
     *
     * The honest cost: the SIMULATION also advances N times faster while the
     * button is held. For a drag lasting a second or two that is a fraction of
     * a game-month, but it is not free, so it is off by default. */
    const bool dragging = s_drag_turbo > 1 &&
      (SDL_GetMouseState(NULL, NULL) &
       (SDL_BUTTON(SDL_BUTTON_LEFT) | SDL_BUTTON(SDL_BUTTON_RIGHT))) != 0;
    /* Map generation gets its own, much larger factor. 6x barely dents a wait
     * the player is staring at; the point is to collapse it, and nothing is
     * being watched while the generator runs. Tab-held fast-forward keeps its
     * modest 6x, since that IS being watched. */
    const bool generating = s_generating || s_gen_loop_active_frames > 0;
    bool fast_forward = keys[SDL_SCANCODE_TAB] || generating;
    int frames_this_iter = generating ? s_mapgen_turbo
                         : fast_forward ? 6
                         : (dragging ? s_drag_turbo : 1);

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
      if (!s_menu_open) audio_acc += (double)audio.freq / 60.0988;
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
        int qrc = sc_audio_queue(&audio, audio_buf, (Uint32)(wantN * 2 * sizeof(int16_t)));
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
                (unsigned long long)s_frames, sc_audio_queued(&audio),
                (unsigned long long)s_audio_dbg_queued, (unsigned long long)s_audio_dbg_calls,
                (unsigned long long)s_audio_dbg_fails);
      }
    }

    void *pixels = NULL; int pitch = 0;
    bool _lok = SDL_LockTexture(texture, NULL, &pixels, &pitch) SC_SDL_OK;
    /* Row-wise, NOT one memcpy of the whole array. s_video_pixels is sized for
     * the maximum widescreen width so the allocation never depends on the
     * runtime value -- copying sizeof() of it into a narrower texture would
     * both overrun the destination and misalign every row. */
    if (_lok && pixels) {
      const int row_bytes = s_video_w * 4;
      for (int y = 0; y < kVideoHeight; y++)
        memcpy((uint8_t *)pixels + (size_t)y * pitch,
               s_video_pixels + (size_t)y * s_video_pitch, (size_t)row_bytes);
    }
    SDL_UnlockTexture(texture);
    SDL_RenderClear(renderer);
    bool _cok = SDL_RenderCopy(renderer, texture, NULL, NULL) SC_SDL_OK;
    { static int diag = -1;
      if (diag < 0) diag = getenv("SC_SDL_DIAG") ? 0 : 99;
      if (diag < 99 && (s_frames % 60) == 0) {
        fprintf(stderr, "[sdl] lock=%d pitch=%d expect=%d copy=%d err=%s\n",
                (int)_lok, pitch, (int)s_video_pitch, (int)_cok, SDL_GetError()); } }
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
    /* SDL_RenderPresent returns void on SDL2 and bool on SDL3, so it cannot
     * share the SC_SDL_OK spelling with the other calls. */
#if SNESRECOMP_SDL3
    { bool _pok = SDL_RenderPresent(renderer);
#else
    { SDL_RenderPresent(renderer); bool _pok = true;
#endif
      static int pdiag = -1;
      if (pdiag < 0) pdiag = getenv("SC_SDL_DIAG") ? 0 : 99;
      if (pdiag < 99 && (s_frames % 60) == 0) {
        int ow = 0, oh = 0;
#if SNESRECOMP_SDL3
        SDL_GetRenderOutputSize(renderer, &ow, &oh);
#else
        SDL_GetRendererOutputSize(renderer, &ow, &oh);
#endif
        fprintf(stderr, "[sdl] present=%d out=%dx%d same_renderer=%d err=%s\n",
                (int)_pok, ow, oh,
                (int)(SDL_GetRenderer(window) == renderer), SDL_GetError());
      } }

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

  if (audio_dev) sc_audio_close(&audio);
  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  write_pc_bitmap_dump();
  write_map_trace_summary();
  { const char *p = getenv("SC_WRAM_MAP"); if (s_wram_map && p) write_wram_map(p); }
  /* Same SRAM dump the headless path does -- a real play session is the only
   * way to get SRAM with actual saved cities in it, so it is worth capturing
   * from the windowed exit too. */
  { const char *p = getenv("SC_SRAM_DUMP_PATH");
    if (p && *p) {
      report_sram_header("exit");
      fprintf(stderr, write_sram_dump(p) ? "dumped SRAM to %s\n"
                                         : "failed to write SRAM dump to %s\n", p);
    } }
  return 0;
}
