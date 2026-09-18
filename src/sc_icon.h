/* The program's identity towards the desktop: its window icon, and on Linux
 * the application ID that ties the window to its desktop entry. */
#ifndef SC_ICON_H_INCLUDED
#define SC_ICON_H_INCLUDED

/* The ID the desktop entry, the hicolor icons and the window share
 * (CMakeLists.txt installs io.github.blackerking.UrbanRecomp.desktop). */
#define SC_APP_ID "io.github.blackerking.UrbanRecomp"

/* Call before the first window opens. On Linux it sets the X11 WM_CLASS and
 * the Wayland app_id to SC_APP_ID -- that is how a desktop finds the entry,
 * and with it the icon, for a running window. Variables set by hand win. */
void ScSetAppIdentity(void);

/* The Urban Recomp icon on a window, from pixels compiled into the program
 * (src/sc_icon_data.h), so it needs no file and no image decoder. Windows
 * also carries it as the executable's icon (src/urbanrecomp.rc). */
struct SDL_Window;
void ScSetWindowIcon(struct SDL_Window *window);

#endif /* SC_ICON_H_INCLUDED */
