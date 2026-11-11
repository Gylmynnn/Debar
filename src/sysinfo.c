#define _GNU_SOURCE
#include "sysinfo.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* Scratch tables, allocated by sysinfo_open() and freed by sysinfo_close(),
 * so the bar costs no memory while the popups are closed. */
#define PREV_SLOTS 2048 /* power of two: pids remembered between CPU samples */
#define AGG_MAX 384     /* distinct program names tracked per scan */

typedef struct cpu_counters {
  unsigned long long total;
  unsigned long long idle;
} cpu_counters_t;

typedef struct prev_proc {
  int pid; /* 0 = never used */
  unsigned gen;
  unsigned long long ticks; /* utime + stime at the previous scan */
} prev_proc_t;

typedef struct agg {
  char name[24];
  unsigned long long rss_kb;
  unsigned long long ticks;
} agg_t;

typedef enum scan_mode {
  SCAN_RECORD, /* remember CPU ticks only (the baseline) */
  SCAN_CPU,    /* CPU ticks since the previous scan, summed per program */
  SCAN_MEM,    /* private memory, summed per program */
} scan_mode_t;

static sysinfo_kind_t g_kind = SYSINFO_NONE;
static sysinfo_cpu_t g_cpu;
static sysinfo_mem_t g_mem;

static cpu_counters_t g_prev_total;
static cpu_counters_t g_prev_core[SYSINFO_MAX_CORES];
static bool g_have_base;

static prev_proc_t *g_prev;
static unsigned g_gen;
static agg_t *g_agg;
static int g_agg_n;

static char g_temp_path[160]; /* hwmon file with the CPU temperature, or "" */

/* ---------------- small helpers ------------------------------------------ */

/* Reads a whole (small) file; returns the length or -1. open/read/close is
 * noticeably cheaper than stdio when hundreds of /proc files are read. */
static ssize_t read_small(const char *path, char *buf, size_t size) {
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return -1;
  }
  const ssize_t n = read(fd, buf, size - 1);
  close(fd);
  if (n < 0) {
    return -1;
  }
  buf[n] = '\0';
  return n;
}

static double clamp_pct(double v) {
  if (v < 0.0) {
    return 0.0;
  }
  return v > 100.0 ? 100.0 : v;
}

/* ---------------- /proc/stat --------------------------------------------- */

static bool parse_counters(const char *line, cpu_counters_t *out) {
  unsigned long long v[8] = {0};
  const int n = sscanf(line, "%*s %llu %llu %llu %llu %llu %llu %llu %llu",
                       &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
  if (n < 4) {
    return false;
  }
  out->idle = v[3] + v[4]; /* idle + iowait */
  out->total = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6] + v[7];
  return true;
}

/* The "cpu" lines come first in /proc/stat; the rest of the file (the long
 * "intr" line in particular) is never needed. */
static bool read_stat(cpu_counters_t *total, cpu_counters_t cores[],
                      int *ncores) {
  char buf[8192];
  if (read_small("/proc/stat", buf, sizeof(buf)) <= 0) {
    return false;
  }

  int n = 0;
  bool have_total = false;
  char *line = buf;
  while (line != NULL && strncmp(line, "cpu", 3) == 0) {
    char *eol = strchr(line, '\n');
    if (eol != NULL) {
      *eol = '\0';
    }
    if (line[3] == ' ') {
      have_total = parse_counters(line, total);
    } else if (isdigit((unsigned char)line[3])) {
      if (n < SYSINFO_MAX_CORES && parse_counters(line, &cores[n])) {
        n++;
      }
    }
    line = eol != NULL ? eol + 1 : NULL;
  }
  *ncores = n;
  return have_total && n > 0;
}

int sysinfo_cpu_count(void) {
  cpu_counters_t total;
  cpu_counters_t cores[SYSINFO_MAX_CORES];
  int n = 0;
  if (!read_stat(&total, cores, &n) || n < 1) {
    return 1;
  }
  return n;
}

