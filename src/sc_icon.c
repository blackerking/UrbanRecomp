/* Window icon and application ID -- see sc_icon.h. */
#include "sc_icon.h"

#include <stdlib.h>

#include "sc_sdl_compat.h"
#include "sc_icon_data.h"

#ifndef SC_VERSION
#define SC_VERSION "dev"
#endif

void ScSetAppIdentity(void) {
#if SNESRECOMP_SDL3
  /* SDL3 derives the Wayland app_id and the X11 class from the identifier. */
  SDL_SetAppMetadata("Urban Recomp", SC_VERSION, SC_APP_ID);
#else
  SDL_SetHint(SDL_HINT_APP_NAME, "Urban Recomp");
#ifndef _WIN32
  /* SDL2 reads the window class from the environment: X11 from
   * SDL_VIDEO_X11_WMCLASS, Wayland (2.0.22+) from SDL_VIDEO_WAYLAND_WMCLASS.
   * Without them it takes the executable's file name. */
  setenv("SDL_VIDEO_X11_WMCLASS", SC_APP_ID, 0);
  setenv("SDL_VIDEO_WAYLAND_WMCLASS", SC_APP_ID, 0);
#endif
#endif
}

void ScSetWindowIcon(SDL_Window *window) {
  if (!window) return;
#if SNESRECOMP_SDL3
  SDL_Surface *surf = SDL_CreateSurfaceFrom(SC_ICON_W, SC_ICON_H, SDL_PIXELFORMAT_RGBA32,
                                            (void *)kScIconRgba, SC_ICON_W * 4);
  if (surf) { SDL_SetWindowIcon(window, surf); SDL_DestroySurface(surf); }
#else
  SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(
      (void *)kScIconRgba, SC_ICON_W, SC_ICON_H, 32, SC_ICON_W * 4, SDL_PIXELFORMAT_RGBA32);
  if (surf) { SDL_SetWindowIcon(window, surf); SDL_FreeSurface(surf); }
#endif
}
