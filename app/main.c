// A Win32 host over the Hyperswarm backend worklet. On startup it creates the
// window, boots the worklet, and runs the typed-RPC client on bare-kit's IPC
// poll thread. Events arrive on that thread and are marshalled to the UI thread
// with PostMessage; clicking the checkbox sends the new state to the worklet.

// WIN32_LEAN_AND_MEAN keeps <windows.h> from pulling in winsock v1, which would
// then collide with the winsock2 that <uv.h> includes ("redefinition of
// 'sockaddr_in'", "redefinition of 'fd_set'", and a dozen more). cmake-bare's
// own win32/delay-load.c does the same for the same reason.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h> // must come first; <commctrl.h> depends on it

#include <commctrl.h>

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

// Private messages, posted from the poll thread to the UI thread. WM_APP_INFO
// carries a heap-allocated info_t as LPARAM, which WndProc frees.
#define WM_APP_STATE (WM_APP + 1)
#define WM_APP_PEERS (WM_APP + 2)
#define WM_APP_INFO  (WM_APP + 3)

#define ID_TOGGLE 1001

typedef struct {
  char key[128];
  char topic[128];
} info_t;

static HWND window;
static HWND toggle;
static HWND peers_value;
static HWND key_value;
static HWND topic_value;

// The manifest declares PerMonitorV2, so Windows scales nothing for us: every
// coordinate below is in 96 DPI units and is scaled to the window's own DPI.
#define BARE_WINDOWS_DPI 96

#define BARE_WINDOWS_CLIENT_WIDTH  504
#define BARE_WINDOWS_CLIENT_HEIGHT 222

typedef struct {
  HWND hwnd;
  int x, y, w, h;
} control_t;

// Every child, with its position in 96 DPI units, so a DPI change is a re-layout
// rather than a rebuild. Sized with room to spare: a control that does not fit
// here is created but never rescaled, so the bound is checked on the way in.
static control_t controls[16];
static int controls_len;

static HFONT shell_font;

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

// --- UI-thread updates, posted from the poll thread ---

// Setting the checkbox programmatically does not send BN_CLICKED, so unlike
// bare-linux's GTK switch there is no handler to block here - an incoming
// new-state cannot echo back out as a local flip.
static void
apply_state(bool on) {
  SendMessageW(toggle, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);

  // The check glyph is drawn at a theme size the control rect does not govern,
  // so the state is spelled out beside it rather than left to a small square.
  SetWindowTextW(toggle, on ? L"On" : L"Off");
}

static void
apply_peers(unsigned int count) {
  wchar_t buf[32];
  _snwprintf_s(buf, 32, _TRUNCATE, L"%u", count);
  SetWindowTextW(peers_value, buf);
}

