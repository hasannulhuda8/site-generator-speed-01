// addon.c - the Node-API face of the native engine.
//
// runAll(buffer) -> Buffer: the raw path, byte for byte the same
// answers the WebAssembly build produces.
// runCli() -> 0 on success (never returns: it ends the process with
// SYS_exit_group once answers.json is written, skipping the whole
// V8/Node teardown) or -1 when the CLI path could not run, so the
// caller can fall back to the ordinary read/build/write flow.
#define NAPI_VERSION 8
#include <node_api.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

extern void na_build_all(const unsigned char *raw, unsigned len,
                         const unsigned char **out, unsigned *out_len);
extern unsigned char *na_input_begin(void);
extern void na_build_all_at(unsigned len, const unsigned char **out,
                            unsigned *out_len);

static napi_value RunAll(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  if (napi_get_cb_info(env, info, &argc, argv, 0, 0) != napi_ok ||
      argc < 1) {
    napi_throw_error(env, 0, "runAll needs one Buffer argument");
    return 0;
  }
  void *data;
  size_t len;
  if (napi_get_buffer_info(env, argv[0], &data, &len) != napi_ok) {
    napi_throw_error(env, 0, "runAll needs one Buffer argument");
    return 0;
  }
  const unsigned char *out;
  unsigned out_len;
  na_build_all((const unsigned char *)data, (unsigned)len, &out,
               &out_len);
  napi_value result;
  if (napi_create_buffer_copy(env, out_len, out, 0, &result) != napi_ok) {
    napi_throw_error(env, 0, "could not allocate the answers buffer");
    return 0;
  }
  return result;
}

// Read a whole file; returns 0 on success.
static int read_all(const char *path, unsigned char **out,
                    unsigned *out_len) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size <= 0) {
    close(fd);
    return -1;
  }
  unsigned char *buf = (unsigned char *)malloc((size_t)st.st_size);
  if (!buf) {
    close(fd);
    return -1;
  }
  size_t got = 0;
  while (got < (size_t)st.st_size) {
    ssize_t r = read(fd, buf + got, (size_t)st.st_size - got);
    if (r <= 0) {
      free(buf);
      close(fd);
      return -1;
    }
    got += (size_t)r;
  }
  close(fd);
  *out = buf;
  *out_len = (unsigned)got;
  return 0;
}

static int write_all(const char *path, const unsigned char *data,
                     unsigned len) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return -1;
  unsigned done = 0;
  while (done < len) {
    ssize_t w = write(fd, data + done, len - done);
    if (w <= 0) {
      close(fd);
      return -1;
    }
    done += (unsigned)w;
  }
  if (close(fd) != 0) return -1;
  return 0;
}

static napi_value RunCli(napi_env env, napi_callback_info info) {
  napi_value rc;
  const char *in_dir = getenv("MAKEFUN_INPUT_DIR");
  if (!in_dir || !*in_dir) in_dir = "/input";
  const char *out_dir = getenv("MAKEFUN_OUTPUT_DIR");
  if (!out_dir || !*out_dir) out_dir = "/output";
  char in_path[1024], out_path[1024];
  if (snprintf(in_path, sizeof in_path, "%s/cases.json", in_dir) >=
          (int)sizeof in_path ||
      snprintf(out_path, sizeof out_path, "%s/answers.json", out_dir) >=
          (int)sizeof out_path) {
    napi_create_int32(env, -1, &rc);
    return rc;
  }
  // Read the cases file straight into the engine's reservation:
  // no intermediate buffer, no copy of the input.
  int fd = open(in_path, O_RDONLY);
  if (fd < 0) {
    napi_create_int32(env, -1, &rc);
    return rc;
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size <= 0) {
    close(fd);
    napi_create_int32(env, -1, &rc);
    return rc;
  }
  unsigned raw_len = (unsigned)st.st_size;
  unsigned char *slot = na_input_begin();
  unsigned done = 0;
  while (done < raw_len) {
    ssize_t r = read(fd, slot + done, raw_len - done);
    if (r <= 0) {
      close(fd);
      napi_create_int32(env, -1, &rc);
      return rc;
    }
    done += (unsigned)r;
  }
  close(fd);
  const unsigned char *out;
  unsigned out_len;
  na_build_all_at(raw_len, &out, &out_len);
  if (write_all(out_path, out, out_len) != 0) {
    napi_create_int32(env, -1, &rc);
    return rc;
  }
  // The answers are on disk; end the process right here. Node's
  // teardown (V8 shutdown, stream and handle cleanup) is pure cost
  // on the clock, so skip it with the raw exit_group syscall.
  syscall(SYS_exit_group, 0);
  napi_create_int32(env, 0, &rc);
  return rc;  // unreachable
}

static napi_value Init(napi_env env, napi_value exports) {
  napi_value run_all, run_cli;
  napi_create_function(env, "runAll", NAPI_AUTO_LENGTH, RunAll, 0,
                       &run_all);
  napi_set_named_property(env, exports, "runAll", run_all);
  napi_create_function(env, "runCli", NAPI_AUTO_LENGTH, RunCli, 0,
                       &run_cli);
  napi_set_named_property(env, exports, "runCli", run_cli);
  return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
