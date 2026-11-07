#define _POSIX_C_SOURCE 200809L
#include "calendar.h"

#include <time.h>

#define CAL_MIN_YEAR 1583
#define CAL_MAX_YEAR 9999

static const char *const MONTH_NAMES[12] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December",
};

static bool is_leap(int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int days_in_month(int year, int month) {
  static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 1 && is_leap(year)) {
    return 29;
  }
  return days[month];
}

/* Day of week, 0 = Sunday (Sakamoto's method). month is 0..11. */
static int day_of_week(int year, int month, int day) {
  static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (month < 2) {
    year -= 1;
  }
  return (year + year / 4 - year / 100 + year / 400 + t[month] + day) % 7;
}

static void read_today(calendar_t *cal) {
  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  cal->today_year = tm.tm_year + 1900;
  cal->today_month = tm.tm_mon;
  cal->today_day = tm.tm_mday;
}

void calendar_init(calendar_t *cal) {
  if (cal == NULL) {
    return;
  }
  read_today(cal);
  cal->year = cal->today_year;
  cal->month = cal->today_month;
  cal->style = 0;
}

bool calendar_refresh_today(calendar_t *cal) {
  if (cal == NULL) {
    return false;
  }
  const int y = cal->today_year;
  const int m = cal->today_month;
  const int d = cal->today_day;
  read_today(cal);
  return y != cal->today_year || m != cal->today_month || d != cal->today_day;
}

void calendar_shift_month(calendar_t *cal, int delta) {
  if (cal == NULL) {
    return;
  }
  int index = cal->year * 12 + cal->month + delta;
  if (index < CAL_MIN_YEAR * 12) {
    index = CAL_MIN_YEAR * 12;
  } else if (index > CAL_MAX_YEAR * 12 + 11) {
    index = CAL_MAX_YEAR * 12 + 11;
  }
  cal->year = index / 12;
  cal->month = index % 12;
}

void calendar_shift_year(calendar_t *cal, int delta) {
  if (cal == NULL) {
    return;
  }
  int year = cal->year + delta;
  if (year < CAL_MIN_YEAR) {
    year = CAL_MIN_YEAR;
  } else if (year > CAL_MAX_YEAR) {
    year = CAL_MAX_YEAR;
  }
  cal->year = year;
}

void calendar_goto_today(calendar_t *cal) {
  if (cal == NULL) {
    return;
  }
  read_today(cal);
  cal->year = cal->today_year;
  cal->month = cal->today_month;
}

void calendar_toggle_style(calendar_t *cal) {
  if (cal != NULL) {
    cal->style = cal->style == 0 ? 1 : 0;
  }
}

const char *calendar_month_name(int month) {
  if (month < 0 || month > 11) {
    return "";
  }
  return MONTH_NAMES[month];
}

void calendar_build_grid(const calendar_t *cal, calendar_cell_t cells[42]) {
  if (cal == NULL || cells == NULL) {
    return;
  }

  /* Monday = 0 ... Sunday = 6 */
  const int first = (day_of_week(cal->year, cal->month, 1) + 6) % 7;
  const int dim = days_in_month(cal->year, cal->month);

  int prev_year = cal->year;
  int prev_month = cal->month - 1;
  if (prev_month < 0) {
    prev_month = 11;
    prev_year -= 1;
  }
  int next_year = cal->year;
  int next_month = cal->month + 1;
  if (next_month > 11) {
    next_month = 0;
    next_year += 1;
  }
  const int prev_dim = days_in_month(prev_year, prev_month);

  for (int i = 0; i < 42; i++) {
    const int n = i - first + 1;
    int day = n;
    int offset = 0;
    int year = cal->year;
    int month = cal->month;

    if (n < 1) {
      day = prev_dim + n;
      offset = -1;
      year = prev_year;
      month = prev_month;
    } else if (n > dim) {
      day = n - dim;
      offset = 1;
      year = next_year;
      month = next_month;
    }

    cells[i].day = day;
    cells[i].month_offset = offset;
    cells[i].today = year == cal->today_year && month == cal->today_month &&
                     day == cal->today_day;
  }
}
