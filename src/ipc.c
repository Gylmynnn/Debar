#include "ipc.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define I3_MAGIC "i3-ipc"
#define IPC_EVENT_FLAG 0x80000000u

static ipc_compositor_t g_comp = IPC_NONE;
static int g_event_fd = -1;

/* Routine IPC chatter is hidden unless DEBAR_DEBUG is set; warnings (no
 * compositor detected, unusable sockets) still go to stderr. */
static bool ipc_debug(void) {
  static int enabled = -1;
  if (enabled < 0) {
    enabled = getenv("DEBAR_DEBUG") != NULL;
  }
  return enabled != 0;
}

static char g_sway_sock[PATH_MAX];

/* Hyprland socket1: newline-delimited event stream. */
static char g_hypr_line[8192];
static size_t g_hypr_fill = 0;

/* Sway persistent connection buffers received i3-ipc frames. */
static unsigned char g_sway_rx[65536];
static size_t g_sway_fill = 0;

struct sway_frame {
  uint32_t type;
  uint32_t len;
  char payload[9216];
};

/* ---------------- small helpers ------------------------------------------ */

/* Bounded string copy. Unlike strncpy it always terminates the destination
 * and does not trigger truncation warnings for paths that may be longer than
 * sockaddr_un.sun_path. */
static void copy_path(char *dst, size_t cap, const char *src) {
  if (cap == 0) {
    return;
  }
  const size_t len = strlen(src);
  const size_t n = len < cap ? len : cap - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static int connect_unix(const char *path) {
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (strlen(path) >= sizeof(addr.sun_path)) {
    return -1;
  }
  copy_path(addr.sun_path, sizeof(addr.sun_path), path);

  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static bool write_all(int fd, const char *buf, size_t len) {
  size_t done = 0;
  while (done < len) {
    const ssize_t k = write(fd, buf + done, len - done);
    if (k <= 0) {
      return false;
    }
    done += (size_t)k;
  }
  return true;
}

/* Reads everything available within an idle window, then returns. */
static void read_idle(int fd, char *buf, size_t nbuf) {
  size_t len = 0;
  buf[0] = '\0';
  for (;;) {
    struct pollfd p = {.fd = fd, .events = POLLIN, .revents = 0};
    if (poll(&p, 1, 250) <= 0 || !(p.revents & POLLIN)) {
      break;
    }
    const ssize_t k = read(fd, buf + len, nbuf - len - 1);
    if (k <= 0) {
      break;
    }
    len += (size_t)k;
    if (len >= nbuf - 1) {
      break;
    }
  }
  buf[len] = '\0';
}

static bool json_int(const char *json, const char *key, int *out) {
  char needle[48];
  snprintf(needle, sizeof(needle), "\"%s\":", key);
  const char *p = strstr(json, needle);
  if (p == NULL) {
    return false;
  }
  p += strlen(needle);
  while (isspace((unsigned char)*p)) {
    p++;
  }
  return sscanf(p, "%d", out) == 1;
}

/* Collects every "key":<int> into out. Returns how many were found. */
static int json_all_ints(const char *json, const char *key, int *out,
                         int outcap) {
  char needle[48];
  snprintf(needle, sizeof(needle), "\"%s\":", key);
  int n = 0;
  const char *p = json;
  while (n < outcap && (p = strstr(p, needle)) != NULL) {
    p += strlen(needle);
    while (isspace((unsigned char)*p)) {
      p++;
    }
    if (sscanf(p, "%d", &out[n]) == 1) {
      n++;
    }
    p++;
  }
  return n;
}

static int clamp_ws(modules_data_t *d, int value) {
  const int count = d->ws_count > 0 ? d->ws_count : 6;
  if (value < 0) {
    return 0;
  }
  return value >= count ? count - 1 : value;
}

/* After debar switches workspaces itself, the 1-second poll can briefly
 * report the stale pre-switch active workspace. During this grace window
 * such stale reports are ignored; the first report matching the switch
 * target confirms it and ends the grace early. (A keybind switch made
 * during the window shows up at most 1.5 s later - no bounce.) */
#define SWITCH_GRACE_MS 1500
static long g_grace_until_ms = 0;
static int g_grace_target = -1;

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Applies a poll-reported active workspace (0-based), respecting the
 * post-switch grace window. Event-driven paths bypass this - they are
 * authoritative compositor pushes, not polls. */
static void apply_active(modules_data_t *d, int active0) {
  if (now_ms() < g_grace_until_ms) {
    if (active0 != g_grace_target) {
      return; /* stale pre-switch report; keep the optimistic value */
    }
    g_grace_until_ms = 0; /* confirmed by the compositor */
  }
  g_grace_until_ms = 0;
  d->ws_active = active0;
}

/* ---------------- Hyprland ------------------------------------------------ */

static char g_hypr_cmd_path[PATH_MAX + 1024]; /* current command socket */

/* Index into the dispatch forms[] of the syntax this build accepted,
 * found by hypr_probe_syntax() at startup (or the first click). */
static int g_hypr_form = -1;

/* All socket candidates in the Hyprland session dir, tried in order when a
 * dispatch seems to be ignored (socket semantics differ across versions). */
#define HYPR_MAX_SOCKETS 5
static char g_hypr_sockets[HYPR_MAX_SOCKETS][PATH_MAX + 1024];
static int g_hypr_socket_n = 0;

static void hypr_record_socket(const char *path) {
  for (int i = 0; i < g_hypr_socket_n; i++) {
    if (strcmp(g_hypr_sockets[i], path) == 0) {
      return; /* dedupe */
    }
  }
  if (g_hypr_socket_n >= HYPR_MAX_SOCKETS) {
    return;
  }
  copy_path(g_hypr_sockets[g_hypr_socket_n], sizeof(g_hypr_sockets[0]), path);
  g_hypr_socket_n++;
}

static void hypr_request(const char *req, char *buf, size_t nbuf) {
  const int fd = connect_unix(g_hypr_cmd_path);
  if (fd < 0) {
    buf[0] = '\0';
    return;
  }
  if (write_all(fd, req, strlen(req))) {
    read_idle(fd, buf, nbuf);
  } else {
    buf[0] = '\0';
  }
  close(fd);
}

void hypr_query_workspaces(modules_data_t *d) {
  char buf[32768];
  hypr_request("j/workspaces", buf, sizeof(buf));

  int ids[64];
  const int n = json_all_ints(buf, "id", ids, 64);
  if (n < 1) {
    return; /* keep the decorative default */
  }
  /* Window counts line up with the ids above. When the field is missing
   * (older builds) fall back to "listed = occupied". */
  int wins[64];
  const int wn = json_all_ints(buf, "windows", wins, 64);

  int max = 1;
  d->ws_occupied = 0;
  for (int i = 0; i < n; i++) {
    if (ids[i] > max) {
      max = ids[i];
    }
    /* "occupied" means a program is open there: the active-but-empty
     * workspace is listed too and must stay an outline ghost. */
    const bool has_windows = i >= wn || wins[i] > 0;
    if (has_windows && ids[i] >= 1 && ids[i] <= 32) {
      d->ws_occupied |= 1u << (ids[i] - 1);
    }
  }
  d->ws_count = max;

  hypr_request("j/activeworkspace", buf, sizeof(buf));
  int active = 0;
  if (json_int(buf, "id", &active) && active >= 1) {
    apply_active(d, active - 1);
  } else {
    d->ws_active = clamp_ws(d, d->ws_active);
  }
}

/* Handles one newline-terminated socket1 event line. Occupancy is left to
 * hypr_query_workspaces() (polled): an event only says a workspace became
 * active or was created, not that a program is open on it. */
static void hypr_handle_event(modules_data_t *d, const char *line) {
  int id = 0;
  if (strncmp(line, "workspace>>", 11) == 0) {
    if (sscanf(line + 11, "%d", &id) == 1 && id >= 1) {
      d->ws_active = id - 1; /* pacman follows the active workspace */
    }
  } else if (strncmp(line, "createworkspace>>", 17) == 0) {
    if (sscanf(line + 17, "%d", &id) == 1 && id >= 1) {
      if (id > d->ws_count) {
        d->ws_count = id; /* a visible slot, still possibly empty */
      }
    }
  } else if (strncmp(line, "destroyworkspace>>", 18) == 0) {
    if (sscanf(line + 18, "%d", &id) == 1 && id >= 1 && id <= 32) {
      d->ws_occupied &= ~(1u << (id - 1));
    }
    if (d->ws_count > 1) {
      d->ws_count--;
      d->ws_active = clamp_ws(d, d->ws_active);
    }
  }
}

/* ---------------- Sway (i3-ipc over $SWAYSOCK) ---------------------------- */

static bool sway_send(int fd, uint32_t type, const char *payload) {
  const uint32_t len = (uint32_t)strlen(payload);
  unsigned char hdr[14];
  memcpy(hdr, I3_MAGIC, 6);
  memcpy(hdr + 6, &len, 4);
  memcpy(hdr + 10, &type, 4);

  if (!write_all(fd, (const char *)hdr, sizeof(hdr))) {
    return false;
  }
  return write_all(fd, payload, len);
}

static bool read_full(int fd, unsigned char *buf, size_t n) {
  size_t done = 0;
  while (done < n) {
    const ssize_t k = read(fd, buf + done, n - done);
    if (k <= 0) {
      return false;
    }
    done += (size_t)k;
  }
  return true;
}

static int sway_simple(uint32_t type, const char *payload, uint32_t reply_type,
                       char *out, size_t nout) {
  const int fd = connect_unix(g_sway_sock);
  if (fd < 0) {
    return -1;
  }

  int result = -1;
  if (sway_send(fd, type, payload)) {
    unsigned char hdr[14];
    if (read_full(fd, hdr, sizeof(hdr)) && memcmp(hdr, I3_MAGIC, 6) == 0) {
      uint32_t len = 0;
      uint32_t rtype = 0;
      memcpy(&len, hdr + 6, 4);
      memcpy(&rtype, hdr + 10, 4);
      if (len <= sizeof(g_sway_rx) - 16) {
        unsigned char body[65536];
        if (read_full(fd, body, len)) {
          /* command/subscribe replies carry one extra success byte */
          if (rtype == 0 || rtype == 2) {
            unsigned char flag = 0;
            read_full(fd, &flag, 1);
          }
          if (rtype == reply_type) {
            const size_t n = len < nout - 1 ? len : nout - 1;
            memcpy(out, body, n);
            out[n] = '\0';
            result = 0;
          }
        }
      }
    }
  }

  close(fd);
  return result;
}

void sway_parse_list(const char *json, modules_data_t *d) {
  int max = 0;
  int active = -1;
  uint32_t occupied = 0;

  const char *p = json;
  while ((p = strstr(p, "\"num\":")) != NULL) {
    p += 6;
    int num = -1;
    if (sscanf(p, "%d", &num) == 1 && num >= 1) {
      if (num > max) {
        max = num;
      }
      occupied |= 1u << (num - 1);

      /* "focused" appears in the same object, before the next "num" */
      const char *next = strstr(p, "\"num\":");
      const char *end = next != NULL ? next : p + strlen(p);
      if (end - p < 256) {
        char segment[256];
        const size_t n = (size_t)(end - p);
        memcpy(segment, p, n);
        segment[n] = '\0';
        if (strstr(segment, "\"focused\":") != NULL) {
          const char *fv = strstr(segment, "\"focused\":");
          fv += 10;
          while (isspace((unsigned char)*fv)) {
            fv++;
          }
          if (strncmp(fv, "true", 4) == 0) {
            active = num;
          }
        }
      }
      if (next == NULL) {
        break;
      }
      p = next;
    } else {
      p++;
    }
  }

  if (max > 0) {
    d->ws_count = max;
    d->ws_occupied = occupied;
  }
  if (active >= 1) {
    apply_active(d, active - 1);
  } else {
    d->ws_active = clamp_ws(d, d->ws_active);
  }
}

static void sway_parse_event(modules_data_t *d, const char *payload) {
  char change[16] = "";
  const char *c = strstr(payload, "\"change\":\"");
  if (c != NULL) {
    c += 10;
    size_t i = 0;
    while (c[i] != '\0' && c[i] != '"' && i + 1 < sizeof(change)) {
      change[i] = c[i];
      i++;
    }
    change[i] = '\0';
  }

  int num = -1;
  json_int(payload, "num", &num);

  if (strcmp(change, "focus") == 0 && num >= 1) {
    d->ws_active = num - 1;
    if (num <= 32) {
      d->ws_occupied |= 1u << (num - 1);
    }
  } else if (strcmp(change, "init") == 0 && num >= 1) {
    if (num > d->ws_count) {
      d->ws_count = num;
    }
    if (num <= 32) {
      d->ws_occupied |= 1u << (num - 1);
    }
  } else if (strcmp(change, "empty") == 0 || strcmp(change, "close") == 0) {
    if (d->ws_count > 1) {
      d->ws_count--;
    }
    d->ws_occupied &=
        (d->ws_count < 32) ? (1u << d->ws_count) - 1u : 0xffffffffu;
    d->ws_active = clamp_ws(d, d->ws_active);
  }
}

void sway_subscribe(void) {
  char buf[64];
  sway_simple(2 /* SUBSCRIBE */, "[\"workspace\"]", 2, buf, sizeof(buf));
}

static void sway_handle_stream(modules_data_t *d) {
  unsigned char chunk[4096];
  for (;;) {
    const ssize_t k = recv(g_event_fd, chunk, sizeof(chunk), MSG_DONTWAIT);
    if (k <= 0) {
      break;
    }
    if (g_sway_fill + (size_t)k <= sizeof(g_sway_rx)) {
      memcpy(g_sway_rx + g_sway_fill, chunk, (size_t)k);
      g_sway_fill += (size_t)k;
    }
  }

  for (;;) {
    if (g_sway_fill < 14) {
      break;
    }
    if (memcmp(g_sway_rx, I3_MAGIC, 6) != 0) {
      /* resynchronize by dropping a single byte */
      memmove(g_sway_rx, g_sway_rx + 1, g_sway_fill - 1);
      g_sway_fill--;
      continue;
    }

    uint32_t len = 0;
    uint32_t type = 0;
    memcpy(&len, g_sway_rx + 6, 4);
    memcpy(&type, g_sway_rx + 10, 4);
    if (14 + (size_t)len > sizeof(g_sway_rx) ||
        g_sway_fill < 14 + (size_t)len) {
      break; /* wait for the rest of the frame */
    }

    struct sway_frame f;
    f.type = type;
    f.len = len;
    size_t n = len < sizeof(f.payload) - 1 ? len : sizeof(f.payload) - 1;
    memcpy(f.payload, g_sway_rx + 14, n);
    f.payload[n] = '\0';

    const size_t frame = 14 + (size_t)len;
    memmove(g_sway_rx, g_sway_rx + frame, g_sway_fill - frame);
    g_sway_fill -= frame;

    if ((type & IPC_EVENT_FLAG) != 0) {
      /* only "workspace" is subscribed; payload describes the change */
      sway_parse_event(d, f.payload);
    }
  }
}

/* ---------------- compositor discovery ------------------------------------ */

/* Probes a socket: does it answer hyprctl-style JSON queries? */
static bool hypr_probe_command(const char *path) {
  const int fd = connect_unix(path);
  if (fd < 0) {
    return false;
  }
  bool ok = false;
  if (write_all(fd, "j/activeworkspace", strlen("j/activeworkspace"))) {
    char buf[512];
    read_idle(fd, buf, sizeof(buf));
    ok = strstr(buf, "\"id\"") != NULL;
  }
  close(fd);
  return ok;
}

/* Locates the Hyprland session dir, identifies the command socket, and
 * connects the event socket when present. Falls back gracefully: without
 * the event socket, keep pointing to the command socket (poll mode). */
static bool hypr_discover(struct ipc_diag *diag) {
  char session[PATH_MAX + 512] = "";
  const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
  const char *xdg = getenv("XDG_RUNTIME_DIR");

  if (sig != NULL && xdg != NULL) {
    snprintf(session, sizeof(session), "%s/hypr/%.300s", xdg, sig);
  } else if (xdg != NULL) {
    char base[PATH_MAX];
    snprintf(base, sizeof(base), "%s/hypr", xdg);
    DIR *dir = opendir(base);
    if (dir != NULL) {
      struct dirent *entry;
      while (session[0] == '\0' && (entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
          continue;
        }
        char probe[PATH_MAX + 512];
        snprintf(probe, sizeof(probe), "%s/%.240s/.socket2.sock", base,
                 entry->d_name);
        if (access(probe, F_OK) == 0) {
          snprintf(session, sizeof(session), "%s/%.400s", base, entry->d_name);
        }
      }
      closedir(dir);
    }
  }
  if (session[0] == '\0') {
    return false;
  }

  /* command socket: .socket2.sock preferred, then .socket.sock, then any */
  static const char *const prefs[] = {".socket2.sock", ".socket.sock"};
  for (size_t i = 0; i < sizeof(prefs) / sizeof(prefs[0]); i++) {
    char path[PATH_MAX + 1024];
    snprintf(path, sizeof(path), "%s/%.440s", session, prefs[i]);
    hypr_record_socket(path); /* candidate for dispatch, tried in order */
    if (access(path, F_OK) == 0 && hypr_probe_command(path)) {
      snprintf(diag->cmd_socket, sizeof(diag->cmd_socket), "%s", prefs[i]);
      copy_path(g_hypr_cmd_path, sizeof(g_hypr_cmd_path), path);
      break;
    }
  }
  if (g_hypr_cmd_path[0] == '\0') {
    /* try any socket file as a last resort */
    DIR *dir = opendir(session);
    if (dir != NULL) {
      struct dirent *entry;
      while (g_hypr_cmd_path[0] == '\0' && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
          continue;
        }
        char path[PATH_MAX + 1024];
        snprintf(path, sizeof(path), "%s/%.240s", session, entry->d_name);
        hypr_record_socket(path);
        if (hypr_probe_command(path)) {
          snprintf(diag->cmd_socket, sizeof(diag->cmd_socket), "%.48s",
                   entry->d_name);
          copy_path(g_hypr_cmd_path, sizeof(g_hypr_cmd_path), path);
        }
      }
      closedir(dir);
    }
  }
  if (g_hypr_cmd_path[0] == '\0') {
    /* list what was there, to help the user report the correct names */
    DIR *dir = opendir(session);
    if (dir != NULL && ipc_debug()) {
      struct dirent *entry;
      fprintf(stderr, "debar: hyprland session dir contains:");
      bool first = true;
      while ((entry = readdir(dir)) != NULL) {
        fprintf(stderr, "%s%s%s", first ? " " : " ", entry->d_name, ",");
        first = false;
      }
      fprintf(stderr, "\n");
      closedir(dir);
    }
    return false;
  }

  /* optional event socket */
  char event_path[PATH_MAX + 1024];
  snprintf(event_path, sizeof(event_path), "%s/.socket1.sock", session);
  g_event_fd = connect_unix(event_path);
  snprintf(diag->event_socket, sizeof(diag->event_socket),
           g_event_fd >= 0 ? ".socket1.sock" : "none (polling)");

  return true;
}

static bool discover_sway_sock(char *out, size_t nout) {
  const char *xdg = getenv("XDG_RUNTIME_DIR");
  if (xdg == NULL) {
    return false;
  }

  DIR *dir = opendir(xdg);
  if (dir == NULL) {
    return false;
  }

  struct dirent *entry;
  bool found = false;
  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "sway-ipc.", 9) != 0) {
      continue;
    }
    snprintf(out, nout, "%s/%s", xdg, entry->d_name);
    found = true;
    break;
  }
  closedir(dir);
  return found;
}