static double usage(cpu_counters_t cur, cpu_counters_t prev) {
  if (cur.total <= prev.total) {
    return 0.0;
  }
  const double dt = (double)(cur.total - prev.total);
  const double di = cur.idle >= prev.idle ? (double)(cur.idle - prev.idle) : 0.0;
  return clamp_pct(100.0 * (dt - di) / dt);
}

/* ---------------- temperature and frequency ------------------------------ */

static int read_int_path(const char *path) {
  char buf[32];
  if (read_small(path, buf, sizeof(buf)) <= 0) {
    return -1;
  }
  return atoi(buf);
}

/* Looks for the CPU package sensor once per popup. coretemp (Intel), k10temp
 * and zenpower (AMD) are preferred; arm boards usually expose cpu_thermal. */
static void find_temp_sensor(void) {
  g_temp_path[0] = '\0';

  DIR *dir = opendir("/sys/class/hwmon");
  if (dir == NULL) {
    return;
  }

  int best_rank = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] == '.') {
      continue;
    }

    char base[96];
    snprintf(base, sizeof(base), "/sys/class/hwmon/%.60s", entry->d_name);

    char path[160];
    char name[48];
    snprintf(path, sizeof(path), "%s/name", base);
    if (read_small(path, name, sizeof(name)) <= 0) {
      continue;
    }
    name[strcspn(name, "\n")] = '\0';

    int rank = 0;
    if (strcmp(name, "coretemp") == 0 || strcmp(name, "k10temp") == 0 ||
        strcmp(name, "zenpower") == 0) {
      rank = 3;
    } else if (strcmp(name, "cpu_thermal") == 0) {
      rank = 2;
    } else if (strcmp(name, "acpitz") == 0) {
      rank = 1;
    }
    if (rank <= best_rank) {
      continue;
    }

    /* Prefer the sensor labelled as the package / die temperature. */
    char candidate[160] = "";
    for (int k = 1; k <= 8; k++) {
      char label[48];
      snprintf(path, sizeof(path), "%s/temp%d_label", base, k);
      if (read_small(path, label, sizeof(label)) <= 0) {
        continue;
      }
      if (strncmp(label, "Package id 0", 12) == 0 ||
          strncmp(label, "Tctl", 4) == 0 || strncmp(label, "Tdie", 4) == 0 ||
          strncmp(label, "CPU", 3) == 0) {
        snprintf(candidate, sizeof(candidate), "%s/temp%d_input", base, k);
        break;
      }
    }
    if (candidate[0] == '\0') {
      snprintf(candidate, sizeof(candidate), "%s/temp1_input", base);
    }
    if (read_int_path(candidate) <= 0) {
      continue;
    }

    snprintf(g_temp_path, sizeof(g_temp_path), "%s", candidate);
    best_rank = rank;
  }
  closedir(dir);
}

static double read_temp(void) {
  if (g_temp_path[0] == '\0') {
    return -1.0;
  }
  const int milli = read_int_path(g_temp_path);
  return milli > 0 ? milli / 1000.0 : -1.0;
}

static double read_avg_freq(int cores) {
  double sum = 0.0;
  int seen = 0;
  for (int i = 0; i < cores; i++) {
    char path[96];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
    const int khz = read_int_path(path);
    if (khz > 0) {
      sum += khz;
      seen++;
    }
  }
  return seen > 0 ? sum / seen / 1000.0 : -1.0;
}

/* ---------------- per-program accounting --------------------------------- */

/* Finds pid's slot in the table of the previous scan. Returns true when the
 * pid is there. *slot is where it lives, or where it can be inserted (NULL:
 * the table is full). Slots whose generation is older than the previous scan
 * belong to dead processes and may be reused. */
