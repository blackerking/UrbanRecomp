/* The title's SIMCITY sign -- see sc_titlesign.c. */
#ifndef SC_TITLESIGN_H_INCLUDED
#define SC_TITLESIGN_H_INCLUDED

#include "sc_selector.h"   /* ScSelSprite, ScSelRomRead: the same records */

enum { SC_SIGN_MAX_SPRITES = 8 };

/* Take the sign's state, at the full NMI (00:80c0) -- the moment the shadow
 * OAM goes out, so what a renderer draws in a margin and what the game draws
 * in its own columns are the same frame of its travel. */
void ScTitleSign_Snapshot(const uint8_t *ram, ScSelRomRead rd, void *ctx);

/* The sign's sprites for the frame that snapshot belongs to, at their true
 * positions -- none while the game is not showing it. */
int ScTitleSign_Sprites(ScSelSprite *out, int max, ScSelRomRead rd, void *ctx);

/* How far the guest's own OAM copy is behind those, in pixels: 1 on a frame
 * the sign stepped, 0 otherwise. The game writes the sprites before it moves
 * the scene, so its copy is a step behind the scroll it is drawn against. */
int ScTitleSign_Lag(void);

/* How far the guest's own OAM copy is behind those, in pixels: 1 on a frame
 * the sign stepped, 0 otherwise. The game writes the sprites before it moves
 * the scene, so its copy is a step behind the scroll it is drawn against. */
int ScTitleSign_Lag(void);

#endif