/* ---------------- public API ---------------------------------------------- */

static void hypr_probe_syntax(void);

ipc_compositor_t ipc_init(void) {
  struct ipc_diag diag;
  memset(&diag, 0, sizeof(diag));

  if (hypr_discover(&diag)) {
    g_comp = IPC_HYPRLAND;
    hypr_probe_syntax();
  } else if (g_hypr_cmd_path[0] != '\0') {
    /* unreachable, defensive */
    g_comp = IPC_HYPRLAND;
  }

  const char *sway = getenv("SWAYSOCK");
  if (g_comp == IPC_NONE && sway != NULL) {
    copy_path(g_sway_sock, sizeof(g_sway_sock), sway);
    g_event_fd = connect_unix(g_sway_sock);
    if (g_event_fd >= 0) {
      sway_subscribe();
      g_comp = IPC_SWAY;
    }
  }
  if (g_comp == IPC_NONE) {
    /* last resort: scan for a sway socket */
    char found[PATH_MAX];
    if (discover_sway_sock(found, sizeof(found))) {
      copy_path(g_sway_sock, sizeof(g_sway_sock), found);
      g_event_fd = connect_unix(g_sway_sock);
      if (g_event_fd >= 0) {
        sway_subscribe();
        g_comp = IPC_SWAY;
      }
    }
  }

  if (g_comp == IPC_NONE) {
    fprintf(stderr,
            "debar: no compositor IPC detected; workspaces stay decorative\n");
  } else if (ipc_debug()) {
    if (g_comp == IPC_HYPRLAND) {
      fprintf(stderr, "debar: hyprland IPC cmd=%s events=%s\n", diag.cmd_socket,
              diag.event_socket);
    } else {
      fprintf(stderr, "debar: compositor IPC: sway\n");
    }
  }
  return g_comp;
}

