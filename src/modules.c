#include "modules.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void update_clock(modules_data_t *d) {
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);

  char weekday[24];
  char timestr[24];
  strftime(weekday, sizeof(weekday), "%A", &tm);
  strftime(timestr, sizeof(timestr), "%I:%M %p", &tm);
  snprintf(d->clock, sizeof(d->clock), "%s %s", weekday, timestr);
}

static void update_cpu(modules_data_t *d) {
  static unsigned long long prev_idle = 0;
  static unsigned long long prev_total = 0;
  static bool have_prev = false;

  FILE *f = fopen("/proc/stat", "r");
  if (f == NULL) {
    snprintf(d->cpu, sizeof(d->cpu), "--");
    return;
  }

  char line[256];
  unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
  if (fgets(line, sizeof(line), f) == NULL ||
      sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &user, &nice,
             &system, &idle, &iowait, &irq, &softirq, &steal) < 8) {
    fclose(f);
    snprintf(d->cpu, sizeof(d->cpu), "0 %%");
    return;
  }
  fclose(f);

  const unsigned long long idle_all = idle + iowait;
  const unsigned long long total =
      user + nice + system + idle + iowait + irq + softirq + steal;

  if (have_prev && total > prev_total) {
    const double dt = (double)(total - prev_total);
    const double di = (double)(idle_all - prev_idle);
    double pct = 100.0 * (dt - di) / dt;
    if (pct < 0.0) {
      pct = 0.0;
    }
    snprintf(d->cpu, sizeof(d->cpu), "%.0f %%", pct);
  } else if (!have_prev) {
    snprintf(d->cpu, sizeof(d->cpu), "0 %%");
  }

  have_prev = true;
  prev_total = total;
  prev_idle = idle_all;
}

static void update_mem(modules_data_t *d) {
  FILE *f = fopen("/proc/meminfo", "r");
  if (f == NULL) {
    snprintf(d->mem, sizeof(d->mem), "-- GiB");
    return;
  }

  long long total = 0;
  long long available = -1;
  char line[128];
  while (fgets(line, sizeof(line), f) != NULL) {
    if (strncmp(line, "MemTotal:", 9) == 0) {
      sscanf(line + 9, " %lld", &total);
    } else if (strncmp(line, "MemAvailable:", 13) == 0) {
      sscanf(line + 13, " %lld", &available);
      break;
    }
  }
  fclose(f);

  if (available < 0) {
    snprintf(d->mem, sizeof(d->mem), "-- GiB");
    return;
  }

  const double used_gib = (double)(total - available) / (1024.0 * 1024.0);
  snprintf(d->mem, sizeof(d->mem), "%.1f GiB", used_gib);
}

static void update_volume(modules_data_t *d) {
  d->volume[0] = '\0';

  /* PipeWire/WirePlumber first: "Volume: 0.42" (may end with [MUTED]). */
  FILE *f = popen("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r");
  if (f != NULL) {
    char line[128];
    if (fgets(line, sizeof(line), f) != NULL) {
      const char *p = strstr(line, "Volume:");
      if (p != NULL) {
        int value = (int)(atof(p + 7) * 100.0 + 0.5);
        if (value < 0) {
          value = 0;
        } else if (value > 100) {
          value = 100;
        }
        snprintf(d->volume, sizeof(d->volume), "%d%%", value);
      }
    }
    pclose(f);
    if (d->volume[0] != '\0') {
      return;
    }
  }

  /* Fallback: pactl (pipewire-pulse or PulseAudio). */
  f = popen("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null", "r");
  if (f == NULL) {
    return;
  }

  char buf[256];
  if (fgets(buf, sizeof(buf), f) != NULL) {
    char *percent = strchr(buf, '%');
    if (percent != NULL && percent > buf) {
      char *start = percent - 1;
      while (start >= buf && isdigit((unsigned char)*start)) {
        start--;
      }
      start++;

      int value = 0;
      if (sscanf(start, "%d", &value) == 1 && value >= 0 && value <= 999) {
        snprintf(d->volume, sizeof(d->volume), "%d%%", value);
      }
    }
  }
  pclose(f);
}

static int read_int_file(const char *path) {
  FILE *f = fopen(path, "r");
  if (f == NULL) {
    return -1;
  }
  int value = -1;
  if (fscanf(f, " %d", &value) != 1) {
    value = -1;
  }
  fclose(f);
  return value;
}

