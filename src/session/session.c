#define _POSIX_C_SOURCE 200809L

#include "brickdesk/app.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/prctl.h>
#endif

#include "app_protocol.h"

#ifndef BRICKDESK_STATE_DIR
#define BRICKDESK_STATE_DIR "/mnt/SDCARD/brickdesk/state"
#endif

#define BD_SESSION_FALLBACK_SOCKET_DIR "/tmp"
#define BD_SESSION_FALLBACK_STATE_DIR "/tmp/brickdesk-state"
#define BD_SESSION_POLL_SLICE_MS 100
#define BD_SESSION_MENU_CLIENTS 4
#define BD_SESSION_MENU_LINE 4096

typedef struct session_menu_client {
  int fd;
  char buffer[BD_SESSION_MENU_LINE];
  size_t length;
  uint64_t deadline_ms;
} session_menu_client;

typedef enum session_op {
  OP_NONE = 0,
  OP_BG_DISPLAY,
  OP_BG_PAUSE,
  OP_RS_DISPLAY,
  OP_RS_RESUME,
  OP_TERM_SAVE,
  OP_TERM_QUIT,
  OP_TERM_WAIT_EXIT,
  OP_TERM_SIGTERM,
  OP_TERM_SIGKILL,
  OP_SAVE_STATE,
  OP_LOAD_STATE
} session_op;

typedef enum session_child_status {
  SESSION_CHILD_RUNNING = 0,
  SESSION_CHILD_STOPPED,
  SESSION_CHILD_EXITED,
  SESSION_CHILD_TIMEOUT,
  SESSION_CHILD_UNKNOWN
} session_child_status;

/*
 * Light-sleep stop ownership.  HELD means a confirmed SIGSTOP this session
 * sent; RECOVERY means a failed freeze rollback whose SIGCONT was rejected, so
 * thaw() must retry it.  Both states are reported as `frozen` publicly.
 */
typedef enum session_freeze_state {
  SESSION_FREEZE_NONE = 0,
  SESSION_FREEZE_HELD,
  SESSION_FREEZE_RECOVERY
} session_freeze_state;

typedef enum session_continue_result {
  SESSION_CONTINUE_OK = 0,
  SESSION_CONTINUE_EXITED,
  SESSION_CONTINUE_FAILED
} session_continue_result;

typedef struct session_instance {
  bd_session *session;
  bd_app app;
  int pid;
  int pgid;
  bd_session_state state;
  bd_app_protocol *protocol;
  char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
  char session_id[64];
  char state_dir[BD_APP_PATH_MAX];
  char state_file[BD_APP_PATH_MAX];
  char frame_path[BD_APP_PATH_MAX];

  bool ipc_connected;
  bool hello_received;
  unsigned hello_capabilities;
  bool stopped_by_session;
  session_freeze_state freeze;
  bool display_released;
  bool perf_applied;
  bool reaped;

  uint64_t started_ms;
  uint64_t backgrounded_ms;
  uint64_t last_active_ms;
  uint64_t hello_deadline_ms;

  int exit_code;
  int term_signal;

  session_op op;
  int request_id;
  uint64_t op_deadline_ms;
  char request_hook[24];

  /* Asynchronous command hook (hooks.<name>.type = "command"). */
  pid_t hook_pid;
  int hook_fd;
  uint64_t hook_started_ms;
  uint64_t hook_deadline_ms;
  int hook_exit_code;
  bool hook_timed_out;
  char hook_output[1024];
  size_t hook_output_length;
} session_instance;

struct bd_session {
  int menu_fd;
  char menu_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
  session_menu_client menu_clients[BD_SESSION_MENU_CLIENTS];

  char socket_dir[BD_APP_PATH_MAX];
  char frame_dir[BD_APP_PATH_MAX];
  char state_root[BD_APP_PATH_MAX];
  char session_id[32];
  int max_background;
  int state_quota_mb;
  char last_error[256];
  bd_session_timeouts timeouts;
  bd_session_perf_ops perf;
  bd_session_event_fn on_event;
  void *event_user;

  session_instance **items;
  size_t count;
  size_t capacity;
  unsigned sequence;
};

static void menu_poll(bd_session *session, uint64_t now_ms);
static void menu_shutdown(bd_session *session);
static session_instance *find_instance(bd_session *session, const char *app_id);
static session_child_status poll_child_status(session_instance *instance);
static bool start_command_hook(bd_session *session, session_instance *instance,
                               session_op op, const char *hook_name,
                               int timeout_ms, int slot);
static void command_hook_cancel(session_instance *instance);

static uint64_t monotonic_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void set_error(char *error, size_t error_size, const char *format, ...) {
  if (error == NULL || error_size == 0) {
    return;
  }
  va_list args;
  va_start(args, format);
  vsnprintf(error, error_size, format, args);
  va_end(args);
}

static bool make_dirs(const char *path, mode_t mode) {
  if (path == NULL || path[0] == '\0') {
    return false;
  }
  char buffer[BD_APP_PATH_MAX];
  int written = snprintf(buffer, sizeof(buffer), "%s", path);
  if (written <= 0 || (size_t)written >= sizeof(buffer)) {
    return false;
  }
  for (char *p = buffer + 1; *p != '\0'; ++p) {
    if (*p != '/') {
      continue;
    }
    *p = '\0';
    if (mkdir(buffer, mode) != 0 && errno != EEXIST) {
      *p = '/';
      return false;
    }
    *p = '/';
  }
  if (mkdir(buffer, mode) != 0 && errno != EEXIST) {
    return false;
  }
  struct stat status;
  return stat(buffer, &status) == 0 && S_ISDIR(status.st_mode);
}

static uint64_t directory_bytes(const char *path, uint64_t limit) {
  DIR *dir = opendir(path);
  if (dir == NULL) {
    return 0;
  }
  uint64_t total = 0;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    char child[BD_APP_PATH_MAX];
    int written = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    if (written <= 0 || (size_t)written >= sizeof(child)) {
      continue;
    }
    struct stat status;
    if (lstat(child, &status) != 0) {
      continue;
    }
    if (S_ISDIR(status.st_mode)) {
      total += directory_bytes(child, limit - (total < limit ? total : limit));
    } else if (S_ISREG(status.st_mode)) {
      if ((uint64_t)status.st_size > UINT64_MAX - total) {
        total = UINT64_MAX;
      } else {
        total += (uint64_t)status.st_size;
      }
    }
    if (total > limit) {
      break;
    }
  }
  closedir(dir);
  return total;
}

/*
 * Enforces session.state_quota_mb per app before a cold launch or a resume.
 * It only reads directory metadata: no file is removed or truncated.
 */
static bool state_quota_check(bd_session *session, const char *app_id,
                              char *error, size_t error_size) {
  if (session == NULL || app_id == NULL || session->state_quota_mb <= 0) {
    return true;
  }
  char dir[BD_APP_PATH_MAX];
  int written = snprintf(dir, sizeof(dir), "%s/%s", session->state_root, app_id);
  if (written <= 0 || (size_t)written >= sizeof(dir)) {
    set_error(error, error_size, "state path is too long for '%s'", app_id);
    snprintf(session->last_error, sizeof(session->last_error),
             "state path is too long");
    return false;
  }
  uint64_t limit = (uint64_t)session->state_quota_mb * 1024u * 1024u;
  uint64_t used = directory_bytes(dir, limit);
  if (used <= limit) {
    return true;
  }
  unsigned long long used_mb = (unsigned long long)(used / (1024u * 1024u));
  set_error(error, error_size,
            "state quota exceeded for '%s': %llu MiB used of %d MiB; "
            "delete old saves or raise session.state_quota_mb",
            app_id, used_mb, session->state_quota_mb);
  snprintf(session->last_error, sizeof(session->last_error),
           "存档配额已满：%s 已用 %llu MB，上限 %d MB。请删除旧存档或提高配额。",
           app_id, used_mb, session->state_quota_mb);
  return false;
}

static bool write_full(int fd, const char *data, size_t length) {
  size_t written = 0;
  while (written < length) {
    ssize_t amount = write(fd, data + written, length - written);
    if (amount < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    written += (size_t)amount;
  }
  return true;
}

static void child_fail(int fd, int code, const char *format, ...) {
  char message[256];
  va_list args;
  va_start(args, format);
  int length = vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  if (length > 0) {
    write_full(fd, message, (size_t)length);
  }
  _exit(code);
}

static void child_exec(const session_instance *instance, int error_fd) {
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = SIG_DFL;
  sigemptyset(&action.sa_mask);
  const int signals[] = {SIGTERM, SIGINT, SIGHUP, SIGQUIT, SIGPIPE, SIGCHLD,
                         SIGUSR1, SIGUSR2};
  for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) {
    sigaction(signals[i], &action, NULL);
  }
  sigset_t empty;
  sigemptyset(&empty);
  sigprocmask(SIG_SETMASK, &empty, NULL);

  if (setsid() < 0) {
    child_fail(error_fd, 126, "setsid: %s", strerror(errno));
  }
#if defined(__linux__)
  prctl(PR_SET_PDEATHSIG, SIGTERM);
  if (getppid() == 1) {
    child_fail(error_fd, 126, "parent exited before exec");
  }
#endif

  for (size_t i = 0; i < instance->app.env_count; ++i) {
    setenv(instance->app.env_keys[i], instance->app.env_values[i], 1);
  }
  setenv("BRICKDESK_APP_ID", instance->app.id, 1);
  setenv("BRICKDESK_SESSION_ID", instance->session_id, 1);
  setenv("BRICKDESK_SESSION_SOCKET", instance->socket_path, 1);
  setenv("BRICKDESK_STATE_DIR", instance->state_dir, 1);
  if (instance->state_file[0] != '\0') {
    setenv("BRICKDESK_STATE_FILE", instance->state_file, 1);
  }
  setenv("BRICKDESK_MENU_MODE",
         bd_app_menu_mode_name(instance->app.menu.mode), 1);
  if (instance->session->menu_path[0] != '\0') {
    setenv("BRICKDESK_MENU_SOCKET", instance->session->menu_path, 1);
  }
  if (instance->frame_path[0] != '\0') {
    setenv("BRICKDESK_LAST_FRAME", instance->frame_path, 1);
  }

  if (chdir(instance->app.cwd) != 0) {
    child_fail(error_fd, 126, "chdir %s: %s", instance->app.cwd,
               strerror(errno));
  }

  char **argv = (char **)calloc(instance->app.exec_count + 1, sizeof(char *));
  if (argv == NULL) {
    child_fail(error_fd, 126, "out of memory building argv");
  }
  for (size_t i = 0; i < instance->app.exec_count; ++i) {
    argv[i] = instance->app.exec[i];
  }
  argv[instance->app.exec_count] = NULL;
  if (strchr(argv[0], '/') != NULL) {
    execv(argv[0], argv);
  } else {
    execvp(argv[0], argv);
  }
  int saved = errno;
  free(argv);
  child_fail(error_fd, 127, "%s: %s", instance->app.exec[0],
             strerror(saved));
}