void ipc_query_workspaces(modules_data_t *d) {
  if (d == NULL) {
    return;
  }
  if (g_comp == IPC_HYPRLAND) {
    hypr_query_workspaces(d);
  } else if (g_comp == IPC_SWAY) {
    char buf[32768];
    if (sway_simple(1 /* GET_WORKSPACES */, "", 1, buf, sizeof(buf)) == 0) {
      sway_parse_list(buf, d);
    }
  }
}

int ipc_event_fd(void) { return g_event_fd; }

void ipc_handle_events(modules_data_t *d) {
  if (d == NULL || g_event_fd < 0) {
    return;
  }

  if (g_comp == IPC_HYPRLAND) {
    unsigned char chunk[4096];
    for (;;) {
      const ssize_t k = recv(g_event_fd, chunk, sizeof(chunk), MSG_DONTWAIT);
      if (k <= 0) {
        break;
      }
      if (g_hypr_fill + (size_t)k >= sizeof(g_hypr_line)) {
        g_hypr_fill = 0; /* overflow safety: drop the line */
        continue;
      }
      memcpy(g_hypr_line + g_hypr_fill, chunk, (size_t)k);
      g_hypr_fill += (size_t)k;

      char *start = g_hypr_line;
      char *end = g_hypr_line + g_hypr_fill;
      char *nl;
      while ((nl = memchr(start, '\n', (size_t)(end - start))) != NULL) {
        *nl = '\0';
        hypr_handle_event(d, start);
        start = nl + 1;
      }
      g_hypr_fill = (size_t)(end - start);
      memmove(g_hypr_line, start, g_hypr_fill);
    }
  } else if (g_comp == IPC_SWAY) {
    sway_handle_stream(d);
  }
}