static void update_brightness(modules_data_t *d) {
  d->brightness[0] = '\0';

  DIR *dir = opendir("/sys/class/backlight");
  if (dir == NULL) {
    return;
  }

  struct dirent *entry;
  char path[PATH_MAX];
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] == '.') {
      continue;
    }

    snprintf(path, sizeof(path), "/sys/class/backlight/%s/max_brightness",
             entry->d_name);
    const int max = read_int_file(path);
    snprintf(path, sizeof(path), "/sys/class/backlight/%s/brightness",
             entry->d_name);
    const int cur = read_int_file(path);

    if (max > 0 && cur >= 0) {
      int pct = (int)((100.0 * cur) / max + 0.5);
      if (pct < 0) {
        pct = 0;
      } else if (pct > 100) {
        pct = 100;
      }
      snprintf(d->brightness, sizeof(d->brightness), "%d%%", pct);
      break;
    }
  }
  closedir(dir);
}

static void update_battery(modules_data_t *d) {
  d->battery_present = false;
  d->battery[0] = '\0';
  d->battery_fill = 0.0;

  DIR *dir = opendir("/sys/class/power_supply");
  if (dir == NULL) {
    return;
  }

  struct dirent *entry;
  char path[PATH_MAX];
  int percent = 0;
  bool found = false;

  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "BAT", 3) != 0) {
      continue;
    }

    snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity",
             entry->d_name);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
      continue;
    }
    const int scan = fscanf(f, " %d", &percent);
    fclose(f);
    if (scan == 1) {
      found = true;
      break;
    }
  }
  closedir(dir);

  if (!found) {
    return;
  }

  if (percent < 0) {
    percent = 0;
  } else if (percent > 100) {
    percent = 100;
  }

  d->battery_present = true;
  d->battery_fill = percent / 100.0;
  snprintf(d->battery, sizeof(d->battery), "%d %%", percent);
}

void modules_update(modules_data_t *d) {
  if (d == NULL) {
    return;
  }

  update_clock(d);
  update_cpu(d);
  update_mem(d);
  update_volume(d);
  update_brightness(d);
  update_battery(d);
}

void modules_init(modules_data_t *d) {
  if (d == NULL) {
    return;
  }
  memset(d, 0, sizeof(*d));
  d->ws_count = 6;
  d->ws_active = 0;
  d->ws_occupied = 1u << 1;
  modules_update(d);
}

/* ---- wheel actions: volume / brightness --------------------------------- */

/* Runs a shell command detached from the bar. Double-fork keeps the Wayland
 * event loop from ever waiting on it and leaves no zombie behind; output is
 * dropped so a missing tool cannot spam the terminal. */
static void spawn_async(const char *cmd) {
  const pid_t pid = fork();
  if (pid < 0) {
    return;
  }
  if (pid == 0) {
    const pid_t inner = fork();
    if (inner == 0) {
      const int devnull = open("/dev/null", O_WRONLY);
      if (devnull >= 0) {
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
      }
      execlp("sh", "sh", "-c", cmd, (char *)NULL);
      _exit(127);
    }
    _exit(inner < 0 ? 1 : 0);
  }
  int status = 0;
  (void)waitpid(pid, &status, 0); /* the intermediate child exits at once */
}

/* Moves an "NN%" label by delta immediately, so the bar reacts on the very
 * next frame instead of waiting for the poll. The poll then replaces it with
 * the authoritative value (correcting any failed command). */
static void bump_percent(char *label, size_t nlabel, int delta) {
  if (label[0] == '\0') {
    return; /* module not shown */
  }
  int value = atoi(label) + delta;
  if (value < 0) {
    value = 0;
  } else if (value > 100) {
    value = 100;
  }
  snprintf(label, nlabel, "%d%%", value);
}

void modules_volume_step(modules_data_t *d, int delta_percent) {
  if (delta_percent == 0) {
    return;
  }
  const int step = delta_percent < 0 ? -delta_percent : delta_percent;
  const char sign = delta_percent > 0 ? '+' : '-';

  /* wpctl (PipeWire) with a pactl fallback for pulse-only setups. */
  char cmd[192];
  snprintf(cmd, sizeof(cmd),
           "wpctl set-volume @DEFAULT_AUDIO_SINK@ %d%%%c 2>/dev/null || "
           "pactl set-sink-volume @DEFAULT_SINK@ %c%d%%",
           step, sign, sign, step);
  spawn_async(cmd);

  if (d != NULL) {
    bump_percent(d->volume, sizeof(d->volume), delta_percent);
  }
}

void modules_brightness_step(modules_data_t *d, int delta_percent) {
  if (delta_percent == 0) {
    return;
  }
  const int step = delta_percent < 0 ? -delta_percent : delta_percent;
  const char sign = delta_percent > 0 ? '+' : '-';

  char cmd[128];
  snprintf(cmd, sizeof(cmd), "brightnessctl -q set %d%%%c", step, sign);
  spawn_async(cmd);

  if (d != NULL) {
    bump_percent(d->brightness, sizeof(d->brightness), delta_percent);
  }
}