static bool spawn_app(session_instance *instance, char *error,
                      size_t error_size) {
  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) {
    set_error(error, error_size, "pipe: %s", strerror(errno));
    return false;
  }
  fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC);

  pid_t pid = fork();
  if (pid < 0) {
    set_error(error, error_size, "fork: %s", strerror(errno));
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return false;
  }
  if (pid == 0) {
    close(pipe_fds[0]);
    child_exec(instance, pipe_fds[1]);
    _exit(127);
  }

  close(pipe_fds[1]);
  char buffer[512];
  size_t used = 0;
  for (;;) {
    ssize_t amount = read(pipe_fds[0], buffer + used, sizeof(buffer) - 1 - used);
    if (amount < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (amount == 0) {
      break;
    }
    used += (size_t)amount;
    if (used >= sizeof(buffer) - 1) {
      break;
    }
  }
  close(pipe_fds[0]);

  if (used > 0) {
    buffer[used] = '\0';
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    set_error(error, error_size, "%s", buffer);
    return false;
  }

  instance->pid = (int)pid;
  instance->pgid = (int)pid;
  instance->reaped = false;
  return true;
}

static bool apply_perf(bd_session *session, session_instance *instance) {
  if (session->perf.apply == NULL) {
    instance->perf_applied = false;
    return true;
  }
  const char *profile = instance->app.perf.profile[0] != '\0'
                            ? instance->app.perf.profile
                            : "balanced";
  bool ok = session->perf.apply(session->perf.user, profile,
                                instance->app.perf.core);
  instance->perf_applied = ok;
  return ok;
}

static void restore_perf(bd_session *session, session_instance *instance) {
  if (!instance->perf_applied) {
    return;
  }
  if (session->perf.restore != NULL) {
    session->perf.restore(session->perf.user);
  }
  instance->perf_applied = false;
}

static void emit_event(bd_session *session, session_instance *instance,
                       bd_session_event_kind kind, bd_session_state previous) {
  if (session->on_event == NULL) {
    return;
  }
  bd_session_event event;
  memset(&event, 0, sizeof(event));
  event.kind = kind;
  event.app_id = instance->app.id;
  event.pid = instance->pid;
  event.previous = previous;
  event.state = instance->state;
  event.exit_code = instance->exit_code;
  event.term_signal = instance->term_signal;
  session->on_event(session->event_user, &event);
}

static void set_state(bd_session *session, session_instance *instance,
                      bd_session_state state) {
  if (instance->state == state) {
    return;
  }
  bd_session_state previous = instance->state;
  instance->state = state;
  uint64_t now = monotonic_ms();
  if (state == BD_SESSION_BACKGROUND) {
    instance->backgrounded_ms = now;
  } else if (state == BD_SESSION_FOREGROUND) {
    instance->last_active_ms = now;
    instance->display_released = false;
  }
  emit_event(session, instance, BD_SESSION_EVENT_STATE, previous);
}

static void emit_hook(bd_session *session, session_instance *instance,
                      const char *hook, bool ok) {
  if (session->on_event == NULL) {
    return;
  }
  bd_session_event event;
  memset(&event, 0, sizeof(event));
  event.kind = BD_SESSION_EVENT_HOOK;
  event.app_id = instance->app.id;
  event.pid = instance->pid;
  event.previous = instance->state;
  event.state = instance->state;
  event.hook = hook;
  event.hook_ok = ok;
  session->on_event(session->event_user, &event);
}

static void emit_notification(bd_session *session, session_instance *instance,
                              const char *method, const char *params) {
  if (session->on_event == NULL) {
    return;
  }
  bd_session_event event;
  memset(&event, 0, sizeof(event));
  event.kind = BD_SESSION_EVENT_NOTIFICATION;
  event.app_id = instance->app.id;
  event.pid = instance->pid;
  event.previous = instance->state;
  event.state = instance->state;
  event.method = method;
  event.params = params;
  session->on_event(session->event_user, &event);
}

static void emit_protocol_error(bd_session *session,
                                session_instance *instance,
                                const char *message) {
  if (session->on_event == NULL) {
    return;
  }
  bd_session_event event;
  memset(&event, 0, sizeof(event));
  event.kind = BD_SESSION_EVENT_PROTOCOL_ERROR;
  event.app_id = instance->app.id;
  event.pid = instance->pid;
  event.previous = instance->state;
  event.state = instance->state;
  event.method = message;
  session->on_event(session->event_user, &event);
}

static int hook_timeout(const bd_session *session,
                        const session_instance *instance,
                        bd_app_hook_kind kind, int fallback) {
  (void)session;
  int configured = instance->app.hooks[kind].timeout_ms;
  return configured > 0 ? configured : fallback;
}

static bool send_signal(session_instance *instance, int signal_number) {
  if (instance->pgid > 0) {
    return kill(-instance->pgid, signal_number) == 0;
  }
  if (instance->pid > 0) {
    return kill(instance->pid, signal_number) == 0;
  }
  return false;
}

static void finish_foreground(bd_session *session, session_instance *instance) {
  instance->op = OP_NONE;
  instance->request_id = -1;
  instance->hello_deadline_ms = 0;
  instance->display_released = false;
  set_state(session, instance, BD_SESSION_FOREGROUND);
}

static void finish_background(bd_session *session,
                              session_instance *instance) {
  instance->op = OP_NONE;
  instance->request_id = -1;
  restore_perf(session, instance);
  set_state(session, instance, BD_SESSION_BACKGROUND);
}

static void start_sigstop(bd_session *session, session_instance *instance) {
  if (send_signal(instance, SIGSTOP)) {
    instance->stopped_by_session = true;
  }
  finish_background(session, instance);
}

/* Continues a light-sleep-frozen foreground app before a normal transition. */
static session_continue_result continue_frozen(session_instance *instance) {
  if (instance->freeze == SESSION_FREEZE_NONE) {
    return SESSION_CONTINUE_OK;
  }
  if (!send_signal(instance, SIGCONT)) {
    int saved = errno;
    if (saved == ESRCH) {
      poll_child_status(instance);
      if (instance->reaped) {
        return SESSION_CONTINUE_EXITED;
      }
    }
    /* Keep ownership: a later thaw/transition may deliver the SIGCONT. */
    return SESSION_CONTINUE_FAILED;
  }
  instance->freeze = SESSION_FREEZE_NONE;
  return SESSION_CONTINUE_OK;
}

static bool begin_request(bd_session *session, session_instance *instance,
                          session_op op, const char *method,
                          const char *params, int timeout,
                          const char *hook) {
  (void)session;
  if (instance->request_id >= 0 || instance->protocol == NULL ||
      !instance->ipc_connected) {
    return false;
  }
  int id = bd_app_protocol_request(instance->protocol, method, params, timeout);
  if (id <= 0 || !bd_app_protocol_connected(instance->protocol)) {
    return false;
  }
  instance->request_id = id;
  instance->op = op;
  snprintf(instance->request_hook, sizeof(instance->request_hook), "%s", hook);
  return true;
}

static void start_pause(bd_session *session, session_instance *instance) {
  if (instance->app.hooks[BD_APP_HOOK_PAUSE].type == BD_APP_HOOK_COMMAND) {
    int timeout = hook_timeout(session, instance, BD_APP_HOOK_PAUSE,
                               session->timeouts.pause_ms);
    if (start_command_hook(session, instance, OP_BG_PAUSE, "pause", timeout,
                           -1)) {
      return;
    }
  }
  if (instance->app.hooks[BD_APP_HOOK_PAUSE].type == BD_APP_HOOK_IPC &&
      instance->ipc_connected) {
    char params[64];
    snprintf(params, sizeof(params), "%s", "{}");
    int timeout = hook_timeout(session, instance, BD_APP_HOOK_PAUSE,
                               session->timeouts.pause_ms);
    if (begin_request(session, instance, OP_BG_PAUSE,
                      instance->app.hooks[BD_APP_HOOK_PAUSE].method, params,
                      timeout, "pause")) {
      return;
    }
  }
  start_sigstop(session, instance);
}

static void start_background(bd_session *session, session_instance *instance) {
  if (instance->app.caps.display_release && instance->ipc_connected) {
    if (begin_request(session, instance, OP_BG_DISPLAY, "app.display_release",
                      "{}", session->timeouts.display_ms, "display_release")) {
      return;
    }
  }
  start_pause(session, instance);
}

static void start_resume_hook(bd_session *session,
                              session_instance *instance) {
  if (instance->app.hooks[BD_APP_HOOK_RESUME].type == BD_APP_HOOK_COMMAND) {
    int timeout = hook_timeout(session, instance, BD_APP_HOOK_RESUME,
                               session->timeouts.resume_ms);
    if (start_command_hook(session, instance, OP_RS_RESUME, "resume", timeout,
                           -1)) {
      return;
    }
  }
  if (instance->app.hooks[BD_APP_HOOK_RESUME].type == BD_APP_HOOK_IPC &&
      instance->ipc_connected) {
    int timeout = hook_timeout(session, instance, BD_APP_HOOK_RESUME,
                               session->timeouts.resume_ms);
    if (begin_request(session, instance, OP_RS_RESUME,
                      instance->app.hooks[BD_APP_HOOK_RESUME].method, "{}",
                      timeout, "resume")) {
      return;
    }
  }
  finish_foreground(session, instance);
}