/* Result of a single dispatch attempt. */
#define HYPR_TRY_DEAD 0     /* socket unusable, try the next socket */
#define HYPR_TRY_REJECTED 1 /* syntax not understood, try the next form */
#define HYPR_TRY_OK 2       /* accepted by the compositor */

/* Sends one dispatch attempt and classifies the response. Errors like
 * "error: ... expected a dispatcher", "unknown request" or "invalid" mean
 * this build does not speak this syntax yet. With verbose=false (startup
 * probing) nothing is printed. */
static int hypr_send_form(const char *path, const char *form, int workspace,
                          bool verbose) {
  char cmd[128];
  const int len = snprintf(cmd, sizeof(cmd), form, workspace);

  const int fd = connect_unix(path);
  if (fd < 0) {
    if (verbose && ipc_debug()) {
      fprintf(stderr, "debar: dispatch: %s connect failed\n", path);
    }
    return HYPR_TRY_DEAD;
  }
  const bool written = write_all(fd, cmd, (size_t)len);
  char resp[256];
  resp[0] = '\0';
  if (written) {
    read_idle(fd, resp, sizeof(resp));
  }
  close(fd);

  if (!written) {
    if (verbose && ipc_debug()) {
      fprintf(stderr, "debar: dispatch via %s -> write failed\n", path);
    }
    return HYPR_TRY_DEAD;
  }
  if (resp[0] == '\0') {
    if (verbose && ipc_debug()) {
      fprintf(stderr, "debar: dispatch via %s -> no response\n", path);
    }
    return HYPR_TRY_REJECTED;
  }
  if (strncmp(resp, "error", 5) == 0 ||
      strncmp(resp, "unknown request", 15) == 0 ||
      strncmp(resp, "invalid", 7) == 0) {
    if (verbose && ipc_debug()) {
      fprintf(stderr, "debar: dispatch via %s -> rejected (%.80s)\n", path,
              resp);
    }
    return HYPR_TRY_REJECTED;
  }

  if (verbose && ipc_debug()) {
    fprintf(stderr, "debar: dispatch via %s -> '%s'\n", path, resp);
  }
  return HYPR_TRY_OK;
}

