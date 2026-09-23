/* The title's SIMCITY sign, from the game's own animation.
 *
 * The sign is the lit billboard that crosses the title building, six 16x16
 * sprites in slots 96-101 making a 48x32 board. It is not a background tile
 * and not a fixed sprite: it is animation channel 0 of the player at COP
 * service 9 (00:8f82), and the title's phase routine 05:942e drives it. That
 * phase is the title's fifth ($30 = 4), the one the sequence settles into;
 * before it the channel holds nothing of the sign's, which is why the state
 * is only read while that phase is running.
 *
 *   $0277  its x, 10 bits. 05:9462 takes one off every frame, and the phase
 *          masks it: with bit 9 set it neither steps the channel nor emits
 *          anything. That is a pause in the drawing, not in the travel -- x
 *          keeps counting down, so 1023 carries on smoothly from 0 -- and the
 *          slots keep their last contents throughout, which is the copy that
 *          used to hang at the left edge. Between the DEC and the next
 *          frame's mask the word reads as $FFFF, so it is masked here too.
 *   $027F  its y (183 on the title; the record's sprites sit 48 and 64 above)
 *   $026F  the record the script is on, a word. $0267, the frames left on
 *          that step, and $026B, the offset into the script, are single
 *          bytes: 00:8f82 indexes those by channel and the rest by channel*2.
 *   The script is a list of (frames, record) pairs at $00:A123 ending in a 0
 *   that restarts it: $32 for $20 frames, $33 and $34 for $10 each, $35 for
 *   $20, then $2E and $35 alternating every 8 -- the lettering's blink.
 *
 * The emitter (COP 2, 00:8ea9) then writes the record's sprites at
 * ($0277 + dx, $027F + dy), taking x modulo 512 into the 9-bit OAM field.
 * Ten bits of travel through a nine-bit field is why the hardware cannot show
 * the whole crossing: while the sign is still approaching from beyond the
 * right edge its sprites read as far-left ones and the screen's edge hides
 * them, and where the game stops emitting they simply stay put.
 *
 * So the position is taken as the continuous one it really is -- x below 512
 * is its own number, x above is x - 1024, which continues left from 0 without
 * a step -- and the script is carried on here for exactly the frames the game
 * skips, by the same rule its own player uses. Without that the sign crossed
 * the margin with its lettering frozen on whichever step it had reached.
 *
 * All of it is taken once a frame, at the full NMI, with the shadow OAM: read
 * at any other point the sign is a pixel ahead of the game's own copy on some
 * frames and not on others, which reads as a shiver rather than a slide.
 *
 * The caller draws the sprites that fall outside the guest's 256 columns and
 * leaves the columns themselves to the game's own -- which are a frame behind
 * the skyline they hang over, and ScTitleSign_Lag() is what says so:
 *
 *   05:9448  COP #$09   runs the channel: the six sprites go to shadow OAM
 *   05:9460  INC $16    the skyline's scroll steps
 *   05:9462  DEC $0277  and the sign steps, in the same breath
 *
 * Both reach the PPU at the next NMI, but the sprites were written before
 * that pair and the scroll after it, so the board hangs one frame behind the
 * building it is bolted to. With the pan moving a pixel every second frame
 * ($2c bit 0) the two never step together: the skyline moves, the sign does
 * not, the sign moves, the skyline does not -- 1 px of shiver for the whole
 * crossing, on hardware as here. The caller puts the copy right. */
#include "sc_titlesign.h"

#include <stdio.h>
#include <stdlib.h>

enum {
  kTitleScreen = 0x01,
  kSignPhase = 4,        /* $30: the title phase that owns the channel */
  kPhase = 0x0030,
  kSignX = 0x0277,       /* animation channel 0's x, y and record */
  kSignY = 0x027f,
  kSignRecord = 0x026f,
  kSignFrames = 0x0267,  /* frames left on this step, one byte ... */
  kSignCursor = 0x026b,  /* ... and the offset into the script, one byte */
  kAway = 0x0200,        /* x bit 9: the game stops drawing it, not moving it */
  kScriptTable = 0x00a121,
};

/* The channel as of the last NMI, carried on from there for as long as the
 * game leaves it alone. */
static struct {
  bool live;
  int x;
  unsigned y, record, frames, cursor;
  int lag;              /* px the guest's own sprites are behind that x */
} s_sign;

static unsigned word(const uint8_t *ram, unsigned adr) {
  return ram[adr] | ((unsigned)ram[adr + 1] << 8);
}

/* One frame of 00:8f82 on our own copy of the channel. */
static void step(ScSelRomRead rd, void *ctx) {
  const unsigned table = rd(ctx, kScriptTable) | ((unsigned)rd(ctx, kScriptTable + 1) << 8);
  if (!s_sign.frames) {
    for (int guard = 0; guard < 64; guard++) {
      const unsigned frames = rd(ctx, table + s_sign.cursor);
      if (!frames) { s_sign.cursor = 0; continue; }   /* the closing 0 loops */
      s_sign.frames = frames;
      s_sign.record = rd(ctx, table + s_sign.cursor + 1);
      s_sign.cursor += 2;
      break;
    }
  }
  if (s_sign.frames) s_sign.frames--;
}

void ScTitleSign_Snapshot(const uint8_t *ram, ScSelRomRead rd, void *ctx) {
  if (!ram || ram[0x14] != kTitleScreen || word(ram, kPhase) != kSignPhase) {
    s_sign.live = false;
    return;
  }
  const unsigned x = word(ram, kSignX) & 0x3ffu;
  if (!(x & kAway)) {                 /* the game is running the channel */
    s_sign.frames = ram[kSignFrames];
    s_sign.cursor = ram[kSignCursor];
    s_sign.record = word(ram, kSignRecord);
  } else if (s_sign.live) {
    step(rd, ctx);                    /* the frames it skips are ours */
  } else {
    return;                           /* arrived mid-pause: nothing to show */
  }
  { const int was = s_sign.live ? s_sign.x : 0;
    const bool had = s_sign.live;
    s_sign.x = (x & kAway) ? (int)x - 1024 : (int)x;
    /* Emitted before the step, so a step means the sprites are behind it. */
    s_sign.lag = (!(x & kAway) && had && s_sign.x != was) ? 1 : 0; }
  s_sign.y = word(ram, kSignY);
  s_sign.live = true;
  { static int diag = -1;   /* SC_SIGN_DIAG=1: the channel, frame by frame */
    if (diag < 0) { const char *e = getenv("SC_SIGN_DIAG"); diag = e && *e && *e != '0'; }
    if (diag)
      fprintf(stderr, "[sign] x=%5d record $%02x frames %2u%s\n", s_sign.x,
              s_sign.record, s_sign.frames, (x & kAway) ? "  (ours)" : ""); }
}

int ScTitleSign_Sprites(ScSelSprite *out, int max, ScSelRomRead rd, void *ctx) {
  if (!out || max <= 0 || !s_sign.live) return 0;
  return ScSelector_Record(s_sign.record, s_sign.x, (int)s_sign.y, out, max, rd, ctx);
}

int ScTitleSign_Lag(void) { return s_sign.live ? s_sign.lag : 0; }