static bool prev_find(int pid, prev_proc_t **slot) {
  *slot = NULL;
  if (g_prev == NULL) {
    return false;
  }
  unsigned idx = ((unsigned)pid * 2654435761u) & (PREV_SLOTS - 1);
  for (int probe = 0; probe < PREV_SLOTS; probe++) {
    prev_proc_t *p = &g_prev[(idx + (unsigned)probe) & (PREV_SLOTS - 1)];
    if (p->pid == pid) {
      *slot = p;
      return true;
    }
    if (p->pid == 0) {
      if (*slot == NULL) {
        *slot = p;
      }
      return false;
    }
    if (*slot == NULL && p->gen + 1 < g_gen) {
      *slot = p; /* stale: reusable, but keep looking for pid itself */
    }
  }
  return false;
}

static void agg_add(const char *name, unsigned long long rss_kb,
                    unsigned long long ticks) {
  if (g_agg == NULL) {
    return;
  }
  for (int i = 0; i < g_agg_n; i++) {
    if (strcmp(g_agg[i].name, name) == 0) {
      g_agg[i].rss_kb += rss_kb;
      g_agg[i].ticks += ticks;
      return;
    }
  }
  if (g_agg_n >= AGG_MAX) {
    return;
  }
  agg_t *a = &g_agg[g_agg_n++];
  snprintf(a->name, sizeof(a->name), "%.*s", (int)sizeof(a->name) - 1, name);
  a->rss_kb = rss_kb;
  a->ticks = ticks;
}

/* Program name for a pid: the basename of its executable, so every helper
 * process of a browser lands under one name. Falls back to the kernel's comm
 * (other users' processes hide their exe link). */
static void proc_name(int pid, const char *comm_hint, char *out, size_t size) {
  char path[40];
  char link[256];
  snprintf(path, sizeof(path), "/proc/%d/exe", pid);
  const ssize_t n = readlink(path, link, sizeof(link) - 1);
  if (n > 0) {
    link[n] = '\0';
    char *deleted = strstr(link, " (deleted)");
    if (deleted != NULL) {
      *deleted = '\0';
    }
    const char *base = strrchr(link, '/');
    base = base != NULL ? base + 1 : link;
    /* "memfd:..." executables (anonymous files) have no useful name */
    if (base[0] != '\0' && strncmp(base, "memfd:", 6) != 0) {
      snprintf(out, size, "%.*s", (int)size - 1, base);
      return;
    }
  }

  if (comm_hint != NULL && comm_hint[0] != '\0') {
    snprintf(out, size, "%.*s", (int)size - 1, comm_hint);
    return;
  }
  snprintf(path, sizeof(path), "/proc/%d/comm", pid);
  char comm[32];
  if (read_small(path, comm, sizeof(comm)) > 0) {
    comm[strcspn(comm, "\n")] = '\0';
    snprintf(out, size, "%.*s", (int)size - 1, comm);
    return;
  }
  snprintf(out, size, "?");
}

/* Walks /proc once. SCAN_RECORD / SCAN_CPU read utime+stime from
 * /proc/<pid>/stat, SCAN_MEM reads the private memory from statm. Returns
 * the number of processes seen. */