static void
apply_info(const info_t *info) {
  wchar_t buf[128];
  _snwprintf_s(buf, 128, _TRUNCATE, L"%hs", info->key);
  SetWindowTextW(key_value, buf);
  _snwprintf_s(buf, 128, _TRUNCATE, L"%hs", info->topic);
  SetWindowTextW(topic_value, buf);
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
    PostMessageW(window, WM_APP_STATE, state.on ? 1 : 0, 0);
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

// PostMessage rather than SendMessage: the poll thread must not block on the UI
// thread, which may itself be inside send_set_state holding the RPC lock.

static void
on_new_state(void *ctx, const sync_switch_state_t *state) {
  PostMessageW(window, WM_APP_STATE, state->on ? 1 : 0, 0);
}

static void
on_peers_changed(void *ctx, const sync_peers_t *peers) {
  PostMessageW(window, WM_APP_PEERS, (WPARAM) peers->count, 0);
}

static void
on_info(void *ctx, const sync_identity_t *id) {
  info_t *info = calloc(1, sizeof(info_t));
  if (info == NULL) return;

  snprintf(info->key, sizeof info->key, "%.*s", (int) id->public_key.len, (const char *) id->public_key.data);
  snprintf(info->topic, sizeof info->topic, "%.*s", (int) id->topic.len, (const char *) id->topic.data);

  if (!PostMessageW(window, WM_APP_INFO, 0, (LPARAM) info)) free(info);
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

static int
scale(int value, UINT dpi) {
  return MulDiv(value, dpi, BARE_WINDOWS_DPI);
}

// Creates a child at a 96 DPI position and records it, so `layout` can place it
// again at whatever DPI the window is showing at.
static HWND
add_control(HWND parent, HINSTANCE instance, const wchar_t *class, const wchar_t *text, DWORD style, int x, int y, int w, int h, HMENU id) {
  UINT dpi = GetDpiForWindow(parent);

  HWND child = CreateWindowExW(
    0, class, text, WS_CHILD | WS_VISIBLE | style,
    scale(x, dpi), scale(y, dpi), scale(w, dpi), scale(h, dpi),
    parent, id, instance, NULL
  );

  if (child != NULL && controls_len < (int) (sizeof controls / sizeof *controls)) {
    controls[controls_len++] = (control_t) {child, x, y, w, h};
  }

  return child;
}

// One "name: value" row of static controls; returns the value control so the
// caller can update it from events.
static HWND
add_row(HWND parent, HINSTANCE instance, int y, const wchar_t *name, const wchar_t *value) {
  add_control(parent, instance, L"STATIC", name, 0, 24, y, 130, 20, NULL);

  return add_control(parent, instance, L"STATIC", value, 0, 160, y, 320, 20, NULL);
}

// Sizes the frame so the client area holds the layout at this DPI. The frame
// itself (caption, borders) scales too, hence the ForDpi variant.
static void
resize_window(HWND parent, UINT dpi) {
  RECT rect = {
    .right = scale(BARE_WINDOWS_CLIENT_WIDTH, dpi),
    .bottom = scale(BARE_WINDOWS_CLIENT_HEIGHT, dpi),
  };

  DWORD style = (DWORD) GetWindowLongPtrW(parent, GWL_STYLE);

  AdjustWindowRectExForDpi(&rect, style, FALSE, 0, dpi);

  SetWindowPos(
    parent, NULL, 0, 0,
    rect.right - rect.left, rect.bottom - rect.top,
    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE
  );
}

static void
layout(HWND parent, UINT dpi) {
  for (int i = 0; i < controls_len; i++) {
    control_t c = controls[i];

    SetWindowPos(
      c.hwnd, NULL,
      scale(c.x, dpi), scale(c.y, dpi), scale(c.w, dpi), scale(c.h, dpi),
      SWP_NOZORDER | SWP_NOACTIVATE
    );
  }
}

// Win32 controls default to the ancient bitmap system font. Apply the shell's
// message font to every child so the window looks current rather than broken.
static BOOL CALLBACK
set_font(HWND child, LPARAM font) {
  SendMessageW(child, WM_SETFONT, (WPARAM) font, MAKELPARAM(TRUE, 0));
  return TRUE;
}

// The metrics are per DPI: asking without one yields a font sized for a
// different display, which is how a scaled window ends up with text too big for
// its own controls.
static void
apply_shell_font(HWND parent, UINT dpi) {
  NONCLIENTMETRICSW metrics = {.cbSize = sizeof(NONCLIENTMETRICSW)};

  if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0, dpi)) return;

  HFONT font = CreateFontIndirectW(&metrics.lfMessageFont);
  if (font == NULL) return;

  EnumChildWindows(parent, set_font, (LPARAM) font);

  if (shell_font != NULL) DeleteObject(shell_font);

  shell_font = font;
}