static void start_resume(bd_session *session, session_instance *instance) {
  set_state(session, instance, BD_SESSION_RESUMING);
  apply_perf(session, instance);
  if (instance->stopped_by_session) {
    send_signal(instance, SIGCONT);
    instance->stopped_by_session = false;
  }
  if (instance->app.caps.display_release && instance->ipc_connected) {
    if (begin_request(session, instance, OP_RS_DISPLAY, "app.display_acquire",
                      "{}", session->timeouts.display_ms,
                      "display_acquire")) {
      return;
    }
  }
  start_resume_hook(session, instance);
}

static void start_sigterm(bd_session *session, session_instance *instance) {
  send_signal(instance, SIGTERM);
  instance->request_id = -1;
  instance->op = OP_TERM_SIGTERM;
  instance->op_deadline_ms =
      monotonic_ms() + (uint64_t)session->timeouts.term_ms;
}

static void start_sigkill(bd_session *session, session_instance *instance) {
  send_signal(instance, SIGKILL);
  instance->request_id = -1;
  instance->op = OP_TERM_SIGKILL;
  instance->op_deadline_ms =
      monotonic_ms() + (uint64_t)session->timeouts.kill_ms;
}

static void start_quit(bd_session *session, session_instance *instance) {
  if (instance->app.hooks[BD_APP_HOOK_QUIT].type == BD_APP_HOOK_COMMAND) {
    int timeout = hook_timeout(session, instance, BD_APP_HOOK_QUIT,
                               session->timeouts.quit_ms);
    if (start_command_hook(session, instance, OP_TERM_QUIT, "quit", timeout,
                           -1)) {
      return;
    }
  }
  if (instance->app.hooks[BD_APP_HOOK_QUIT].type == BD_APP_HOOK_IPC &&
      instance->ipc_connected) {
    int timeout = hook_timeout(session, instance, BD_APP_HOOK_QUIT,
                               session->timeouts.quit_ms);
    if (begin_request(session, instance, OP_TERM_QUIT,
                      instance->app.hooks[BD_APP_HOOK_QUIT].method,
                      "{\"save\":true}", timeout, "quit")) {
      return;
    }
  }
  start_sigterm(session, instance);
}

static bd_session_result begin_terminate(bd_session *session,
                                         session_instance *instance) {
  session_continue_result resumed = continue_frozen(instance);
  if (resumed == SESSION_CONTINUE_FAILED) {
    return BD_SESSION_IO_ERROR;
  }
  if (resumed == SESSION_CONTINUE_EXITED) {
    return BD_SESSION_OK;
  }
  set_state(session, instance, BD_SESSION_TERMINATING);
  if (instance->stopped_by_session) {
    send_signal(instance, SIGCONT);
    instance->stopped_by_session = false;
  }
  if (instance->app.hooks[BD_APP_HOOK_SAVE].type == BD_APP_HOOK_COMMAND) {
    int timeout = hook_timeout(session, instance, BD_APP_HOOK_SAVE,
                               session->timeouts.save_ms);
    if (start_command_hook(session, instance, OP_TERM_SAVE, "save", timeout,
                           0)) {
      return BD_SESSION_OK;
    }
  }
  if (instance->app.hooks[BD_APP_HOOK_SAVE].type == BD_APP_HOOK_IPC &&
      instance->ipc_connected) {
    int timeout = hook_timeout(session, instance, BD_APP_HOOK_SAVE,
                               session->timeouts.save_ms);
    if (begin_request(session, instance, OP_TERM_SAVE,
                      instance->app.hooks[BD_APP_HOOK_SAVE].method,
                      "{\"slot\":0}", timeout, "save")) {
      return BD_SESSION_OK;
    }
  }
  start_quit(session, instance);
  return BD_SESSION_OK;
}

static void do_finalize(bd_session *session, session_instance *instance,
                        int status, bool status_valid) {
  if (instance->reaped) {
    return;
  }
  instance->reaped = true;
  instance->exit_code = -1;
  instance->term_signal = 0;
  instance->freeze = SESSION_FREEZE_NONE;
  bool exited_normally = false;
  if (status_valid) {
    if (WIFEXITED(status)) {
      instance->exit_code = WEXITSTATUS(status);
      exited_normally = true;
    } else if (WIFSIGNALED(status)) {
      instance->term_signal = WTERMSIG(status);
    }
  } else {
    instance->term_signal = SIGKILL;
  }

  bool terminating = instance->state == BD_SESSION_TERMINATING;
  bool crashed = !terminating &&
                 (!exited_normally || instance->exit_code != 0);

  command_hook_cancel(instance);
  if (instance->protocol != NULL) {
    bd_app_protocol_destroy(instance->protocol);
    instance->protocol = NULL;
  }
  restore_perf(session, instance);
  instance->request_id = -1;
  instance->op = OP_NONE;
  instance->ipc_connected = false;

  bd_session_state previous = instance->state;
  instance->state = crashed ? BD_SESSION_CRASHED : BD_SESSION_EXITED;
  emit_event(session, instance, BD_SESSION_EVENT_STATE, previous);
  emit_event(session, instance,
             crashed ? BD_SESSION_EVENT_CRASH : BD_SESSION_EVENT_EXIT,
             previous);
}

static void force_reap(bd_session *session, session_instance *instance) {
  int status = 0;
  pid_t result;
  do {
    result = waitpid(instance->pid, &status, 0);
  } while (result < 0 && errno == EINTR);
  if (result == instance->pid) {
    do_finalize(session, instance, status, true);
  } else {
    do_finalize(session, instance, 0, false);
  }
}

static void sleep_slice(void) {
  struct timespec delay = {0, 1000000L};
  nanosleep(&delay, NULL);
}

static session_child_status poll_child_status(session_instance *instance) {
  if (instance->reaped || instance->pid <= 0) {
    return SESSION_CHILD_EXITED;
  }
  for (;;) {
    int status = 0;
    pid_t result = waitpid(instance->pid, &status, WNOHANG | WUNTRACED);
    if (result == instance->pid) {
      if (WIFSTOPPED(status)) {
        return SESSION_CHILD_STOPPED;
      }
      if (WIFEXITED(status) || WIFSIGNALED(status)) {
        do_finalize(instance->session, instance, status, true);
        return SESSION_CHILD_EXITED;
      }
      continue;
    }
    if (result == 0) {
      return SESSION_CHILD_RUNNING;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == ECHILD) {
      do_finalize(instance->session, instance, 0, false);
      return SESSION_CHILD_EXITED;
    }
    return SESSION_CHILD_UNKNOWN;
  }
}

static session_child_status wait_child_status(session_instance *instance,
                                              uint64_t deadline_ms) {
  for (;;) {
    session_child_status status = poll_child_status(instance);
    if (status != SESSION_CHILD_RUNNING) {
      return status;
    }
    if (monotonic_ms() >= deadline_ms) {
      return SESSION_CHILD_TIMEOUT;
    }
    sleep_slice();
  }
}

static void check_child(bd_session *session, session_instance *instance) {
  (void)session;
  if (instance->reaped || instance->pid <= 0) {
    return;
  }
  if (poll_child_status(instance) == SESSION_CHILD_STOPPED) {
    instance->stopped_by_session = true;
  }
}

static void complete_op(bd_session *session, session_instance *instance,
                        session_op op, const char *hook, bool ok) {
  switch (op) {
    case OP_BG_DISPLAY:
      if (ok) {
        instance->display_released = true;
      }
      emit_hook(session, instance, hook, ok);
      start_pause(session, instance);
      break;
    case OP_BG_PAUSE:
      emit_hook(session, instance, hook, ok);
      if (ok) {
        instance->display_released = true;
        finish_background(session, instance);
      } else {
        start_sigstop(session, instance);
      }
      break;
    case OP_RS_DISPLAY:
      emit_hook(session, instance, hook, ok);
      start_resume_hook(session, instance);
      break;
    case OP_RS_RESUME:
      emit_hook(session, instance, hook, ok);
      finish_foreground(session, instance);
      break;
    case OP_TERM_SAVE:
      emit_hook(session, instance, hook, ok);
      start_quit(session, instance);
      break;
    case OP_TERM_QUIT:
      emit_hook(session, instance, hook, ok);
      if (ok) {
        instance->op = OP_TERM_WAIT_EXIT;
        instance->op_deadline_ms =
            monotonic_ms() + (uint64_t)session->timeouts.term_ms;
      } else {
        start_sigterm(session, instance);
      }
      break;
    case OP_SAVE_STATE:
    case OP_LOAD_STATE:
      emit_hook(session, instance, hook, ok);
      break;
    default:
      break;
  }
}

static void complete_request(bd_session *session, session_instance *instance,
                             bool ok) {
  if (instance->request_id < 0) {
    return;
  }
  session_op op = instance->op;
  char hook[sizeof(instance->request_hook)];
  snprintf(hook, sizeof(hook), "%s", instance->request_hook);
  instance->request_id = -1;
  instance->op = OP_NONE;
  complete_op(session, instance, op, hook, ok);
}

static void command_hook_append(session_instance *instance, const char *data,
                                size_t length) {
  size_t room = sizeof(instance->hook_output) - instance->hook_output_length;
  if (length >= room) {
    length = room > 0 ? room - 1 : 0;
  }
  if (length > 0) {
    memcpy(instance->hook_output + instance->hook_output_length, data, length);
    instance->hook_output_length += length;
  }
  instance->hook_output[instance->hook_output_length] = '\0';
}

