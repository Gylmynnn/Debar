#define _GNU_SOURCE
#include "netmgr.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "jobs.h"

#define REFRESH_INTERVAL_MS 8000
#define LIST_TIMEOUT_MS 8000
#define ACTION_TIMEOUT_MS 20000
#define CONNECT_TIMEOUT_MS 45000

static netmgr_wifi_t g_wifi;
static netmgr_bt_t g_bt;
static bool g_dirty = false;
static long g_wifi_due_ms = 0;
static long g_bt_due_ms = 0;

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

const netmgr_wifi_t *netmgr_wifi(void) { return &g_wifi; }
const netmgr_bt_t *netmgr_bt(void) { return &g_bt; }

int netmgr_wifi_icon_state(void) {
  if (!g_wifi.loaded || !g_wifi.available) {
    return 0;
  }
  return g_wifi.enabled ? 2 : 1;
}

int netmgr_bt_icon_state(void) {
  if (!g_bt.loaded || !g_bt.available) {
    return 0;
  }
  return g_bt.powered ? 2 : 1;
}

bool netmgr_take_dirty(void) {
  const bool was = g_dirty;
  g_dirty = false;
  return was;
}

static void copy_str(char *dst, size_t size, const char *src) {
  if (size == 0) {
    return;
  }
  if (src == NULL) {
    src = "";
  }
  const size_t len = strlen(src);
  const size_t n = len < size ? len : size - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static void set_wifi_status(const char *text, bool error) {
  copy_str(g_wifi.status, sizeof(g_wifi.status), text);
  g_wifi.status_error = error;
  g_dirty = true;
}

static void set_bt_status(const char *text, bool error) {
  copy_str(g_bt.status, sizeof(g_bt.status), text);
  g_bt.status_error = error;
  g_dirty = true;
}

/* First meaningful line of command output, without a leading "Error: ". */
static void first_line(const char *out, char *dst, size_t size) {
  dst[0] = '\0';
  while (*out != '\0') {
    while (*out == '\n' || *out == '\r' || *out == ' ' || *out == '\t') {
      out++;
    }
    const char *end = out;
    while (*end != '\0' && *end != '\n' && *end != '\r') {
      end++;
    }
    if (end > out) {
      if (strncmp(out, "Error: ", 7) == 0) {
        out += 7;
      }
      snprintf(dst, size, "%.*s", (int)(end - out), out);
      return;
    }
    out = end;
  }
}

/* ---------------- Wi-Fi parsing ------------------------------------------- */

/* Splits an `nmcli -t` line on unescaped ':' ("\:" and "\\" are escapes). */
static int split_terse(const char *line, char fields[][64], int max) {
  int n = 0;
  size_t len = 0;
  fields[0][0] = '\0';
  for (const char *p = line;; p++) {
    if (*p == '\\' && p[1] != '\0') {
      p++;
      if (len + 1 < 64) {
        fields[n][len++] = *p;
        fields[n][len] = '\0';
      }
      continue;
    }
    if (*p == ':' || *p == '\0') {
      n++;
      if (*p == '\0' || n >= max) {
        break;
      }
      len = 0;
      fields[n][0] = '\0';
      continue;
    }
    if (len + 1 < 64) {
      fields[n][len++] = *p;
      fields[n][len] = '\0';
    }
  }
  return n;
}

static int compare_nets(const void *a, const void *b) {
  const netmgr_wifi_net_t *x = a;
  const netmgr_wifi_net_t *y = b;
  if (x->active != y->active) {
    return x->active ? -1 : 1;
  }
  if (x->signal != y->signal) {
    return y->signal - x->signal;
  }
  return strcmp(x->ssid, y->ssid);
}

static void add_net(netmgr_wifi_t *w, const char *ssid, int signal,
                    bool secure, bool active) {
  for (int i = 0; i < w->count; i++) {
    if (strcmp(w->nets[i].ssid, ssid) == 0) {
      if (signal > w->nets[i].signal) {
        w->nets[i].signal = signal;
      }
      w->nets[i].secure = w->nets[i].secure || secure;
      w->nets[i].active = w->nets[i].active || active;
      return;
    }
  }
  if (w->count >= NETMGR_MAX_ITEMS) {
    return;
  }
  netmgr_wifi_net_t *net = &w->nets[w->count++];
  copy_str(net->ssid, sizeof(net->ssid), ssid);
  net->signal = signal;
  net->secure = secure;
  net->active = active;
}

void netmgr_parse_wifi(const char *out, netmgr_wifi_t *w) {
  w->available = false;
  w->enabled = false;
  w->iface[0] = '\0';
  w->count = 0;

  enum { SEC_NONE, SEC_RADIO, SEC_DEV, SEC_LIST } section = SEC_NONE;
  bool radio_known = false;
  char first_error[96] = "";

  const char *p = out;
  while (*p != '\0') {
    const char *eol = strchr(p, '\n');
    const size_t len = eol != NULL ? (size_t)(eol - p) : strlen(p);
    char line[256];
    snprintf(line, sizeof(line), "%.*s", (int)(len < 255 ? len : 255), p);
    p += len + (eol != NULL ? 1 : 0);

    size_t l = strlen(line);
    while (l > 0 && (line[l - 1] == '\r' || line[l - 1] == ' ')) {
      line[--l] = '\0';
    }
    if (l == 0) {
      continue;
    }

    if (strcmp(line, "@@missing") == 0) {
      copy_str(w->status, sizeof(w->status), "nmcli (NetworkManager) not found");
      return;
    }
    if (strcmp(line, "@@radio") == 0) {
      section = SEC_RADIO;
      continue;
    }
    if (strcmp(line, "@@dev") == 0) {
      section = SEC_DEV;
      continue;
    }
    if (strcmp(line, "@@list") == 0) {
      section = SEC_LIST;
      continue;
    }

    if (section == SEC_RADIO) {
      if (strcmp(line, "enabled") == 0) {
        w->enabled = true;
        radio_known = true;
      } else if (strcmp(line, "disabled") == 0) {
        w->enabled = false;
        radio_known = true;
      } else if (first_error[0] == '\0') {
        first_line(line, first_error, sizeof(first_error));
      }
    } else if (section == SEC_DEV) {
      char f[4][64];
      if (split_terse(line, f, 4) >= 2 && strcmp(f[1], "wifi") == 0 &&
          w->iface[0] == '\0') {
        copy_str(w->iface, sizeof(w->iface), f[0]);
      }
    } else if (section == SEC_LIST && w->enabled) {
      char f[4][64];
      if (split_terse(line, f, 4) >= 4 && f[1][0] != '\0') {
        const bool active = strcmp(f[0], "*") == 0;
        const int signal = atoi(f[2]);
        const bool secure = f[3][0] != '\0' && strcmp(f[3], "--") != 0;
        add_net(w, f[1], signal, secure, active);
      }
    }
  }

  if (!radio_known) {
    copy_str(w->status, sizeof(w->status),
             first_error[0] != '\0' ? first_error : "NetworkManager unavailable");
    return;
  }
  w->available = true;
  qsort(w->nets, (size_t)w->count, sizeof(w->nets[0]), compare_nets);
}

/* ---------------- Bluetooth parsing --------------------------------------- */

static const char *skip_ws(const char *s) {
  while (*s == ' ' || *s == '\t') {
    s++;
  }
  return s;
}

static int compare_devs(const void *a, const void *b) {
  const netmgr_bt_dev_t *x = a;
  const netmgr_bt_dev_t *y = b;
  if (x->connected != y->connected) {
    return x->connected ? -1 : 1;
  }
  return strcmp(x->name, y->name);
}

void netmgr_parse_bt(const char *out, netmgr_bt_t *b) {
  b->available = false;
  b->powered = false;
  b->count = 0;

  enum { SEC_NONE, SEC_SHOW, SEC_DEV } section = SEC_NONE;
  bool adapter = true;
  bool have_powered = false;
  netmgr_bt_dev_t cur;
  bool cur_paired = false;
  bool in_dev = false;
  memset(&cur, 0, sizeof(cur));
  char alias[64] = "";
  char name[64] = "";

#define FLUSH_DEV()                                                           \
  do {                                                                        \
    if (in_dev && cur_paired && b->count < NETMGR_MAX_ITEMS) {                \
      const char *label = alias[0] != '\0' ? alias                            \
                          : name[0] != '\0' ? name                            \
                                            : cur.addr;                       \
      copy_str(cur.name, sizeof(cur.name), label);                            \
      b->devs[b->count++] = cur;                                              \
    }                                                                         \
    in_dev = false;                                                           \
  } while (0)

  const char *p = out;
  while (*p != '\0') {
    const char *eol = strchr(p, '\n');
    const size_t len = eol != NULL ? (size_t)(eol - p) : strlen(p);
    char raw[256];
    snprintf(raw, sizeof(raw), "%.*s", (int)(len < 255 ? len : 255), p);
    p += len + (eol != NULL ? 1 : 0);

    size_t l = strlen(raw);
    while (l > 0 && (raw[l - 1] == '\r' || raw[l - 1] == ' ')) {
      raw[--l] = '\0';
    }
    const char *line = skip_ws(raw);
    if (*line == '\0') {
      continue;
    }

    if (strcmp(line, "@@missing") == 0) {
      copy_str(b->status, sizeof(b->status), "bluetoothctl (BlueZ) not found");
      return;
    }
    if (strcmp(line, "@@show") == 0) {
      section = SEC_SHOW;
      continue;
    }
    if (strncmp(line, "@@dev ", 6) == 0) {
      FLUSH_DEV();
      section = SEC_DEV;
      in_dev = true;
      cur_paired = false;
      memset(&cur, 0, sizeof(cur));
      alias[0] = name[0] = '\0';
      copy_str(cur.addr, sizeof(cur.addr), line + 6);
      continue;
    }

    if (section == SEC_SHOW) {
      if (strstr(line, "No default controller") != NULL) {
        adapter = false;
      } else if (strncmp(line, "Powered:", 8) == 0) {
        have_powered = true;
        b->powered = strstr(line + 8, "yes") != NULL;
      }
    } else if (section == SEC_DEV && in_dev) {
      if (strncmp(line, "Alias:", 6) == 0) {
        copy_str(alias, sizeof(alias), skip_ws(line + 6));
      } else if (strncmp(line, "Name:", 5) == 0) {
        copy_str(name, sizeof(name), skip_ws(line + 5));
      } else if (strncmp(line, "Paired:", 7) == 0) {
        cur_paired = strstr(line + 7, "yes") != NULL;
      } else if (strncmp(line, "Connected:", 10) == 0) {
        cur.connected = strstr(line + 10, "yes") != NULL;
      }
    }
  }
  FLUSH_DEV();
#undef FLUSH_DEV

  if (!adapter || !have_powered) {
    copy_str(b->status, sizeof(b->status), "No Bluetooth adapter found");
    b->count = 0;
    return;
  }
  b->available = true;
  qsort(b->devs, (size_t)b->count, sizeof(b->devs[0]), compare_devs);
}

/* ---------------- Wi-Fi commands ------------------------------------------ */

static const char WIFI_SCRIPT[] =
    "export LC_ALL=C\n"
    "command -v nmcli >/dev/null 2>&1 || { echo '@@missing'; exit 0; }\n"
    "nmcli dev wifi rescan >/dev/null 2>&1\n"
    "echo '@@radio'; nmcli radio wifi 2>&1\n"
    "echo '@@dev'; nmcli -t -f DEVICE,TYPE dev 2>&1\n"
    "echo '@@list'; nmcli -t -f IN-USE,SSID,SIGNAL,SECURITY dev wifi list 2>&1\n";

static void wifi_refresh_done(void *user, bool ok, const char *out) {
  (void)user;
  (void)ok;
  g_wifi.busy = false;

  /* the password prompt (need_password) and the footer status survive a
   * refresh: the parser only touches the network list */
  netmgr_parse_wifi(out, &g_wifi);
  g_wifi.loaded = true;
  g_wifi_due_ms = now_ms() + REFRESH_INTERVAL_MS;
  g_dirty = true;
}

void netmgr_wifi_refresh(void) {
  if (g_wifi.busy) {
    return;
  }
  char *const argv[] = {"sh", "-c", (char *)WIFI_SCRIPT, NULL};
  if (jobs_start(argv, LIST_TIMEOUT_MS, wifi_refresh_done, NULL)) {
    g_wifi.busy = true;
    g_wifi_due_ms = now_ms() + REFRESH_INTERVAL_MS;
  }
}

static void wifi_action_done(void *user, bool ok, const char *out) {
  (void)user;
  g_wifi.busy = false;
  if (ok) {
    set_wifi_status("", false);
  } else {
    char msg[96];
    first_line(out, msg, sizeof(msg));
    set_wifi_status(msg[0] != '\0' ? msg : "Command failed", true);
  }
  g_wifi_due_ms = 0; /* refresh on the next tick */
  netmgr_wifi_refresh();
}

void netmgr_wifi_set_enabled(bool enabled) {
  if (g_wifi.busy) {
    return;
  }
  char *const argv[] = {"nmcli", "radio", "wifi",
                        enabled ? "on" : "off", NULL};
  if (jobs_start(argv, ACTION_TIMEOUT_MS, wifi_action_done, NULL)) {
    g_wifi.busy = true;
    g_wifi.need_password = false;
    set_wifi_status(enabled ? "Turning Wi-Fi on..." : "Turning Wi-Fi off...",
                    false);
  }
}

static void wifi_connect_done(void *user, bool ok, const char *out) {
  (void)user;
  g_wifi.busy = false;

  if (ok) {
    char msg[96];
    snprintf(msg, sizeof(msg), "Connected to %s", g_wifi.password_ssid);
    g_wifi.need_password = false;
    set_wifi_status(msg, false);
  } else if (strstr(out, "Secrets were required") != NULL ||
             strstr(out, "No secrets") != NULL) {
    g_wifi.need_password = true; /* ask for the password */
    set_wifi_status("", false);
  } else {
    char msg[96];
    first_line(out, msg, sizeof(msg));
    /* a rejected password keeps the prompt open so it can be retried */
    set_wifi_status(msg[0] != '\0' ? msg : "Could not connect", true);
  }
  g_wifi_due_ms = 0;
  netmgr_wifi_refresh();
}

void netmgr_wifi_connect(const char *ssid, const char *password) {
  if (g_wifi.busy || ssid == NULL || ssid[0] == '\0') {
    return;
  }
  char ssid_copy[64];
  copy_str(ssid_copy, sizeof(ssid_copy), ssid);

  char *argv[12];
  int n = 0;
  argv[n++] = "env";
  argv[n++] = "LC_ALL=C";
  argv[n++] = "nmcli";
  argv[n++] = "-w";
  argv[n++] = "30";
  argv[n++] = "dev";
  argv[n++] = "wifi";
  argv[n++] = "connect";
  argv[n++] = ssid_copy;
  if (password != NULL && password[0] != '\0') {
    argv[n++] = "password";
    argv[n++] = (char *)password;
  }
  argv[n] = NULL;

  if (jobs_start(argv, CONNECT_TIMEOUT_MS, wifi_connect_done, NULL)) {
    g_wifi.busy = true;
    copy_str(g_wifi.password_ssid, sizeof(g_wifi.password_ssid), ssid_copy);
    char msg[96];
    snprintf(msg, sizeof(msg), "Connecting to %s...", ssid_copy);
    set_wifi_status(msg, false);
  }
}

void netmgr_wifi_disconnect(void) {
  if (g_wifi.busy || g_wifi.iface[0] == '\0') {
    return;
  }
  char iface[32];
  copy_str(iface, sizeof(iface), g_wifi.iface);
  char *const argv[] = {"nmcli", "dev", "disconnect", iface, NULL};
  if (jobs_start(argv, ACTION_TIMEOUT_MS, wifi_action_done, NULL)) {
    g_wifi.busy = true;
    set_wifi_status("Disconnecting...", false);
  }
}

void netmgr_wifi_cancel_password(void) {
  g_wifi.need_password = false;
  g_wifi.status[0] = '\0';
  g_wifi.status_error = false;
  g_dirty = true;
}

/* ---------------- Bluetooth commands -------------------------------------- */

static const char BT_SCRIPT[] =
    "export LC_ALL=C\n"
    "command -v bluetoothctl >/dev/null 2>&1 || { echo '@@missing'; exit 0; }\n"
    "echo '@@show'; bluetoothctl show 2>&1\n"
    "bluetoothctl devices 2>&1 | while read -r _ mac _; do\n"
    "  [ -n \"$mac\" ] || continue\n"
    "  echo \"@@dev $mac\"; bluetoothctl info \"$mac\" 2>&1\n"
    "done\n";

static void bt_refresh_done(void *user, bool ok, const char *out) {
  (void)user;
  (void)ok;
  g_bt.busy = false;
  netmgr_parse_bt(out, &g_bt);
  g_bt.loaded = true;
  g_bt_due_ms = now_ms() + REFRESH_INTERVAL_MS;
  g_dirty = true;
}

void netmgr_bt_refresh(void) {
  if (g_bt.busy) {
    return;
  }
  char *const argv[] = {"sh", "-c", (char *)BT_SCRIPT, NULL};
  if (jobs_start(argv, LIST_TIMEOUT_MS, bt_refresh_done, NULL)) {
    g_bt.busy = true;
    g_bt_due_ms = now_ms() + REFRESH_INTERVAL_MS;
  }
}

static void bt_action_done(void *user, bool ok, const char *out) {
  (void)user;
  g_bt.busy = false;

  /* bluetoothctl exits 0 even when the operation failed: read its output. */
  const bool failed =
      !ok || strstr(out, "Failed") != NULL || strstr(out, "not available") != NULL;
  if (failed) {
    char msg[96];
    const char *f = strstr(out, "Failed");
    first_line(f != NULL ? f : out, msg, sizeof(msg));
    set_bt_status(msg[0] != '\0' ? msg : "Command failed", true);
  } else {
    set_bt_status("", false);
  }
  g_bt_due_ms = 0;
  netmgr_bt_refresh();
}

void netmgr_bt_set_powered(bool powered) {
  if (g_bt.busy) {
    return;
  }
  char *const argv[] = {"bluetoothctl", "power", powered ? "on" : "off", NULL};
  if (jobs_start(argv, ACTION_TIMEOUT_MS, bt_action_done, NULL)) {
    g_bt.busy = true;
    set_bt_status(powered ? "Turning Bluetooth on..." : "Turning Bluetooth off...",
                  false);
  }
}

void netmgr_bt_toggle_device(int index) {
  if (g_bt.busy || index < 0 || index >= g_bt.count) {
    return;
  }
  const netmgr_bt_dev_t dev = g_bt.devs[index];
  char addr[18];
  copy_str(addr, sizeof(addr), dev.addr);
  char *const argv[] = {"bluetoothctl", dev.connected ? "disconnect" : "connect",
                        addr, NULL};
  if (jobs_start(argv, CONNECT_TIMEOUT_MS, bt_action_done, NULL)) {
    g_bt.busy = true;
    char msg[96];
    snprintf(msg, sizeof(msg), "%s %s...",
             dev.connected ? "Disconnecting" : "Connecting to", dev.name);
    set_bt_status(msg, false);
  }
}

/* ---------------- periodic refresh ---------------------------------------- */

void netmgr_tick(bool wifi_visible, bool bt_visible) {
  const long now = now_ms();
  if (wifi_visible && !g_wifi.busy && now >= g_wifi_due_ms) {
    netmgr_wifi_refresh();
  }
  if (bt_visible && !g_bt.busy && now >= g_bt_due_ms) {
    netmgr_bt_refresh();
  }
}