/* Once, right after discovery: figures out which dispatch syntax this
 * compositor speaks, using harmless no-op dispatchers ("do nothing"), so
 * the first click switches instantly instead of probing with logging. */
static void hypr_probe_syntax(void) {
  static const char *const probes[] = {
      "dispatch hl.dsp.no_op()\n", /* Lua config >= 0.55 -> form 0 */
      "dispatch nop\n",            /* hyprlang <= 0.54     -> form 1 */
  };
  for (size_t p = 0; p < sizeof(probes) / sizeof(probes[0]); p++) {
    for (int i = 0; i < g_hypr_socket_n; i++) {
      if (hypr_send_form(g_hypr_sockets[i], probes[p], 1, false) ==
          HYPR_TRY_OK) {
        g_hypr_form = (int)p;
        /* future commands go to the socket that actually answers */
        if (strcmp(g_hypr_cmd_path, g_hypr_sockets[i]) != 0) {
          copy_path(g_hypr_cmd_path, sizeof(g_hypr_cmd_path),
                    g_hypr_sockets[i]);
        }
        if (ipc_debug()) {
          fprintf(stderr, "debar: dispatch form %zd: %.40s\n", p, probes[p]);
        }
        return;
      }
    }
  }
  if (ipc_debug()) {
    fprintf(stderr, "debar: dispatch form unknown yet (probe on first click)\n");
  }
}