static void command_hook_log(bd_session *session, session_instance *instance,
                             const char *hook_name, int exit_code,
                             bool timed_out) {
  char path[BD_APP_PATH_MAX];
  int written = snprintf(path, sizeof(path), "%s/hooks.log",
                         session->state_root);
  if (written <= 0 || (size_t)written >= sizeof(path)) {
    return;
  }
  struct stat status;
  if (stat(path, &status) == 0 && status.st_size > 512 * 1024) {
    char rotated[BD_APP_PATH_MAX + 8];
    int rotated_written =
        snprintf(rotated, sizeof(rotated), "%s.1", path);
    if (rotated_written > 0 && (size_t)rotated_written < sizeof(rotated)) {
      (void)rename(path, rotated);
    }
  }
  int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (fd < 0) {
    return;
  }
  char output[sizeof(instance->hook_output)];
  size_t used = 0;
  for (size_t i = 0; i < instance->hook_output_length &&
                     used + 1 < sizeof(output);
       ++i) {
    char c = instance->hook_output[i];
    if (c == '\n' || c == '\r' || c == '\t') {
      c = ' ';
    }
    output[used++] = c;
  }
  output[used] = '\0';
  time_t wall = time(NULL);
  struct tm local;
  char stamp[32] = "?";
  if (wall != (time_t)-1 && localtime_r(&wall, &local) != NULL) {
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &local);
  }
  uint64_t now = monotonic_ms();
  uint64_t duration =
      instance->hook_started_ms != 0 && now > instance->hook_started_ms
          ? now - instance->hook_started_ms
          : 0;
  dprintf(fd, "%s app=%s hook=%s exit=%d timeout=%d duration_ms=%llu output=\"%s\"\n",
          stamp, instance->app.id, hook_name != NULL ? hook_name : "?",
          exit_code, timed_out ? 1 : 0, (unsigned long long)duration, output);
  close(fd);
}

static bool start_command_hook(bd_session *session, session_instance *instance,
                               session_op op, const char *hook_name,
                               int timeout_ms, int slot) {
  (void)session;
  bd_app_hook_kind kind = BD_APP_HOOK_PAUSE;
  bool found = false;
  for (int i = 0; i < BD_APP_HOOK_COUNT; ++i) {
    if (strcmp(bd_app_hook_name((bd_app_hook_kind)i), hook_name) == 0) {
      kind = (bd_app_hook_kind)i;
      found = true;
      break;
    }
  }
  if (!found) {
    return false;
  }
  const bd_app_hook *hook = &instance->app.hooks[kind];
  if (hook->type != BD_APP_HOOK_COMMAND || hook->exec == NULL ||
      hook->exec_count == 0) {
    return false;
  }
  if (instance->hook_pid > 0 || instance->op != OP_NONE ||
      instance->request_id >= 0) {
    return false;
  }
  if (timeout_ms < 50) {
    timeout_ms = 50;
  } else if (timeout_ms > 600000) {
    timeout_ms = 600000;
  }

  int pipe_fds[2];
  if (pipe(pipe_fds) != 0) {
    return false;
  }
  fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC);

  pid_t pid = fork();
  if (pid < 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return false;
  }
  if (pid == 0) {
    close(pipe_fds[0]);
    setpgid(0, 0);
#if defined(__linux__)
    prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
    if (pipe_fds[1] != STDOUT_FILENO) {
      dup2(pipe_fds[1], STDOUT_FILENO);
    }
    if (pipe_fds[1] != STDERR_FILENO) {
      dup2(pipe_fds[1], STDERR_FILENO);
    }
    if (pipe_fds[1] > STDERR_FILENO) {
      close(pipe_fds[1]);
    }
    for (size_t i = 0; i < instance->app.env_count; ++i) {
      setenv(instance->app.env_keys[i], instance->app.env_values[i], 1);
    }
    setenv("BRICKDESK_APP_ID", instance->app.id, 1);
    setenv("BRICKDESK_HOOK", hook_name != NULL ? hook_name : "", 1);
    setenv("BRICKDESK_SESSION_ID", instance->session_id, 1);
    setenv("BRICKDESK_STATE_DIR", instance->state_dir, 1);
    if (instance->socket_path[0] != '\0') {
      setenv("BRICKDESK_SESSION_SOCKET", instance->socket_path, 1);
    }
    char slot_text[16];
    if (slot >= 0) {
      snprintf(slot_text, sizeof(slot_text), "%d", slot);
      setenv("BRICKDESK_SLOT", slot_text, 1);
    }
    if (chdir(instance->app.cwd) != 0) {
      dprintf(STDERR_FILENO, "chdir %s: %s\n", instance->app.cwd,
              strerror(errno));
      _exit(126);
    }
    char **argv = (char **)calloc(hook->exec_count + 1, sizeof(char *));
    if (argv == NULL) {
      dprintf(STDERR_FILENO, "out of memory building hook argv\n");
      _exit(126);
    }
    for (size_t i = 0; i < hook->exec_count; ++i) {
      argv[i] = hook->exec[i];
    }
    argv[hook->exec_count] = NULL;
    if (strchr(argv[0], '/') != NULL) {
      execv(argv[0], argv);
    } else {
      execvp(argv[0], argv);
    }
    dprintf(STDERR_FILENO, "%s: %s\n", argv[0], strerror(errno));
    _exit(127);
  }

  close(pipe_fds[1]);
  (void)setpgid(pid, pid);
  int flags = fcntl(pipe_fds[0], F_GETFL, 0);
  if (flags >= 0) {
    fcntl(pipe_fds[0], F_SETFL, flags | O_NONBLOCK);
  }
  instance->hook_pid = pid;
  instance->hook_fd = pipe_fds[0];
  instance->hook_started_ms = monotonic_ms();
  instance->hook_deadline_ms =
      instance->hook_started_ms + (uint64_t)timeout_ms;
  instance->hook_exit_code = -1;
  instance->hook_timed_out = false;
  instance->hook_output_length = 0;
  instance->hook_output[0] = '\0';
  instance->request_id = -1;
  instance->op = op;
  snprintf(instance->request_hook, sizeof(instance->request_hook), "%s",
           hook_name != NULL ? hook_name : "");
  return true;
}

