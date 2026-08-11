// Phase 1 host: a console front end over the Hyperswarm backend worklet. On
// startup it boots the worklet and runs the typed-RPC client on bare-kit's IPC
// poll thread; the main thread reads stdin. Press Enter to flip the switch.
// Phase 2 replaces the printing with a Win32 window.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <uv.h>

#include <rpc.h>
#include <rpc/client.h>
#include <sync_hrpc.h>

#include "bare-kit.h"

static bare_worklet_t *worklet;
static bare_ipc_t *ipc;
static bare_ipc_poll_t *ipc_poll;
static uv_buf_t source;

static rpc_client_t client;

// Guards rpc_client_t, which is not thread-safe: the main thread (next_id/track)
// and the poll thread (rpc_client_read) both touch it. Callbacks fire under the
// lock but never re-enter the client.
static uv_mutex_t client_lock;

// Last state we know about, used only to decide which way the next local flip
// should go. Written on the poll thread, read on the main thread; a stale read
// costs at most one redundant flip, so it is deliberately unsynchronized.
static volatile bool current_on = false;

// Read the packed worklet bundle into memory; bare_worklet_start takes its bytes.
static uv_buf_t
read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    perror("fopen");
    exit(1);
  }

  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (n < 0) {
    fprintf(stderr, "cannot size %s\n", path);
    exit(1);
  }

  char *buf = malloc(n);
  if (buf == NULL) {
    fprintf(stderr, "out of memory reading %s\n", path);
    exit(1);
  }

  size_t read = fread(buf, 1, n, f);
  fclose(f);

  if (read != (size_t) n) {
    fprintf(stderr, "short read of %s\n", path);
    exit(1);
  }

  return uv_buf_init(buf, (unsigned int) n);
}

// --- event sinks. Phase 2 replaces these bodies with PostMessage. ---

static void
apply_state(bool on) {
  current_on = on;
  printf("[state] %s\n", on ? "on" : "off");
  fflush(stdout);
}

static void
apply_peers(unsigned int count) {
  printf("[peers] %u\n", count);
  fflush(stdout);
}

static void
apply_info(const char *key, const char *topic) {
  printf("[info] key=%s topic=%s\n", key, topic);
  fflush(stdout);
}

// --- poll-thread RPC callbacks ---

// The reply to our set-state request, routed here by id. Reconciles our idea of
// the state with the worklet's authoritative one.
static void
on_set_state_reply(void *data, const rpc_message_t *msg) {
  sync_switch_state_t state;
  hrpc_error_t error;
  int r = sync_decode_set_state_response(msg, &state, &error);

  if (r == hrpc_ok) {
    apply_state(state.on);
  } else if (r == hrpc_error_response) {
    // The backend never rejects set-state, so this only logs; a backend that
    // can reject should reconcile the state back here.
    fprintf(stderr, "set-state error: %.*s\n", (int) error.message.len, error.message.data);
  } else {
    fprintf(stderr, "set-state reply decode failed (%d)\n", r);
  }
}

// Send one set-state request and track its reply by id. Called from the main
// thread while the poll thread runs, so the client touches are locked. The reply
// can only arrive after the worklet sees this write, so tracking before the
// write is enough to catch it.
static void
send_set_state(bare_ipc_t *i, bool on) {
  uv_mutex_lock(&client_lock);
  uint64_t id = rpc_client_next_id(&client);
  uv_mutex_unlock(&client_lock);

  sync_switch_state_t want = {.on = on};
  uint8_t *request;
  size_t request_len;
  if (sync_encode_set_state(id, &want, &request, &request_len) < 0) {
    fprintf(stderr, "encode set-state failed\n");
    return;
  }

  uv_mutex_lock(&client_lock);
  rpc_client_track(&client, id, on_set_state_reply, NULL);
  uv_mutex_unlock(&client_lock);

  // A short or failed write leaves a partial frame in the pipe, desyncing every
  // later frame, and the tracked reply never comes. Unrecoverable, so fatal.
  int written = bare_ipc_write(i, request, request_len);
  free(request);
  if (written < 0 || (size_t) written != request_len) {
    fprintf(stderr, "set-state write failed (%d of %zu bytes)\n", written, request_len);
    exit(1);
  }
}

static void
on_new_state(void *ctx, const sync_switch_state_t *state) {
  apply_state(state->on);
}

static void
on_peers_changed(void *ctx, const sync_peers_t *peers) {
  apply_peers((unsigned int) peers->count);
}

static void
on_info(void *ctx, const sync_identity_t *id) {
  char key[128];
  char topic[128];

  int key_len = (int) id->public_key.len;
  int topic_len = (int) id->topic.len;

  snprintf(key, sizeof key, "%.*s", key_len, (const char *) id->public_key.data);
  snprintf(topic, sizeof topic, "%.*s", topic_len, (const char *) id->topic.data);

  apply_info(key, topic);
}