bool ipc_switch_workspace(modules_data_t *d, int workspace) {
  if (d == NULL || workspace < 1) {
    return false;
  }

  bool sent = false;
  if (g_comp == IPC_HYPRLAND) {
    /* Command syntax differs across Hyprland versions. Since 0.55 the
     * config is Lua and .socket.sock wraps requests as
     * `return hl.dispatch(<text>)`; dispatchers are closures built with
     * hl.dsp.* (a plain string fails: "expected a dispatcher"). The
     * workspace must be a numeric ID - string names silently no-op.
     * Older hyprlang-era builds take the classic plain form. */
    static const char *const forms[] = {
        "dispatch hl.dsp.focus({ workspace = %d })\n", /* Lua config >= 0.55 */
        "dispatch workspace %d\n",                     /* hyprlang <= 0.54 */
        "dispatch \"workspace\", \"%d\"\n",            /* legacy hybrid */
    };
    const size_t nforms = sizeof(forms) / sizeof(forms[0]);

    /* Fast path: reuse the form + socket learned at startup (or an
     * earlier click), with no trial-and-error delay. */
    if (g_hypr_form >= 0) {
      if (hypr_send_form(g_hypr_cmd_path, forms[g_hypr_form], workspace,
                         true) == HYPR_TRY_OK) {
        sent = true;
      } else {
        g_hypr_form = -1; /* stale; probe again this round */
      }
    }

    for (int i = 0; i < g_hypr_socket_n && !sent; i++) {
      const char *path = g_hypr_sockets[i];
      for (size_t f = 0; f < nforms && !sent; f++) {
        const int rc = hypr_send_form(path, forms[f], workspace, true);
        if (rc == HYPR_TRY_OK) {
          /* use whatever socket actually answers for future commands */
          if (strcmp(g_hypr_cmd_path, path) != 0) {
            copy_path(g_hypr_cmd_path, sizeof(g_hypr_cmd_path), path);
          }
          g_hypr_form = (int)f;
          sent = true;
        } else if (rc == HYPR_TRY_DEAD) {
          break; /* socket dead, try the next candidate */
        }
      }
    }

    if (!sent && g_hypr_socket_n == 0) {
      fprintf(stderr, "debar: no hyprland socket candidates\n");
    }
  } else if (g_comp == IPC_SWAY) {
    const int fd = connect_unix(g_sway_sock);
    if (fd >= 0) {
      char cmd[64];
      snprintf(cmd, sizeof(cmd), "workspace number %d", workspace);
      sent = sway_send(fd, 0 /* RUN_COMMAND */, cmd);
      close(fd);
      if (ipc_debug()) {
        fprintf(stderr, "debar: sway '%s' -> %s\n", cmd,
                sent ? "sent" : "write failed");
      }
    }
  } else {
    return false;
  }

  if (sent) {
    /* Optimistic update: move the pacman to the target immediately, but do
     * NOT mark it occupied - switching to an empty workspace must leave an
     * outline ghost. Deliberately not clamped against ws_count either:
     * clicking an empty slot beyond the current count is valid (the
     * compositor creates it). apply_active() shields the value from stale
     * polls until the compositor confirms. */
    g_grace_target = workspace - 1;
    g_grace_until_ms = now_ms() + SWITCH_GRACE_MS;
    d->ws_active = workspace - 1;
  }
  return sent;
}

void ipc_finish(void) {
  if (g_event_fd >= 0) {
    close(g_event_fd);
    g_event_fd = -1;
  }
  g_comp = IPC_NONE;
  g_hypr_fill = 0;
  g_sway_fill = 0;
}
