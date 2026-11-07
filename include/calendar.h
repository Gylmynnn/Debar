#ifndef DEBAR_CALENDAR_H
#define DEBAR_CALENDAR_H

#include <stdbool.h>

/* Size of the floating calendar popup in px (matches the reference). */
#define CAL_WIDTH 300
#define CAL_HEIGHT 288

typedef struct calendar {
  int year;  /* displayed year */
  int month; /* displayed month, 0..11 */
  int today_year;
  int today_month; /* 0..11 */
  int today_day;   /* 1..31 */
  int style;       /* 0 = filled today marker, 1 = outline */
} calendar_t;

/* One cell of the 6x7 month grid (weeks start on Monday). */
typedef struct calendar_cell {
  int day;          /* day of month shown in the cell */
  int month_offset; /* -1 previous month, 0 displayed month, +1 next month */
  bool today;
} calendar_cell_t;

void calendar_init(calendar_t *cal);

/* Re-reads the system date. Returns true when the day changed. */
bool calendar_refresh_today(calendar_t *cal);

void calendar_shift_month(calendar_t *cal, int delta);
void calendar_shift_year(calendar_t *cal, int delta);
void calendar_goto_today(calendar_t *cal);
void calendar_toggle_style(calendar_t *cal);

void calendar_build_grid(const calendar_t *cal, calendar_cell_t cells[42]);
const char *calendar_month_name(int month);

#endif
