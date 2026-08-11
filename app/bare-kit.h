#ifndef BARE_WINDOWS_BARE_KIT_H
#define BARE_WINDOWS_BARE_KIT_H

// Forward declarations of the small bare-kit C API this host calls. The Windows
// prebuild ships libraries only (no headers), so we declare the subset we use
// here, matching bare-kit's shared/worklet.h and shared/ipc.h exactly - these
// declarations are the ABI against bare-kit.dll. The real
// uv_buf_t comes from <uv.h> (the bare-headers package).

#include <stddef.h>
#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bare_worklet_s bare_worklet_t;
typedef struct bare_worklet_options_s bare_worklet_options_t;
typedef struct bare_ipc_s bare_ipc_t;
typedef struct bare_ipc_poll_s bare_ipc_poll_t;

typedef void (*bare_ipc_poll_cb)(bare_ipc_poll_t *, int events);

struct bare_worklet_options_s {
  size_t memory_limit;
  const char *assets;
};

enum {
  bare_ipc_readable = 0x1,
  bare_ipc_writable = 0x2,
};

// Negative returns from bare_ipc_read / bare_ipc_write; a 0 return is a
// successful read (a 0-length read meaning EOF).
enum {
  bare_ipc_would_block = -1,
  bare_ipc_error = -2,
};

int
bare_worklet_alloc(bare_worklet_t **result);

int
bare_worklet_init(bare_worklet_t *worklet, const bare_worklet_options_t *options);

int
bare_worklet_start(bare_worklet_t *worklet, const char *filename, const uv_buf_t *source, int argc, const char *argv[]);

int
bare_worklet_terminate(bare_worklet_t *worklet);

void
bare_worklet_destroy(bare_worklet_t *worklet);

int
bare_ipc_alloc(bare_ipc_t **result);

int
bare_ipc_init(bare_ipc_t *ipc, bare_worklet_t *worklet);

void
bare_ipc_destroy(bare_ipc_t *ipc);

int
bare_ipc_read(bare_ipc_t *ipc, void **data, size_t *len);

int
bare_ipc_write(bare_ipc_t *ipc, const void *data, size_t len);

int
bare_ipc_poll_alloc(bare_ipc_poll_t **result);

int
bare_ipc_poll_init(bare_ipc_poll_t *poll, bare_ipc_t *ipc);

void
bare_ipc_poll_destroy(bare_ipc_poll_t *poll);

bare_ipc_t *
bare_ipc_poll_get_ipc(bare_ipc_poll_t *poll);

int
bare_ipc_poll_start(bare_ipc_poll_t *poll, int events, bare_ipc_poll_cb cb);

int
bare_ipc_poll_stop(bare_ipc_poll_t *poll);

#ifdef __cplusplus
}
#endif

#endif // BARE_WINDOWS_BARE_KIT_H
