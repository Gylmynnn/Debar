#ifndef DEBAR_NETMGR_H
#define DEBAR_NETMGR_H

#include <stdbool.h>

/* Backend of the Wi-Fi and Bluetooth manager popups.
 *
 * Wi-Fi goes through NetworkManager (`nmcli`), Bluetooth through BlueZ
 * (`bluetoothctl`). Everything runs asynchronously (see jobs.h) and only
 * while a popup is open: there is no polling when the popups are closed.
 * Results land in the state below; netmgr_take_dirty() tells the UI when it
 * should repaint. */

#define NETMGR_MAX_ITEMS 24

typedef struct netmgr_wifi_net {
  char ssid[64];
  int signal; /* 0..100 */
  bool secure;
  bool active; /* currently connected */
} netmgr_wifi_net_t;

typedef struct netmgr_wifi {
  bool loaded;    /* at least one refresh finished */
  bool available; /* nmcli exists and NetworkManager answers */
  bool enabled;   /* Wi-Fi radio on */
  char iface[32];
  netmgr_wifi_net_t nets[NETMGR_MAX_ITEMS];
  int count;
  bool busy;
  char status[96]; /* last action result, shown in the popup footer */
  bool status_error;
  bool need_password; /* the last connect attempt asked for a password */
  char password_ssid[64];
} netmgr_wifi_t;

typedef struct netmgr_bt_dev {
  char addr[18];
  char name[64];
  bool connected;
} netmgr_bt_dev_t;

typedef struct netmgr_bt {
  bool loaded;
  bool available; /* bluetoothctl exists and an adapter is present */
  bool powered;
  netmgr_bt_dev_t devs[NETMGR_MAX_ITEMS]; /* paired devices */
  int count;
  bool busy;
  char status[96];
  bool status_error;
} netmgr_bt_t;

const netmgr_wifi_t *netmgr_wifi(void);
const netmgr_bt_t *netmgr_bt(void);

/* 0 = not known yet, 1 = off, 2 = on (used to dim the bar icons). */
int netmgr_wifi_icon_state(void);
int netmgr_bt_icon_state(void);

/* Wi-Fi actions (no-ops while a Wi-Fi command is already running). */
void netmgr_wifi_refresh(void);
void netmgr_wifi_set_enabled(bool enabled);
/* Connects to the network called ssid; password may be NULL (saved or open
 * networks). When a password is needed, need_password/password_ssid are set.
 * The password is only read during the call: wipe your copy afterwards. */
void netmgr_wifi_connect(const char *ssid, const char *password);
void netmgr_wifi_disconnect(void);
void netmgr_wifi_cancel_password(void);

/* Bluetooth actions. */
void netmgr_bt_refresh(void);
void netmgr_bt_set_powered(bool powered);
void netmgr_bt_toggle_device(int index); /* connect or disconnect */

/* Called about once per second with the popups currently on screen: starts a
 * refresh when one is due (every few seconds while visible). */
void netmgr_tick(bool wifi_visible, bool bt_visible);

/* True once after the state changed (repaint the popup). */
bool netmgr_take_dirty(void);

/* Parsers, exposed for tests. `out` is the output of the status scripts. */
void netmgr_parse_wifi(const char *out, netmgr_wifi_t *wifi);
void netmgr_parse_bt(const char *out, netmgr_bt_t *bt);

#endif