static void command_hook_drain(session_instance *instance) {
  if (instance->hook_fd < 0) {
    return;
  }
  char buffer[256];
  for (;;) {
    ssize_t amount = read(instance->hook_fd, buffer, sizeof(buffer));
    if (amount > 0) {
      command_hook_append(instance, buffer, (size_t)amount);
      continue;
    }
    if (amount < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
}

static void command_hook_complete(bd_session *session,
                                  session_instance *instance) {
  session_op op = instance->op;
  char hook[sizeof(instance->request_hook)];
  snprintf(hook, sizeof(hook), "%s", instance->request_hook);
  bool ok = !instance->hook_timed_out && instance->hook_exit_code == 0;
  instance->op = OP_NONE;
  instance->request_id = -1;
  instance->hook_pid = -1;
  if (instance->hook_fd >= 0) {
    close(instance->hook_fd);
    instance->hook_fd = -1;
  }
  complete_op(session, instance, op, hook, ok);
}

static void command_hook_poll(bd_session *session, session_instance *instance,
                              uint64_t now) {
  if (instance->hook_pid <= 0) {
    return;
  }
  command_hook_drain(instance);
  int status = 0;
  pid_t result = waitpid(instance->hook_pid, &status, WNOHANG);
  if (result == 0 && now >= instance->hook_deadline_ms) {
    instance->hook_timed_out = true;
    (void)kill(-instance->hook_pid, SIGKILL);
    (void)kill(instance->hook_pid, SIGKILL);
    do {
      result = waitpid(instance->hook_pid, &status, 0);
    } while (result < 0 && errno == EINTR);
  }
  if (result == 0) {
    return;
  }
  if (result < 0 && errno != ECHILD) {
    return;
  }
  command_hook_drain(instance);
  if (result == instance->hook_pid) {
    if (WIFEXITED(status)) {
      instance->hook_exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      instance->hook_exit_code = 128 + WTERMSIG(status);
    } else {
      instance->hook_exit_code = -1;
    }
  } else {
    instance->hook_exit_code = -1;
  }
  command_hook_log(session, instance, instance->request_hook,
                   instance->hook_exit_code, instance->hook_timed_out);
  command_hook_complete(session, instance);
}

static void command_hook_cancel(session_instance *instance) {
  if (instance == NULL || instance->hook_pid <= 0) {
    return;
  }
  (void)kill(-instance->hook_pid, SIGKILL);
  (void)kill(instance->hook_pid, SIGKILL);
  int status = 0;
  while (waitpid(instance->hook_pid, &status, 0) < 0 && errno == EINTR) {
  }
  instance->hook_pid = -1;
  if (instance->hook_fd >= 0) {
    close(instance->hook_fd);
    instance->hook_fd = -1;
  }
}

static bool notification_allowed(const session_instance *instance,
                                 const char *method) {
  if (method == NULL) {
    return true;
  }
  if (strncmp(method, "rumble.", 7) == 0) {
    return instance->app.caps.rumble;
  }
  if (strncmp(method, "led.", 4) == 0) {
    return instance->app.caps.led_ambient;
  }
  return true;
}

static void on_protocol_event(void *user,
                              const bd_app_protocol_event *event) {
  session_instance *instance = (session_instance *)user;
  bd_session *session = instance->session;

  switch (event->kind) {
    case BD_APP_PROTOCOL_EVENT_HELLO:
      instance->ipc_connected = true;
      instance->hello_received = true;
      instance->hello_capabilities = event->capabilities;
      if (instance->state == BD_SESSION_LAUNCHING) {
        finish_foreground(session, instance);
      }
      break;
    case BD_APP_PROTOCOL_EVENT_RESPONSE:
      if (event->id == instance->request_id) {
        complete_request(session, instance, event->ok);
      }
      break;
    case BD_APP_PROTOCOL_EVENT_TIMEOUT:
      if (event->id == instance->request_id) {
        complete_request(session, instance, false);
      }
      break;
    case BD_APP_PROTOCOL_EVENT_DISCONNECTED:
      instance->ipc_connected = false;
      if (instance->request_id >= 0) {
        complete_request(session, instance, false);
      }
      if (instance->state == BD_SESSION_LAUNCHING) {
        finish_foreground(session, instance);
      }
      break;
    case BD_APP_PROTOCOL_EVENT_REQUEST:
      if (instance->protocol != NULL) {
        bd_app_protocol_reply_error(instance->protocol, event->id,
                                    BD_APP_PROTOCOL_ERROR_UNSUPPORTED,
                                    "method is not implemented by this session");
      }
      if (!notification_allowed(instance, event->method)) {
        emit_protocol_error(session, instance,
                            "capability not declared for this method");
        break;
      }
      emit_notification(session, instance, event->method, event->params);
      break;
    case BD_APP_PROTOCOL_EVENT_NOTIFICATION:
      if (!notification_allowed(instance, event->method)) {
        emit_protocol_error(session, instance,
                            "capability not declared for this method");
        break;
      }
      emit_notification(session, instance, event->method, event->params);
      break;
    case BD_APP_PROTOCOL_EVENT_ERROR:
      emit_protocol_error(session, instance, event->error_message);
      break;
  }
}


static session_instance *find_instance(bd_session *session, const char *app_id) {
  if (session == NULL || app_id == NULL) {
    return NULL;
  }
  for (size_t i = 0; i < session->count; ++i) {
    if (strcmp(session->items[i]->app.id, app_id) == 0) {
      return session->items[i];
    }
  }
  return NULL;
}

static bool instance_active(const session_instance *instance) {
  return instance->state != BD_SESSION_EXITED &&
         instance->state != BD_SESSION_CRASHED;
}

static bool has_foreground(const bd_session *session,
                           const session_instance *except) {
  for (size_t i = 0; i < session->count; ++i) {
    const session_instance *instance = session->items[i];
    if (instance == except) {
      continue;
    }
    switch (instance->state) {
      case BD_SESSION_LAUNCHING:
      case BD_SESSION_FOREGROUND:
      case BD_SESSION_SUSPENDING:
      case BD_SESSION_RESUMING:
      case BD_SESSION_TERMINATING:
        return true;
      default:
        break;
    }
  }
  return false;
}

static void fill_info(const session_instance *instance,
                      bd_session_app_info *out) {
  memset(out, 0, sizeof(*out));
  snprintf(out->app_id, sizeof(out->app_id), "%s", instance->app.id);
  snprintf(out->name, sizeof(out->name), "%s", instance->app.name);
  out->pid = instance->pid;
  out->state = instance->state;
  out->ipc_connected = instance->ipc_connected;
  out->stopped_by_session = instance->stopped_by_session;
  out->frozen = instance->freeze != SESSION_FREEZE_NONE;
  out->display_released = instance->display_released;
  out->background = instance->state == BD_SESSION_BACKGROUND;
  out->exited = instance->state == BD_SESSION_EXITED ||
                instance->state == BD_SESSION_CRASHED;
  out->crashed = instance->state == BD_SESSION_CRASHED;
  out->started_ms = instance->started_ms;
  out->backgrounded_ms = instance->backgrounded_ms;
  out->last_active_ms = instance->last_active_ms;
  out->exit_code = instance->exit_code;
  out->term_signal = instance->term_signal;
}

static void free_instance(bd_session *session, session_instance *instance,
                          bool terminate) {
  if (instance == NULL) {
    return;
  }
  command_hook_cancel(instance);
  if (instance->protocol != NULL) {
    bd_app_protocol_destroy(instance->protocol);
    instance->protocol = NULL;
  }
  if (terminate && !instance->reaped && instance->pid > 0) {
    if (instance->pgid > 0) {
      kill(-instance->pgid, SIGKILL);
    } else {
      kill(instance->pid, SIGKILL);
    }
    int status = 0;
    while (waitpid(instance->pid, &status, 0) < 0 && errno == EINTR) {
    }
    instance->reaped = true;
  }
  restore_perf(session, instance);
  bd_app_destroy(&instance->app);
  free(instance);
}

static bool append_instance(bd_session *session, session_instance *instance) {
  if (session->count == session->capacity) {
    size_t capacity = session->capacity == 0 ? 8 : session->capacity * 2;
    session_instance **items = (session_instance **)realloc(
        session->items, capacity * sizeof(session_instance *));
    if (items == NULL) {
      return false;
    }
    session->items = items;
    session->capacity = capacity;
  }
  session->items[session->count++] = instance;
  return true;
}

static void remove_instance_at(bd_session *session, size_t index,
                               bool terminate) {
  session_instance *instance = session->items[index];
  free_instance(session, instance, terminate);
  memmove(&session->items[index], &session->items[index + 1],
          (session->count - index - 1) * sizeof(session_instance *));
  --session->count;
}

static uint64_t deadline_remaining(uint64_t deadline, uint64_t now) {
  if (deadline == 0) {
    return 0;
  }
  return deadline > now ? deadline - now : 0;
}

static void update_next(uint64_t *next, bool *has_next, uint64_t candidate) {
  if (!*has_next || candidate < *next) {
    *next = candidate;
    *has_next = true;
  }
}


static bool menu_json_field(const char *json, const char *field, char *out,
                            size_t out_size) {
  char pattern[96];
  int written = snprintf(pattern, sizeof(pattern), "\"%s\"", field);
  if (json == NULL || field == NULL || out == NULL || out_size == 0 ||
      written <= 0 || (size_t)written >= sizeof(pattern)) {
    return false;
  }
  const char *cursor = json;
  while ((cursor = strstr(cursor, pattern)) != NULL) {
    const char *after = cursor + strlen(pattern);
    while (*after != '\0' && (*after == ' ' || *after == '\t')) {
      ++after;
    }
    if (*after != ':') {
      cursor = after;
      continue;
    }
    ++after;
    while (*after != '\0' && (*after == ' ' || *after == '\t')) {
      ++after;
    }
    if (*after != '"') {
      return false;
    }
    ++after;
    size_t used = 0;
    while (*after != '\0' && *after != '"') {
      if (*after == '\\' && after[1] != '\0') {
        ++after;
      }
      if (used + 1 < out_size) {
        out[used++] = *after;
      }
      ++after;
    }
    out[used] = '\0';
    return *after == '"';
  }
  return false;
}

static bool menu_socket_create(bd_session *session) {
  if (session->socket_dir[0] == '\0' || session->session_id[0] == '\0') {
    return false;
  }
  int written = snprintf(session->menu_path, sizeof(session->menu_path),
                         "%s/menu-%s.sock", session->socket_dir,
                         session->session_id);
  if (written <= 0 || (size_t)written >= sizeof(session->menu_path)) {
    session->menu_path[0] = '\0';
    return false;
  }
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  if (strlen(session->menu_path) >= sizeof(address.sun_path)) {
    session->menu_path[0] = '\0';
    return false;
  }
  snprintf(address.sun_path, sizeof(address.sun_path), "%s",
           session->menu_path);
  unlink(session->menu_path);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    session->menu_path[0] = '\0';
    return false;
  }
  if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
      listen(fd, 8) != 0) {
    close(fd);
    unlink(session->menu_path);
    session->menu_path[0] = '\0';
    return false;
  }
  chmod(session->menu_path, 0660);
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  session->menu_fd = fd;
  return true;
}

static void menu_shutdown(bd_session *session) {
  for (size_t i = 0; i < BD_SESSION_MENU_CLIENTS; ++i) {
    if (session->menu_clients[i].fd >= 0) {
      close(session->menu_clients[i].fd);
      session->menu_clients[i].fd = -1;
    }
  }
  if (session->menu_fd >= 0) {
    close(session->menu_fd);
    session->menu_fd = -1;
  }
  if (session->menu_path[0] != '\0') {
    unlink(session->menu_path);
    session->menu_path[0] = '\0';
  }
}

static void menu_handle_line(bd_session *session, const char *line) {
  char app[BD_APP_ID_MAX];
  char action[32];
  if (!menu_json_field(line, "app", app, sizeof(app)) ||
      !menu_json_field(line, "action", action, sizeof(action))) {
    return;
  }
  if (strcmp(action, "background") != 0 && strcmp(action, "quit") != 0) {
    return;
  }
  session_instance *instance = find_instance(session, app);
  if (instance == NULL || instance->reaped) {
    return;
  }
  if (session->on_event == NULL) {
    return;
  }
  bd_session_event event;
  memset(&event, 0, sizeof(event));
  event.kind = BD_SESSION_EVENT_MENU_ACTION;
  event.app_id = instance->app.id;
  event.pid = instance->pid;
  event.previous = instance->state;
  event.state = instance->state;
  event.method = action;
  event.params = line;
  session->on_event(session->event_user, &event);
}

static void menu_poll(bd_session *session, uint64_t now_ms) {
  if (session->menu_fd < 0) {
    return;
  }
  for (;;) {
    int fd = accept(session->menu_fd, NULL, NULL);
    if (fd < 0) {
      break;
    }
    int slot = -1;
    for (size_t i = 0; i < BD_SESSION_MENU_CLIENTS; ++i) {
      if (session->menu_clients[i].fd < 0) {
        slot = (int)i;
        break;
      }
    }
    if (slot < 0) {
      close(fd);
      continue;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
      fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    int fd_flags = fcntl(fd, F_GETFD, 0);
    if (fd_flags >= 0) {
      fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
    }
    session_menu_client *client = &session->menu_clients[slot];
    client->fd = fd;
    client->length = 0;
    client->deadline_ms = now_ms + 2000;
  }
  for (size_t i = 0; i < BD_SESSION_MENU_CLIENTS; ++i) {
    session_menu_client *client = &session->menu_clients[i];
    if (client->fd < 0) {
      continue;
    }
    if (now_ms >= client->deadline_ms) {
      close(client->fd);
      client->fd = -1;
      continue;
    }
    for (;;) {
      char chunk[512];
      ssize_t amount = recv(client->fd, chunk, sizeof(chunk), 0);
      if (amount < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          break;
        }
        close(client->fd);
        client->fd = -1;
        break;
      }
      if (amount == 0) {
        char *newline = memchr(client->buffer, '\n', client->length);
        if (newline != NULL) {
          *newline = '\0';
          menu_handle_line(session, client->buffer);
        }
        close(client->fd);
        client->fd = -1;
        break;
      }
      if (client->length + (size_t)amount >= sizeof(client->buffer)) {
        close(client->fd);
        client->fd = -1;
        break;
      }
      memcpy(client->buffer + client->length, chunk, (size_t)amount);
      client->length += (size_t)amount;
      char *newline = memchr(client->buffer, '\n', client->length);
      if (newline == NULL) {
        continue;
      }
      *newline = '\0';
      menu_handle_line(session, client->buffer);
      close(client->fd);
      client->fd = -1;
      break;
    }
  }
}

