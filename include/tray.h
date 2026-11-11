#ifndef DEBAR_TRAY_H
#define DEBAR_TRAY_H

#include <cairo.h>
#include <stdbool.h>

/* Dynamic system tray (StatusNotifierItem over D-Bus).
 *
 * tray_create() starts a background GLib main loop that talks to
 * org.kde.StatusNotifierWatcher. The bar never blocks on D-Bus: icons are
 * published through a small mutex-protected snapshot, and a self-pipe wakes
 * the Wayland poll loop when the list changes. */

/* Maximum number of icons the tray can display. */
#define DEBAR_TRAY_MAX_ITEMS 12

typedef struct tray tray_t;

/* Always returns a usable object (even when D-Bus is unavailable); the tray
 * simply stays empty in that case. */
tray_t *tray_create(void);
void tray_destroy(tray_t *tray);

/* Read end of the wake pipe. Readable when the tray contents changed.
 * Returns -1 when no tray is running. */
int tray_event_fd(const tray_t *tray);
void tray_drain_events(tray_t *tray);

/* Copies up to max icon surfaces (references added) into out and returns the
 * number of items. A NULL entry means the item has no resolvable icon.
 * The caller owns the references. */
int tray_snapshot(tray_t *tray, cairo_surface_t **out, int max);

/* Left click activates the item, middle click asks for the secondary
 * action. Runs asynchronously; never blocks the bar. */
void tray_activate(tray_t *tray, int index, bool secondary);

/* How many icons the tray currently holds (used for layout hit-testing). */
int tray_count(tray_t *tray);

#endif