// Fallthrough for frames not matched to a pending request. Dispatch decodes the
// known events and calls the matching handler above; anything else is raw.
static void
on_event(void *data, const rpc_message_t *msg) {
  sync_hrpc_handlers_t handlers = {
    .on_new_state = on_new_state,
    .on_peers_changed = on_peers_changed,
    .on_info = on_info,
  };

  uint8_t *reply = NULL;
  size_t reply_len = 0;
  if (sync_hrpc_dispatch(&handlers, msg, &reply, &reply_len) < 0) {
    fprintf(stderr, "unhandled frame (command %llu)\n", (unsigned long long) msg->command);
  }
}

// Runs on bare-kit's IPC poll thread. Drains the readable bytes and feeds them
// to the client, which decodes complete frames and invokes the matching
// callback. A decode error means the stream is unrecoverable, so it is fatal.
//
// The stop/re-arm around the drain is required on win32 and is the one real
// difference from bare-linux's version of this function. The win32 poll is
// one-shot: bare-kit's `bare_ipc__on_read` calls `uv_read_stop` after buffering
// each message, and `bare_ipc_poll_start` only restarts the read when the poll
// does not already have `bare_ipc_readable` set - so re-arming without stopping
// first is a no-op. bare-kit's own `worklet-ipc` test uses exactly this
// stop-drain-start shape. bare-linux does not need it because epoll there is
// level-triggered.
//
// Without it the host receives precisely one message - the worklet's opening
// `info` - and then goes silent forever: no peer counts, no state changes, and
// no reply to anything it sends. Since win32 IPC also surfaces no end-of-stream,
// that presents as a live but inert window rather than as an error.
//
// The zero-length branch below is dead code on Windows: win32 IPC surfaces no
// end-of-stream, so a worklet that exits - or dies - is indistinguishable from
// an idle one. It is kept because it is correct on other platforms and will
// start working if upstream fixes this. Do not rely on it to notice a dead
// worklet; nothing here can.
static void
on_readable(bare_ipc_poll_t *poll, int events) {
  bare_ipc_t *i = bare_ipc_poll_get_ipc(poll);

  bare_ipc_poll_stop(poll);

  while (1) {
    void *data;
    size_t len;
    if (bare_ipc_read(i, &data, &len) != 0) break;
    if (len == 0) {
      fprintf(stderr, "[host] worklet closed\n");
      exit(1);
    }
    uv_mutex_lock(&client_lock);
    int r = rpc_client_read(&client, data, len);
    uv_mutex_unlock(&client_lock);
    if (r < 0) {
      fprintf(stderr, "rpc read failed (%d)\n", r);
      exit(1);
    }
  }

  int err = bare_ipc_poll_start(poll, bare_ipc_readable, on_readable);
  if (err != 0) {
    fprintf(stderr, "bare_ipc_poll_start failed (%d)\n", err);
    exit(1);
  }
}

// Abort if a boot step failed. Each setup call returns 0 on success; a failure
// would leave the host talking to a dead worklet, so it is fatal.
static void
boot_step(int rc, const char *what) {
  if (rc != 0) {
    fprintf(stderr, "%s failed (%d)\n", what, rc);
    exit(1);
  }
}

// Boot the worklet and start the RPC client + poll thread.
static void
boot(void) {
  uv_mutex_init(&client_lock);

  boot_step(bare_worklet_alloc(&worklet), "bare_worklet_alloc");
  bare_worklet_options_t options = {0};
  boot_step(bare_worklet_init(worklet, &options), "bare_worklet_init");

  source = read_file(BUNDLE_PATH);
  boot_step(bare_worklet_start(worklet, "/app.bundle", &source, 0, NULL), "bare_worklet_start");

  boot_step(bare_ipc_alloc(&ipc), "bare_ipc_alloc");
  boot_step(bare_ipc_init(ipc, worklet), "bare_ipc_init");

  boot_step(rpc_client_init(&client, on_event, NULL), "rpc_client_init");

  boot_step(bare_ipc_poll_alloc(&ipc_poll), "bare_ipc_poll_alloc");
  boot_step(bare_ipc_poll_init(ipc_poll, ipc), "bare_ipc_poll_init");
  boot_step(bare_ipc_poll_start(ipc_poll, bare_ipc_readable, on_readable), "bare_ipc_poll_start");
}

static void
shutdown_host(void) {
  bare_ipc_poll_destroy(ipc_poll); // also stops the poll thread and joins it
  bare_ipc_destroy(ipc);
  rpc_client_destroy(&client);
  uv_mutex_destroy(&client_lock);
  bare_worklet_terminate(worklet);
  bare_worklet_destroy(worklet);
  free(source.base);
}

int
main(int argc, char **argv) {
  boot();

  printf("Press Enter to flip the switch, q then Enter to quit.\n");
  fflush(stdout);

  char line[16];
  while (fgets(line, sizeof line, stdin) != NULL) {
    if (line[0] == 'q') break;
    send_set_state(ipc, !current_on);
  }

  shutdown_host();
  return 0;
}