static int scan_procs(scan_mode_t mode) {
  g_agg_n = 0;
  if (mode != SCAN_MEM) {
    g_gen++;
  }

  DIR *dir = opendir("/proc");
  if (dir == NULL) {
    return 0;
  }

  const long page_kb = sysconf(_SC_PAGESIZE) / 1024;
  int count = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] < '1' || entry->d_name[0] > '9') {
      continue; /* not a pid directory */
    }
    const int pid = atoi(entry->d_name);
    if (pid <= 0) {
      continue;
    }
    count++;

    char path[40];
    char buf[512];
    char name[24];

    if (mode == SCAN_MEM) {
      snprintf(path, sizeof(path), "/proc/%d/statm", pid);
      if (read_small(path, buf, sizeof(buf)) <= 0) {
        continue;
      }
      unsigned long long resident = 0;
      unsigned long long shared = 0;
      if (sscanf(buf, "%*u %llu %llu", &resident, &shared) != 2) {
        continue;
      }
      /* resident - shared = private memory. Counting RSS as is would add
       * the same shared libraries once per process of a multi-process app. */
      const unsigned long long priv = resident > shared ? resident - shared : 0;
      if (priv == 0) {
        continue; /* kernel threads */
      }
      proc_name(pid, NULL, name, sizeof(name));
      agg_add(name, priv * (unsigned long long)page_kb, 0);
      continue;
    }

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    if (read_small(path, buf, sizeof(buf)) <= 0) {
      continue;
    }
    char *open_paren = strchr(buf, '(');
    char *close_paren = strrchr(buf, ')');
    if (open_paren == NULL || close_paren == NULL || close_paren < open_paren ||
        close_paren[1] != ' ') {
      continue;
    }
    unsigned long long utime = 0;
    unsigned long long stime = 0;
    if (sscanf(close_paren + 2,
               "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",
               &utime, &stime) != 2) {
      continue;
    }
    const unsigned long long ticks = utime + stime;

    prev_proc_t *slot = NULL;
    const bool known = prev_find(pid, &slot);
    unsigned long long delta = 0;
    if (known && ticks >= slot->ticks) {
      delta = ticks - slot->ticks;
    }
    if (slot != NULL) {
      slot->pid = pid;
      slot->gen = g_gen;
      slot->ticks = ticks;
    }

    if (mode == SCAN_CPU && delta > 0) {
      *close_paren = '\0';
      proc_name(pid, open_paren + 1, name, sizeof(name));
      agg_add(name, 0, delta);
    }
  }
  closedir(dir);
  return count;
}

/* Picks the n largest entries of the last scan. For CPU the value becomes a
 * percentage of the whole machine (total_delta is all cores' ticks). */
static int pick_top(sysinfo_proc_t *out, int n, bool by_cpu,
                    unsigned long long total_delta) {
  bool used[AGG_MAX] = {false};
  int filled = 0;

  for (; filled < n; filled++) {
    int best = -1;
    unsigned long long best_key = 0;
    for (int i = 0; i < g_agg_n; i++) {
      const unsigned long long key = by_cpu ? g_agg[i].ticks : g_agg[i].rss_kb;
      if (!used[i] && key > best_key) {
        best = i;
        best_key = key;
      }
    }
    if (best < 0) {
      break;
    }
    used[best] = true;
    memcpy(out[filled].name, g_agg[best].name, sizeof(out[filled].name));
    out[filled].value = by_cpu && total_delta > 0
                            ? clamp_pct(100.0 * (double)best_key /
                                        (double)total_delta)
                            : (double)best_key;
  }
  return filled;
}

/* ---------------- CPU sample --------------------------------------------- */

static void sample_cpu(void) {
  cpu_counters_t total;
  cpu_counters_t cores[SYSINFO_MAX_CORES];
  int n = 0;
  if (!read_stat(&total, cores, &n)) {
    return;
  }

  unsigned long long total_delta = 0;
  if (g_have_base) {
    total_delta =
        total.total > g_prev_total.total ? total.total - g_prev_total.total : 0;
    g_cpu.total = usage(total, g_prev_total);
    for (int i = 0; i < n; i++) {
      g_cpu.core[i] = usage(cores[i], g_prev_core[i]);
    }
    g_cpu.valid = total_delta > 0;
  }
  g_cpu.cores = n;
  g_prev_total = total;
  memcpy(g_prev_core, cores, sizeof(cores[0]) * (size_t)n);

  /* The baseline scan only records ticks; real samples add them up. */
  g_cpu.procs = scan_procs(g_have_base ? SCAN_CPU : SCAN_RECORD);
  if (g_have_base && total_delta > 0) {
    g_cpu.top_count = pick_top(g_cpu.top, SYSINFO_CPU_TOP, true, total_delta);
  }
  g_have_base = true;

  FILE *f = fopen("/proc/loadavg", "r");
  if (f != NULL) {
    if (fscanf(f, "%lf %lf %lf", &g_cpu.load[0], &g_cpu.load[1],
               &g_cpu.load[2]) != 3) {
      g_cpu.load[0] = g_cpu.load[1] = g_cpu.load[2] = 0.0;
    }
    fclose(f);
  }

  char up[48];
  if (read_small("/proc/uptime", up, sizeof(up)) > 0) {
    g_cpu.uptime_s = (long)atof(up);
  }

  g_cpu.temp_c = read_temp();
  g_cpu.freq_mhz = read_avg_freq(n);
}