static LRESULT CALLBACK
on_message(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
  case WM_APP_STATE:
    apply_state(wparam != 0);
    return 0;

  case WM_APP_PEERS:
    apply_peers((unsigned int) wparam);
    return 0;

  case WM_APP_INFO: {
    info_t *info = (info_t *) lparam;
    apply_info(info);
    free(info);
    return 0;
  }

  case WM_COMMAND:
    // The checkbox auto-toggles itself on click, so read back what it now shows
    // and tell the worklet. The reply reconciles it.
    if (LOWORD(wparam) == ID_TOGGLE && HIWORD(wparam) == BN_CLICKED) {
      LRESULT checked = SendMessageW(toggle, BM_GETCHECK, 0, 0);
      apply_state(checked == BST_CHECKED); // label follows the click, not the reply
      send_set_state(ipc, checked == BST_CHECKED);
      return 0;
    }
    break;

  // Dragged to a monitor with different scaling. Windows hands us the frame it
  // wants; the children and the font are ours to redo.
  case WM_DPICHANGED: {
    UINT dpi = HIWORD(wparam);
    RECT *suggested = (RECT *) lparam;

    SetWindowPos(
      hwnd, NULL,
      suggested->left, suggested->top,
      suggested->right - suggested->left, suggested->bottom - suggested->top,
      SWP_NOZORDER | SWP_NOACTIVATE
    );

    layout(hwnd, dpi);
    apply_shell_font(hwnd, dpi);

    return 0;
  }

  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  }

  return DefWindowProcW(hwnd, message, wparam, lparam);
}

static void
create_window(HINSTANCE instance) {
  WNDCLASSEXW class = {
    .cbSize = sizeof(WNDCLASSEXW),
    .lpfnWndProc = on_message,
    .hInstance = instance,
    .hCursor = LoadCursorW(NULL, IDC_ARROW),
    .hbrBackground = (HBRUSH) (COLOR_WINDOW + 1),
    .lpszClassName = L"BareWindowsHost",
  };

  if (RegisterClassExW(&class) == 0) {
    fprintf(stderr, "RegisterClassExW failed (%lu)\n", GetLastError());
    exit(1);
  }

  window = CreateWindowExW(
    0, L"BareWindowsHost", L"Bare <-> Windows",
    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
    CW_USEDEFAULT, CW_USEDEFAULT, 0, 0,
    NULL, NULL, instance, NULL
  );

  if (window == NULL) {
    fprintf(stderr, "CreateWindowExW failed (%lu)\n", GetLastError());
    exit(1);
  }

  // Only now is there a window to ask, and its DPI is the one that matters: on a
  // multi-monitor desktop it is the monitor it opened on, not the primary.
  UINT dpi = GetDpiForWindow(window);

  resize_window(window, dpi);

  add_control(window, instance, L"STATIC", L"Shared switch", 0, 24, 24, 130, 20, NULL);

  toggle = add_control(window, instance, L"BUTTON", L"Off", BS_AUTOCHECKBOX, 160, 22, 120, 24, (HMENU) ID_TOGGLE);

  peers_value = add_row(window, instance, 64, L"Peers connected", L"0");
  key_value = add_row(window, instance, 92, L"Your key", L"...");
  topic_value = add_row(window, instance, 120, L"Topic", L"...");

  add_control(
    window, instance, L"STATIC",
    L"Launch a second copy - flip the switch in one window and watch the other "
    L"follow. No server in between.",
    0, 24, 160, 456, 40, NULL
  );

  apply_shell_font(window, dpi);

  ShowWindow(window, SW_SHOWNORMAL);
  UpdateWindow(window);
}

int
main(int argc, char **argv) {
  INITCOMMONCONTROLSEX controls = {
    .dwSize = sizeof(INITCOMMONCONTROLSEX),
    .dwICC = ICC_STANDARD_CLASSES,
  };
  InitCommonControlsEx(&controls);

  HINSTANCE instance = GetModuleHandleW(NULL);

  // The window must exist before the worklet starts: the poll thread posts to
  // it as soon as the first event arrives.
  create_window(instance);

  boot();

  MSG message;
  while (GetMessageW(&message, NULL, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  shutdown_host();
  return 0;
}
