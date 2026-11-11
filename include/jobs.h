#ifndef DEBAR_JOBS_H
#define DEBAR_JOBS_H

#include <poll.h>
#include <stdbool.h>

/* Tiny async subprocess runner for the Wi-Fi / Bluetooth managers.
 *
 * Commands run without blocking the bar: their output is read from a pipe the
 * Wayland poll loop watches, and `done` is called on the main thread when the
 * process ends (or is killed after its timeout). No threads, no allocations
 * while idle. */

#define JOBS_MAX 4

/* ok = exited with status 0 and did not time out. output is stdout+stderr,
 * NUL-terminated, valid only during the call. */
typedef void (*job_done_fn)(void *user, bool ok, const char *output);

/* Starts argv (no shell is involved unless argv itself is `sh -c ...`).
 * Returns false when every slot is busy or the spawn failed; `done` is not
 * called in that case. */
bool jobs_start(char *const argv[], int timeout_ms, job_done_fn done,
                void *user);

/* Poll integration: jobs_fill() writes up to max pollfds, jobs_handle() must
 * be called with the same entries after poll() (also on timeout, it expires
 * overdue jobs). jobs_timeout_ms() is the longest poll may sleep, or -1. */
int jobs_fill(struct pollfd *fds, int max);
void jobs_handle(const struct pollfd *fds, int count);
int jobs_timeout_ms(void);

bool jobs_running(void);

#endif