const char *bd_session_menu_socket(const bd_session *session) {
  if (session == NULL || session->menu_path[0] == '\0') {
    return NULL;
  }
  return session->menu_path;
}

int bd_session_fds(const bd_session *session, int *out, int max) {
  if (session == NULL || out == NULL || max <= 0) {
    return 0;
  }
  int count = 0;
  if (session->menu_fd >= 0 && count < max) {
    out[count++] = session->menu_fd;
  }
  for (size_t i = 0; i < BD_SESSION_MENU_CLIENTS && count < max; ++i) {
    if (session->menu_clients[i].fd >= 0) {
      out[count++] = session->menu_clients[i].fd;
    }
  }
  for (size_t i = 0; i < session->count && count < max; ++i) {
    const session_instance *instance = session->items[i];
    if (instance->reaped) {
      continue;
    }
    if (instance->hook_fd >= 0) {
      out[count++] = instance->hook_fd;
      if (count >= max) {
        break;
      }
    }
    if (instance->protocol == NULL) {
      continue;
    }
    int remaining = max - count;
    int added = bd_app_protocol_fds(instance->protocol, out + count, remaining);
    if (added > 0) {
      count += added;
    }
  }
  return count;
}

const char *bd_session_last_frame_path(const bd_session *session,
                                       const char *app_id) {
  if (session == NULL || app_id == NULL) {
    return NULL;
  }
  session_instance *instance = find_instance((bd_session *)session, app_id);
  if (instance == NULL || instance->frame_path[0] == '\0') {
    return NULL;
  }
  return instance->frame_path;
}

int bd_session_poll(bd_session *session) {
  if (session == NULL) {
    return -1;
  }
  uint64_t now = monotonic_ms();
  menu_poll(session, now);
  for (size_t i = 0; i < session->count; ++i) {
    session_instance *instance = session->items[i];
    if (instance->reaped) {
      continue;
    }
    if (instance->protocol != NULL) {
      bd_app_protocol_poll(instance->protocol, now);
    }
    check_child(session, instance);
    if (instance->reaped) {
      continue;
    }
    if (instance->hook_pid > 0) {
      command_hook_poll(session, instance, now);
      if (instance->reaped) {
        continue;
      }
    }
    if (instance->state == BD_SESSION_LAUNCHING &&
        instance->hello_deadline_ms != 0 &&
        now >= instance->hello_deadline_ms) {
      finish_foreground(session, instance);
    }
    switch (instance->op) {
      case OP_TERM_WAIT_EXIT:
        if (now >= instance->op_deadline_ms) {
          start_sigterm(session, instance);
        }
        break;
      case OP_TERM_SIGTERM:
        if (now >= instance->op_deadline_ms) {
          start_sigkill(session, instance);
        }
        break;
      case OP_TERM_SIGKILL:
        if (now >= instance->op_deadline_ms) {
          force_reap(session, instance);
        }
        break;
      default:
        break;
    }
  }

  bool active = false;
  bool has_next = false;
  uint64_t next = 0;
  for (size_t i = 0; i < session->count; ++i) {
    session_instance *instance = session->items[i];
    if (instance->reaped) {
      continue;
    }
    active = true;
    if (instance->state == BD_SESSION_LAUNCHING &&
        instance->hello_deadline_ms != 0) {
      update_next(&next, &has_next,
                  deadline_remaining(instance->hello_deadline_ms, now));
    }
    if (instance->op == OP_TERM_WAIT_EXIT ||
        instance->op == OP_TERM_SIGTERM ||
        instance->op == OP_TERM_SIGKILL) {
      update_next(&next, &has_next,
                  deadline_remaining(instance->op_deadline_ms, now));
    }
    if (instance->hook_pid > 0) {
      update_next(&next, &has_next,
                  deadline_remaining(instance->hook_deadline_ms, now));
    }
    if (instance->protocol != NULL) {
      int timeout = bd_app_protocol_next_timeout(instance->protocol, now);
      if (timeout >= 0) {
        update_next(&next, &has_next, (uint64_t)timeout);
      }
    }
  }
  if (!active) {
    return -1;
  }
  if (!has_next || next > BD_SESSION_POLL_SLICE_MS) {
    return BD_SESSION_POLL_SLICE_MS;
  }
  return (int)next;
}

bd_session *bd_session_create(const bd_session_config *config, char *error,
                              size_t error_size) {
  bd_session *session = (bd_session *)calloc(1, sizeof(bd_session));
  if (session == NULL) {
    set_error(error, error_size, "out of memory creating session");
    return NULL;
  }
  session->menu_fd = -1;
  for (size_t i = 0; i < BD_SESSION_MENU_CLIENTS; ++i) {
    session->menu_clients[i].fd = -1;
  }

  const char *socket_dir = config != NULL ? config->socket_dir : NULL;
  if (socket_dir == NULL || socket_dir[0] == '\0') {
    if (make_dirs("/run/brickdesk", 0770)) {
      socket_dir = "/run/brickdesk";
    } else {
      socket_dir = BD_SESSION_FALLBACK_SOCKET_DIR;
    }
  }
  if (strlen(socket_dir) >= sizeof(session->socket_dir)) {
    set_error(error, error_size, "socket directory path is too long");
    free(session);
    return NULL;
  }
  snprintf(session->socket_dir, sizeof(session->socket_dir), "%s", socket_dir);
  if (!make_dirs(session->socket_dir, 0770)) {
    set_error(error, error_size, "cannot create socket directory '%s'",
              session->socket_dir);
    free(session);
    return NULL;
  }

  const char *frame_dir = config != NULL ? config->frame_dir : NULL;
  if (frame_dir == NULL || frame_dir[0] == '\0') {
    frame_dir = session->socket_dir;
  }
  if (strlen(frame_dir) >= sizeof(session->frame_dir)) {
    set_error(error, error_size, "frame directory path is too long");
    free(session);
    return NULL;
  }
  snprintf(session->frame_dir, sizeof(session->frame_dir), "%s", frame_dir);
  if (!make_dirs(session->frame_dir, 0770)) {
    set_error(error, error_size, "cannot create frame directory '%s'",
              session->frame_dir);
    free(session);
    return NULL;
  }

  const char *state_root = config != NULL ? config->state_root : NULL;
  bool default_state = state_root == NULL || state_root[0] == '\0';
  if (default_state) {
    state_root = BRICKDESK_STATE_DIR;
  }
  if (strlen(state_root) >= sizeof(session->state_root)) {
    set_error(error, error_size, "state directory path is too long");
    free(session);
    return NULL;
  }
  snprintf(session->state_root, sizeof(session->state_root), "%s", state_root);
  if (!make_dirs(session->state_root, 0700)) {
    if (default_state) {
      snprintf(session->state_root, sizeof(session->state_root), "%s",
               BD_SESSION_FALLBACK_STATE_DIR);
      if (!make_dirs(session->state_root, 0700)) {
        set_error(error, error_size, "cannot create a state directory");
        free(session);
        return NULL;
      }
    } else {
      set_error(error, error_size, "cannot create state directory '%s'",
                session->state_root);
      free(session);
      return NULL;
    }
  }

  const char *session_id = config != NULL ? config->session_id : NULL;
  if (session_id == NULL || session_id[0] == '\0') {
    uint64_t now = monotonic_ms();
    snprintf(session->session_id, sizeof(session->session_id), "%06x%06x",
             (unsigned)(now & 0xffffffu),
             (unsigned)((uint64_t)getpid() & 0xffffffu));
  } else {
    snprintf(session->session_id, sizeof(session->session_id), "%s",
             session_id);
  }
  session->max_background =
      config != NULL && config->max_background > 0 ? config->max_background : 3;
  session->state_quota_mb =
      config != NULL && config->state_quota_mb > 0 ? config->state_quota_mb
                                                   : 512;
  session->last_error[0] = '\0';
  session->on_event = config != NULL ? config->on_event : NULL;
  session->event_user = config != NULL ? config->event_user : NULL;
  session->perf = config != NULL ? config->perf : (bd_session_perf_ops){0};

  session->timeouts.hello_ms = 2000;
  session->timeouts.pause_ms = 300;
  session->timeouts.resume_ms = 2000;
  session->timeouts.save_ms = 5000;
  session->timeouts.quit_ms = 3000;
  session->timeouts.term_ms = 1000;
  session->timeouts.kill_ms = 1000;
  session->timeouts.display_ms = 500;
  session->timeouts.freeze_ms = 1000;
  if (config != NULL) {
    if (config->timeouts.hello_ms > 0) {
      session->timeouts.hello_ms = config->timeouts.hello_ms;
    }
    if (config->timeouts.pause_ms > 0) {
      session->timeouts.pause_ms = config->timeouts.pause_ms;
    }
    if (config->timeouts.resume_ms > 0) {
      session->timeouts.resume_ms = config->timeouts.resume_ms;
    }
    if (config->timeouts.save_ms > 0) {
      session->timeouts.save_ms = config->timeouts.save_ms;
    }
    if (config->timeouts.quit_ms > 0) {
      session->timeouts.quit_ms = config->timeouts.quit_ms;
    }
    if (config->timeouts.term_ms > 0) {
      session->timeouts.term_ms = config->timeouts.term_ms;
    }
    if (config->timeouts.kill_ms > 0) {
      session->timeouts.kill_ms = config->timeouts.kill_ms;
    }
    if (config->timeouts.display_ms > 0) {
      session->timeouts.display_ms = config->timeouts.display_ms;
    }
    if (config->timeouts.freeze_ms > 0) {
      session->timeouts.freeze_ms = config->timeouts.freeze_ms;
    }
  }
  menu_socket_create(session);
  return session;
}

void bd_session_destroy(bd_session *session) {
  if (session == NULL) {
    return;
  }
  session->on_event = NULL;
  menu_shutdown(session);
  for (size_t i = 0; i < session->count; ++i) {
    free_instance(session, session->items[i], true);
  }
  free(session->items);
  free(session);
}