/* ---------------- RAM sample --------------------------------------------- */

static void sample_mem(void) {
  char buf[4096];
  if (read_small("/proc/meminfo", buf, sizeof(buf)) <= 0) {
    return;
  }

  long long total = 0;
  long long free_kb = 0;
  long long avail = -1;
  long long swap_total = 0;
  long long swap_free = 0;
  for (char *line = buf; line != NULL;) {
    char *eol = strchr(line, '\n');
    if (eol != NULL) {
      *eol = '\0';
    }
    if (strncmp(line, "MemTotal:", 9) == 0) {
      sscanf(line + 9, " %lld", &total);
    } else if (strncmp(line, "MemFree:", 8) == 0) {
      sscanf(line + 8, " %lld", &free_kb);
    } else if (strncmp(line, "MemAvailable:", 13) == 0) {
      sscanf(line + 13, " %lld", &avail);
    } else if (strncmp(line, "SwapTotal:", 10) == 0) {
      sscanf(line + 10, " %lld", &swap_total);
    } else if (strncmp(line, "SwapFree:", 9) == 0) {
      sscanf(line + 9, " %lld", &swap_free);
    }
    line = eol != NULL ? eol + 1 : NULL;
  }
  if (total <= 0 || avail < 0) {
    return;
  }

  g_mem.total_kb = total;
  g_mem.used_kb = total - avail;
  g_mem.free_kb = free_kb;
  g_mem.cache_kb = avail > free_kb ? avail - free_kb : 0;
  g_mem.swap_total_kb = swap_total;
  g_mem.swap_used_kb = swap_total > swap_free ? swap_total - swap_free : 0;
  g_mem.valid = true;

  scan_procs(SCAN_MEM);
  g_mem.top_count = pick_top(g_mem.top, SYSINFO_MEM_TOP, false, 0);
}

/* ---------------- public API --------------------------------------------- */

void sysinfo_close(void) {
  free(g_prev);
  g_prev = NULL;
  free(g_agg);
  g_agg = NULL;
  g_agg_n = 0;
  g_kind = SYSINFO_NONE;
  g_have_base = false;
  memset(&g_cpu, 0, sizeof(g_cpu));
  memset(&g_mem, 0, sizeof(g_mem));
}

void sysinfo_open(sysinfo_kind_t kind) {
  sysinfo_close();
  if (kind == SYSINFO_NONE) {
    return;
  }

  g_kind = kind;
  g_cpu.temp_c = -1.0;
  g_cpu.freq_mhz = -1.0;
  g_agg = calloc(AGG_MAX, sizeof(*g_agg));

  if (kind == SYSINFO_CPU) {
    g_prev = calloc(PREV_SLOTS, sizeof(*g_prev));
    g_gen = 1;
    find_temp_sensor();
    sample_cpu(); /* baseline: percentages appear with the next sample */
  } else {
    sample_mem();
  }
}

void sysinfo_sample(void) {
  if (g_kind == SYSINFO_CPU) {
    sample_cpu();
  } else if (g_kind == SYSINFO_MEM) {
    sample_mem();
  }
}

const sysinfo_cpu_t *sysinfo_cpu(void) { return &g_cpu; }

const sysinfo_mem_t *sysinfo_mem(void) { return &g_mem; }
