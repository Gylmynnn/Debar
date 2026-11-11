#ifndef DEBAR_SYSINFO_H
#define DEBAR_SYSINFO_H

#include <stdbool.h>

/* Backend of the CPU and RAM popups.
 *
 * The bar pills only need one number (modules.c), but the popups show much
 * more: per-core load, temperature, frequency, and the programs that use the
 * most CPU / RAM. That data is expensive enough (it walks /proc) that it is
 * only collected while a popup is open: sysinfo_open() starts sampling,
 * sysinfo_sample() is called once per second by the main loop, and
 * sysinfo_close() frees everything again. Nothing runs while the popups are
 * closed. */

#define SYSINFO_MAX_CORES 32 /* cores beyond this are not listed */
#define SYSINFO_MEM_TOP 5    /* programs listed in the RAM popup */
#define SYSINFO_CPU_TOP 3    /* programs listed in the CPU popup */

typedef enum sysinfo_kind {
  SYSINFO_NONE = 0,
  SYSINFO_CPU,
  SYSINFO_MEM,
} sysinfo_kind_t;

/* A program, i.e. every process that runs the same executable summed up (a
 * browser with 30 helper processes is one entry). */
typedef struct sysinfo_proc {
  char name[24];
  double value; /* RAM popup: KiB (private memory); CPU popup: percent of the
                 * whole machine, so it is comparable with the total */
} sysinfo_proc_t;

typedef struct sysinfo_cpu {
  bool valid; /* two samples exist, so the percentages mean something */
  int cores;
  double total;                   /* 0..100 */
  double core[SYSINFO_MAX_CORES]; /* 0..100 each */
  double load[3];                 /* 1, 5, 15 minute load average */
  double temp_c;                  /* < 0: no sensor found */
  double freq_mhz;                /* average over the cores, < 0: unknown */
  long uptime_s;
  int procs; /* processes seen in the last scan */
  int top_count;
  sysinfo_proc_t top[SYSINFO_CPU_TOP];
} sysinfo_cpu_t;

typedef struct sysinfo_mem {
  bool valid;
  long long total_kb;
  long long used_kb;  /* total - available: what the bar pill shows */
  long long cache_kb; /* available - free: reclaimable cache and buffers */
  long long free_kb;
  long long swap_total_kb;
  long long swap_used_kb;
  int top_count;
  sysinfo_proc_t top[SYSINFO_MEM_TOP];
} sysinfo_mem_t;

/* Number of CPU cores that will be listed (1..SYSINFO_MAX_CORES). Used to
 * size the CPU popup before it opens. */
int sysinfo_cpu_count(void);

/* Starts sampling for one popup (stops any previous one). The RAM data is
 * valid right away; the CPU data needs a second sample, so call
 * sysinfo_sample() again shortly after opening. */
void sysinfo_open(sysinfo_kind_t kind);
void sysinfo_close(void);

/* Takes a new sample for the kind passed to sysinfo_open(). */
void sysinfo_sample(void);

const sysinfo_cpu_t *sysinfo_cpu(void);
const sysinfo_mem_t *sysinfo_mem(void);

#endif