static bool valid_app_id(const char *id) {
  if (id == NULL || id[0] == '\0' || strlen(id) >= BD_APP_ID_MAX) {
    return false;
  }
  for (const char *p = id; *p != '\0'; ++p) {
    char c = *p;
    bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

static bool app_wants_ipc(const bd_app *app) {
  for (int kind = 0; kind < BD_APP_HOOK_COUNT; ++kind) {
    if (app->hooks[kind].type == BD_APP_HOOK_IPC) {
      return true;
    }
  }
  return false;
}

bd_session_result bd_session_launch(bd_session *session, const bd_app *app,
                                    char *error, size_t error_size) {
  if (session == NULL || app == NULL || !valid_app_id(app->id) ||
      app->exec_count == 0) {
    set_error(error, error_size, "invalid app");
    return BD_SESSION_IO_ERROR;
  }

  session_instance *existing = find_instance(session, app->id);
  if (existing != NULL) {
    if (instance_active(existing)) {
      set_error(error, error_size, "app '%s' is already running", app->id);
      return BD_SESSION_BUSY;
    }
    for (size_t i = 0; i < session->count; ++i) {
      if (session->items[i] == existing) {
        remove_instance_at(session, i, false);
        break;
      }
    }
  }
  if (has_foreground(session, NULL)) {
    set_error(error, error_size, "another app is already in the foreground");
    return BD_SESSION_BUSY;
  }
  if (!state_quota_check(session, app->id, error, error_size)) {
    return BD_SESSION_STATE_QUOTA;
  }

  session_instance *instance =
      (session_instance *)calloc(1, sizeof(session_instance));
  if (instance == NULL) {
    set_error(error, error_size, "out of memory launching app");
    return BD_SESSION_IO_ERROR;
  }
  instance->session = session;
  instance->pid = -1;
  instance->pgid = -1;
  instance->hook_pid = -1;
  instance->hook_fd = -1;
  instance->state = BD_SESSION_IDLE;
  instance->request_id = -1;
  instance->op = OP_NONE;
  if (!bd_app_copy(&instance->app, app)) {
    set_error(error, error_size, "out of memory copying manifest");
    free(instance);
    return BD_SESSION_IO_ERROR;
  }

  unsigned sequence = session->sequence++;
  snprintf(instance->session_id, sizeof(instance->session_id), "%s-%02x",
           session->session_id, (unsigned)(sequence & 0xffu));
  int written = snprintf(instance->socket_path, sizeof(instance->socket_path),
                         "%s/session-%s.sock", session->socket_dir,
                         instance->session_id);
  if (written <= 0 || (size_t)written >= sizeof(instance->socket_path)) {
    set_error(error, error_size, "session socket path is too long");
    bd_app_destroy(&instance->app);
    free(instance);
    return BD_SESSION_IO_ERROR;
  }
  written = snprintf(instance->state_dir, sizeof(instance->state_dir), "%s/%s",
                     session->state_root, instance->app.id);
  if (written <= 0 || (size_t)written >= sizeof(instance->state_dir) ||
      !make_dirs(instance->state_dir, 0700)) {
    set_error(error, error_size, "cannot create state directory for '%s'",
              instance->app.id);
    bd_app_destroy(&instance->app);
    free(instance);
    return BD_SESSION_IO_ERROR;
  }
  if (instance->app.state.path[0] != '\0') {
    written = snprintf(instance->state_file, sizeof(instance->state_file),
                       "%s/%s", instance->state_dir,
                       instance->app.state.path);
    if (written <= 0 || (size_t)written >= sizeof(instance->state_file)) {
      set_error(error, error_size, "state file path is too long");
      bd_app_destroy(&instance->app);
      free(instance);
      return BD_SESSION_IO_ERROR;
    }
    char parent[BD_APP_PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", instance->state_file);
    char *slash = strrchr(parent, '/');
    if (slash != NULL && slash != parent) {
      *slash = '\0';
      make_dirs(parent, 0700);
    }
  }
  if (!bd_app_last_frame_path(session->frame_dir, instance->session_id,
                              instance->frame_path,
                              sizeof(instance->frame_path))) {
    set_error(error, error_size, "last-frame path is too long");
    bd_app_destroy(&instance->app);
    free(instance);
    return BD_SESSION_IO_ERROR;
  }

  bd_app_protocol_config protocol_config;
  memset(&protocol_config, 0, sizeof(protocol_config));
  protocol_config.socket_path = instance->socket_path;
  protocol_config.session_id = instance->session_id;
  protocol_config.menu_mode =
      bd_app_menu_mode_name(instance->app.menu.mode);
  protocol_config.state_dir = instance->state_dir;
  protocol_config.on_event = on_protocol_event;
  protocol_config.user = instance;
  instance->protocol = bd_app_protocol_create(&protocol_config, error,
                                              error_size);
  if (instance->protocol == NULL) {
    bd_app_destroy(&instance->app);
    free(instance);
    return BD_SESSION_IO_ERROR;
  }

  instance->started_ms = monotonic_ms();
  instance->last_active_ms = instance->started_ms;
  if (!bd_app_protocol_socket_path(instance->protocol)) {
    set_error(error, error_size, "session socket was not created");
    bd_app_protocol_destroy(instance->protocol);
    bd_app_destroy(&instance->app);
    free(instance);
    return BD_SESSION_IO_ERROR;
  }
  bool perf_ok = apply_perf(session, instance);
  (void)perf_ok;

  if (!spawn_app(instance, error, error_size)) {
    restore_perf(session, instance);
    bd_app_protocol_destroy(instance->protocol);
    instance->protocol = NULL;
    bd_app_destroy(&instance->app);
    free(instance);
    return BD_SESSION_SPAWN_FAILED;
  }
  if (!append_instance(session, instance)) {
    set_error(error, error_size, "out of memory tracking app");
    free_instance(session, instance, true);
    return BD_SESSION_IO_ERROR;
  }

  uint64_t now = monotonic_ms();
  set_state(session, instance, BD_SESSION_LAUNCHING);
  if (app_wants_ipc(&instance->app)) {
    instance->hello_deadline_ms =
        now + (uint64_t)session->timeouts.hello_ms;
  } else {
    finish_foreground(session, instance);
  }
  session->last_error[0] = '\0';
  return BD_SESSION_OK;
}

bd_session_result bd_session_background(bd_session *session,
                                        const char *app_id) {
  session_instance *instance = find_instance(session, app_id);
  if (instance == NULL) {
    return BD_SESSION_NOT_FOUND;
  }
  if (instance->state != BD_SESSION_FOREGROUND) {
    return BD_SESSION_INVALID_STATE;
  }
  if (instance->op != OP_NONE) {
    return BD_SESSION_BUSY;
  }
  int background_count = bd_session_background_count(session);
  if (background_count >= session->max_background) {
    bd_session_eviction eviction;
    if (bd_session_suggest_eviction(session, &eviction) &&
        session->on_event != NULL) {
      bd_session_event event;
      memset(&event, 0, sizeof(event));
      event.kind = BD_SESSION_EVENT_NEEDS_ATTENTION;
      event.app_id = instance->app.id;
      event.pid = instance->pid;
      event.previous = instance->state;
      event.state = instance->state;
      snprintf(event.suggested_app_id, sizeof(event.suggested_app_id), "%s",
               eviction.app_id);
      session->on_event(session->event_user, &event);
    }
    return BD_SESSION_NEEDS_CONFIRM;
  }
  session_continue_result resumed = continue_frozen(instance);
  if (resumed == SESSION_CONTINUE_FAILED) {
    return BD_SESSION_IO_ERROR;
  }
  if (resumed == SESSION_CONTINUE_EXITED) {
    return BD_SESSION_INVALID_STATE;
  }
  set_state(session, instance, BD_SESSION_SUSPENDING);
  start_background(session, instance);
  return BD_SESSION_OK;
}

bd_session_result bd_session_resume(bd_session *session, const char *app_id) {
  session_instance *instance = find_instance(session, app_id);
  if (instance == NULL) {
    return BD_SESSION_NOT_FOUND;
  }
  if (instance->state != BD_SESSION_BACKGROUND) {
    return BD_SESSION_INVALID_STATE;
  }
  if (instance->op != OP_NONE) {
    return BD_SESSION_BUSY;
  }
  if (has_foreground(session, instance)) {
    return BD_SESSION_BUSY;
  }
  char error[256] = {0};
  if (!state_quota_check(session, app_id, error, sizeof(error))) {
    return BD_SESSION_STATE_QUOTA;
  }
  session->last_error[0] = '\0';
  start_resume(session, instance);
  return BD_SESSION_OK;
}

bd_session_result bd_session_freeze(bd_session *session, const char *app_id) {
  session_instance *instance = find_instance(session, app_id);
  if (instance == NULL) {
    return BD_SESSION_NOT_FOUND;
  }
  switch (instance->state) {
    case BD_SESSION_LAUNCHING:
    case BD_SESSION_SUSPENDING:
    case BD_SESSION_RESUMING:
    case BD_SESSION_TERMINATING:
      return BD_SESSION_BUSY;
    case BD_SESSION_FOREGROUND:
      break;
    default:
      return BD_SESSION_INVALID_STATE;
  }
  if (instance->freeze == SESSION_FREEZE_HELD) {
    return BD_SESSION_OK;
  }
  if (instance->freeze == SESSION_FREEZE_RECOVERY) {
    /* A previous freeze rollback still needs a successful thaw. */
    return BD_SESSION_IO_ERROR;
  }
  if (instance->stopped_by_session) {
    return BD_SESSION_INVALID_STATE;
  }
  if (instance->op != OP_NONE || instance->request_id >= 0) {
    return BD_SESSION_BUSY;
  }

  session_child_status pending = poll_child_status(instance);
  if (pending == SESSION_CHILD_STOPPED) {
    /* Already stopped, but not by this session: never claim that stop. */
    instance->stopped_by_session = true;
    return BD_SESSION_INVALID_STATE;
  }
  if (pending == SESSION_CHILD_EXITED) {
    return BD_SESSION_INVALID_STATE;
  }
  if (pending == SESSION_CHILD_UNKNOWN) {
    return BD_SESSION_IO_ERROR;
  }

  if (!send_signal(instance, SIGSTOP)) {
    int saved = errno;
    poll_child_status(instance);
    return saved == ESRCH ? BD_SESSION_INVALID_STATE : BD_SESSION_IO_ERROR;
  }
  uint64_t deadline = monotonic_ms() + (uint64_t)session->timeouts.freeze_ms;
  session_child_status status = wait_child_status(instance, deadline);
  if (status == SESSION_CHILD_STOPPED) {
    instance->freeze = SESSION_FREEZE_HELD;
    return BD_SESSION_OK;
  }

  /*
   * Failed confirmation: undo our SIGSTOP.  If SIGCONT is rejected, keep a
   * recovery marker so thaw() can retry instead of claiming the app resumed.
   */
  if (!instance->reaped) {
    instance->freeze = SESSION_FREEZE_RECOVERY;
    (void)continue_frozen(instance);
    if (!instance->reaped &&
        poll_child_status(instance) == SESSION_CHILD_STOPPED) {
      instance->freeze = SESSION_FREEZE_RECOVERY;
      (void)continue_frozen(instance);
    }
  }
  return instance->reaped ? BD_SESSION_INVALID_STATE : BD_SESSION_IO_ERROR;
}

bd_session_result bd_session_thaw(bd_session *session, const char *app_id) {
  session_instance *instance = find_instance(session, app_id);
  if (instance == NULL) {
    return BD_SESSION_NOT_FOUND;
  }
  if (instance->state != BD_SESSION_FOREGROUND) {
    instance->freeze = SESSION_FREEZE_NONE;
    return BD_SESSION_INVALID_STATE;
  }
  if (instance->freeze == SESSION_FREEZE_NONE) {
    return instance->stopped_by_session ? BD_SESSION_INVALID_STATE
                                        : BD_SESSION_OK;
  }
  if (instance->op != OP_NONE || instance->request_id >= 0) {
    return BD_SESSION_BUSY;
  }
  session_continue_result resumed = continue_frozen(instance);
  if (resumed == SESSION_CONTINUE_FAILED) {
    return BD_SESSION_IO_ERROR;
  }
  if (resumed == SESSION_CONTINUE_EXITED) {
    return BD_SESSION_INVALID_STATE;
  }
  if (poll_child_status(instance) == SESSION_CHILD_EXITED) {
    return BD_SESSION_INVALID_STATE;
  }
  return BD_SESSION_OK;
}

bd_session_result bd_session_terminate(bd_session *session,
                                       const char *app_id) {
  session_instance *instance = find_instance(session, app_id);
  if (instance == NULL) {
    return BD_SESSION_NOT_FOUND;
  }
  if (instance->state == BD_SESSION_EXITED ||
      instance->state == BD_SESSION_CRASHED ||
      instance->state == BD_SESSION_TERMINATING) {
    return BD_SESSION_OK;
  }
  if (instance->state == BD_SESSION_SUSPENDING ||
      instance->state == BD_SESSION_RESUMING) {
    return BD_SESSION_BUSY;
  }
  return begin_terminate(session, instance);
}

static bd_session_result request_state_transfer(bd_session *session,
                                                const char *app_id, int slot,
                                                bool save) {
  session_instance *instance = find_instance(session, app_id);
  if (instance == NULL) {
    return BD_SESSION_NOT_FOUND;
  }
  if (instance->state != BD_SESSION_FOREGROUND &&
      !(instance->state == BD_SESSION_BACKGROUND &&
        !instance->stopped_by_session)) {
    return BD_SESSION_INVALID_STATE;
  }
  if (instance->op != OP_NONE) {
    return BD_SESSION_BUSY;
  }
  if (instance->freeze != SESSION_FREEZE_NONE) {
    return BD_SESSION_BUSY;
  }
  const bd_app_hook_kind kind = save ? BD_APP_HOOK_SAVE : BD_APP_HOOK_LOAD;
  if (slot < 0 || (instance->app.state.max_slots > 0 &&
                   slot >= instance->app.state.max_slots)) {
    return BD_SESSION_INVALID_STATE;
  }
  int timeout = hook_timeout(session, instance, kind, session->timeouts.save_ms);
  if (instance->app.hooks[kind].type == BD_APP_HOOK_COMMAND) {
    if (!start_command_hook(session, instance,
                            save ? OP_SAVE_STATE : OP_LOAD_STATE,
                            save ? "save" : "load", timeout, slot)) {
      return BD_SESSION_IO_ERROR;
    }
    return BD_SESSION_OK;
  }
  if (instance->app.hooks[kind].type != BD_APP_HOOK_IPC ||
      !instance->ipc_connected) {
    return BD_SESSION_UNSUPPORTED;
  }
  char params[64];
  snprintf(params, sizeof(params), "{\"slot\":%d}", slot);
  if (!begin_request(session, instance,
                     save ? OP_SAVE_STATE : OP_LOAD_STATE,
                     instance->app.hooks[kind].method, params, timeout,
                     save ? "save" : "load")) {
    return BD_SESSION_IO_ERROR;
  }
  return BD_SESSION_OK;
}

bd_session_result bd_session_save_state(bd_session *session,
                                        const char *app_id, int slot) {
  return request_state_transfer(session, app_id, slot, true);
}

bd_session_result bd_session_load_state(bd_session *session,
                                        const char *app_id, int slot) {
  return request_state_transfer(session, app_id, slot, false);
}

size_t bd_session_running_count(const bd_session *session) {
  if (session == NULL) {
    return 0;
  }
  size_t count = 0;
  for (size_t i = 0; i < session->count; ++i) {
    if (instance_active(session->items[i])) {
      ++count;
    }
  }
  return count;
}

bool bd_session_running_at(const bd_session *session, size_t index,
                           bd_session_app_info *out) {
  if (session == NULL || out == NULL) {
    return false;
  }
  size_t seen = 0;
  for (size_t i = 0; i < session->count; ++i) {
    if (!instance_active(session->items[i])) {
      continue;
    }
    if (seen == index) {
      fill_info(session->items[i], out);
      return true;
    }
    ++seen;
  }
  return false;
}

bool bd_session_find(const bd_session *session, const char *app_id,
                     bd_session_app_info *out) {
  session_instance *instance =
      find_instance((bd_session *)session, app_id);
  if (instance == NULL || out == NULL) {
    return false;
  }
  fill_info(instance, out);
  return true;
}

const bd_app *bd_session_manifest(const bd_session *session,
                                  const char *app_id) {
  session_instance *instance =
      find_instance((bd_session *)session, app_id);
  return instance != NULL ? &instance->app : NULL;
}

size_t bd_session_clear_terminated(bd_session *session) {
  if (session == NULL) {
    return 0;
  }
  size_t removed = 0;
  for (size_t i = session->count; i > 0; --i) {
    session_instance *instance = session->items[i - 1];
    if (instance->state == BD_SESSION_EXITED ||
        instance->state == BD_SESSION_CRASHED) {
      remove_instance_at(session, i - 1, false);
      ++removed;
    }
  }
  return removed;
}

int bd_session_background_count(const bd_session *session) {
  if (session == NULL) {
    return 0;
  }
  int count = 0;
  for (size_t i = 0; i < session->count; ++i) {
    if (session->items[i]->state == BD_SESSION_BACKGROUND) {
      ++count;
    }
  }
  return count;
}

int bd_session_max_background(const bd_session *session) {
  return session != NULL ? session->max_background : 0;
}

void bd_session_set_max_background(bd_session *session, int value) {
  if (session != NULL) {
    session->max_background = value > 0 ? value : 3;
  }
}

void bd_session_set_state_quota(bd_session *session, int value) {
  if (session != NULL) {
    session->state_quota_mb = value > 0 ? value : 512;
  }
}

bool bd_session_suggest_eviction(const bd_session *session,
                                 bd_session_eviction *out) {
  if (session == NULL || out == NULL) {
    return false;
  }
  const session_instance *oldest = NULL;
  for (size_t i = 0; i < session->count; ++i) {
    const session_instance *instance = session->items[i];
    if (instance->state != BD_SESSION_BACKGROUND) {
      continue;
    }
    if (oldest == NULL ||
        instance->backgrounded_ms < oldest->backgrounded_ms) {
      oldest = instance;
    }
  }
  if (oldest == NULL) {
    return false;
  }
  memset(out, 0, sizeof(*out));
  snprintf(out->app_id, sizeof(out->app_id), "%s", oldest->app.id);
  out->pid = oldest->pid;
  out->backgrounded_ms = oldest->backgrounded_ms;
  return true;
}

const char *bd_session_state_name(bd_session_state state) {
  switch (state) {
    case BD_SESSION_IDLE: return "idle";
    case BD_SESSION_LAUNCHING: return "launching";
    case BD_SESSION_FOREGROUND: return "foreground";
    case BD_SESSION_SUSPENDING: return "suspending";
    case BD_SESSION_BACKGROUND: return "background";
    case BD_SESSION_RESUMING: return "resuming";
    case BD_SESSION_TERMINATING: return "terminating";
    case BD_SESSION_EXITED: return "exited";
    case BD_SESSION_CRASHED: return "crashed";
  }
  return "unknown";
}

const char *bd_session_last_error(const bd_session *session) {
  return session != NULL ? session->last_error : "";
}

const char *bd_session_result_name(bd_session_result result) {
  switch (result) {
    case BD_SESSION_OK: return "ok";
    case BD_SESSION_NEEDS_CONFIRM: return "needs_confirm";
    case BD_SESSION_BUSY: return "busy";
    case BD_SESSION_NOT_FOUND: return "not_found";
    case BD_SESSION_INVALID_STATE: return "invalid_state";
    case BD_SESSION_UNSUPPORTED: return "unsupported";
    case BD_SESSION_SPAWN_FAILED: return "spawn_failed";
    case BD_SESSION_IO_ERROR: return "io_error";
    case BD_SESSION_STATE_QUOTA: return "state_quota";
  }
  return "unknown";
}
