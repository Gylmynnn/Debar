#define _GNU_SOURCE
#include "jobs.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define JOB_OUTPUT_MAX 32768

typedef struct job {
  bool used;
  pid_t pid;
  int fd;
  char *buf;
  size_t len;
  long deadline_ms;
  job_done_fn done;
  void *user;
} job_t;

static job_t g_jobs[JOBS_MAX];
static int g_fill_slot[JOBS_MAX];

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

bool jobs_start(char *const argv[], int timeout_ms, job_done_fn done,
                void *user) {
  if (argv == NULL || argv[0] == NULL || done == NULL) {
    return false;
  }

  int slot = -1;
  for (int i = 0; i < JOBS_MAX; i++) {
    if (!g_jobs[i].used) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    return false;
  }

  char *buf = malloc(JOB_OUTPUT_MAX + 1);
  if (buf == NULL) {
    return false;
  }

  int fds[2];
  if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
    free(buf);
    return false;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                   O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);

  pid_t pid = 0;
  const int rc = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  close(fds[1]);
  if (rc != 0) {
    close(fds[0]);
    free(buf);
    return false;
  }

  job_t *job = &g_jobs[slot];
  job->used = true;
  job->pid = pid;
  job->fd = fds[0];
  job->buf = buf;
  job->len = 0;
  job->deadline_ms = now_ms() + (timeout_ms > 0 ? timeout_ms : 10000);
  job->done = done;
  job->user = user;
  return true;
}

static void finish(int slot, bool timed_out) {
  job_t job = g_jobs[slot]; /* copy: done() may start a new job in this slot */
  g_jobs[slot].used = false;

  close(job.fd);
  if (timed_out) {
    kill(job.pid, SIGKILL);
  }
  int status = 0;
  (void)waitpid(job.pid, &status, 0);

  job.buf[job.len] = '\0';
  const bool ok = !timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  job.done(job.user, ok, job.buf);
  free(job.buf);
}

int jobs_fill(struct pollfd *fds, int max) {
  int n = 0;
  for (int i = 0; i < JOBS_MAX && n < max; i++) {
    if (!g_jobs[i].used) {
      continue;
    }
    fds[n].fd = g_jobs[i].fd;
    fds[n].events = POLLIN;
    fds[n].revents = 0;
    g_fill_slot[n] = i;
    n++;
  }
  return n;
}

void jobs_handle(const struct pollfd *fds, int count) {
  for (int i = 0; i < count; i++) {
    if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
      continue;
    }
    const int slot = g_fill_slot[i];
    job_t *job = &g_jobs[slot];
    if (!job->used || job->fd != fds[i].fd) {
      continue;
    }

    bool eof = false;
    for (;;) {
      if (job->len >= JOB_OUTPUT_MAX) {
        /* keep draining so the child never blocks, but drop the excess */
        char sink[1024];
        const ssize_t k = read(job->fd, sink, sizeof(sink));
        if (k > 0) {
          continue;
        }
        eof = k == 0;
        break;
      }
      const ssize_t k =
          read(job->fd, job->buf + job->len, JOB_OUTPUT_MAX - job->len);
      if (k > 0) {
        job->len += (size_t)k;
        continue;
      }
      if (k < 0 && errno == EINTR) {
        continue;
      }
      eof = k == 0 || (k < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
      break;
    }
    if (eof) {
      finish(slot, false);
    }
  }

  const long now = now_ms();
  for (int i = 0; i < JOBS_MAX; i++) {
    if (g_jobs[i].used && now >= g_jobs[i].deadline_ms) {
      finish(i, true);
    }
  }
}

int jobs_timeout_ms(void) {
  long best = -1;
  const long now = now_ms();
  for (int i = 0; i < JOBS_MAX; i++) {
    if (!g_jobs[i].used) {
      continue;
    }
    long left = g_jobs[i].deadline_ms - now;
    if (left < 0) {
      left = 0;
    }
    if (best < 0 || left < best) {
      best = left;
    }
  }
  return best < 0 ? -1 : (best > 1000 ? 1000 : (int)best);
}

bool jobs_running(void) {
  for (int i = 0; i < JOBS_MAX; i++) {
    if (g_jobs[i].used) {
      return true;
    }
  }
  return false;
}
