// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

#include <winpty_constants.h>

#include "nvim/os/os.h"
#include "nvim/ascii.h"
#include "nvim/memory.h"
#include "nvim/mbyte.h"  // for utf8_to_utf16, utf16_to_utf8
#include "nvim/os/pty_process_win.h"
#include "nvim/os/pty_conpty_win.h"

#ifdef INCLUDE_GENERATED_DECLARATIONS
# include "os/pty_process_win.c.generated.h"
#endif

static void CALLBACK pty_process_finish1(void *context, BOOLEAN unused)
  FUNC_ATTR_NONNULL_ALL
{
  PtyProcess *ptyproc = (PtyProcess *)context;
  Process *proc = (Process *)ptyproc;

  if (ptyproc->type == kConpty
      && ptyproc->object.conpty != NULL) {
    os_conpty_free(ptyproc->object.conpty);
    ptyproc->object.conpty = NULL;
  }
  uv_timer_init(&proc->loop->uv, &ptyproc->wait_eof_timer);
  ptyproc->wait_eof_timer.data = (void *)ptyproc;
  uv_timer_start(&ptyproc->wait_eof_timer, wait_eof_timer_cb, 200, 200);
}

/// @returns zero on success, or negative error code.
int pty_process_spawn(PtyProcess *ptyproc)
  FUNC_ATTR_NONNULL_ALL
{
  Process *proc = (Process *)ptyproc;
  int status = 0;
  winpty_error_ptr_t err = NULL;
  winpty_config_t *cfg = NULL;
  winpty_spawn_config_t *spawncfg = NULL;
  winpty_t *winpty_object = NULL;
  conpty_t *conpty_object = NULL;
  char *in_name = NULL;
  char *out_name = NULL;
  HANDLE process_handle = NULL;
  uv_connect_t *in_req = NULL;
  uv_connect_t *out_req = NULL;
  wchar_t *cmd_line = NULL;
  wchar_t *cwd = NULL;
  wchar_t *env = NULL;
  const char *emsg = NULL;

  assert(proc->err.closed);

  if (os_has_conpty_working()) {
    if ((conpty_object =
         os_conpty_init(&in_name, &out_name,
                        ptyproc->width, ptyproc->height)) != NULL) {
      ptyproc->type = kConpty;
    }
  }

  if (ptyproc->type == kWinpty) {
    cfg = winpty_config_new(WINPTY_FLAG_ALLOW_CURPROC_DESKTOP_CREATION, &err);
    if (cfg == NULL) {
      emsg = "winpty_config_new failed";
      goto cleanup;
    }

    winpty_config_set_initial_size(cfg, ptyproc->width, ptyproc->height);
    winpty_object = winpty_open(cfg, &err);
    if (winpty_object == NULL) {
      emsg = "winpty_open failed";
      goto cleanup;
    }

    status = utf16_to_utf8(winpty_conin_name(winpty_object), -1, &in_name);
    if (status != 0) {
      emsg = "utf16_to_utf8(winpty_conin_name) failed";
      goto cleanup;
    }

    status = utf16_to_utf8(winpty_conout_name(winpty_object), -1, &out_name);
    if (status != 0) {
      emsg = "utf16_to_utf8(winpty_conout_name) failed";
      goto cleanup;
    }
  }

  if (!proc->in.closed) {
    in_req = xmalloc(sizeof(uv_connect_t));
    uv_pipe_connect(
        in_req,
        &proc->in.uv.pipe,
        in_name,
        pty_process_connect_cb);
  }

  if (!proc->out.closed) {
    out_req = xmalloc(sizeof(uv_connect_t));
    uv_pipe_connect(
        out_req,
        &proc->out.uv.pipe,
        out_name,
        pty_process_connect_cb);
  }

  if (proc->cwd != NULL) {
    status = utf8_to_utf16(proc->cwd, -1, &cwd);
    if (status != 0) {
      emsg = "utf8_to_utf16(proc->cwd) failed";
      goto cleanup;
    }
  }

  status = build_cmd_line(proc->argv, &cmd_line,
                          os_shell_is_cmdexe(proc->argv[0]));
  if (status != 0) {
    emsg = "build_cmd_line failed";
    goto cleanup;
  }

  if (proc->env != NULL) {
    status = build_env_block(proc->env, &env);
  }

  if (status != 0) {
    emsg = "build_env_block failed";
    goto cleanup;
  }

  if (ptyproc->type == kConpty) {
    if (!os_conpty_spawn(conpty_object,
                         &process_handle,
                         NULL,
                         cmd_line,
                         cwd,
                         env)) {
      emsg = "os_conpty_spawn failed";
      status = (int)GetLastError();
      goto cleanup;
    }
  } else {
    spawncfg = winpty_spawn_config_new(
        WINPTY_SPAWN_FLAG_AUTO_SHUTDOWN,
        NULL,  // Optional application name
        cmd_line,
        cwd,
        env,
        &err);
    if (spawncfg == NULL) {
      emsg = "winpty_spawn_config_new failed";
      goto cleanup;
    }

    DWORD win_err = 0;
    if (!winpty_spawn(winpty_object,
                      spawncfg,
                      &process_handle,
                      NULL,  // Optional thread handle
                      &win_err,
                      &err)) {
      if (win_err) {
        status = (int)win_err;
        emsg = "failed to spawn process";
      } else {
        emsg = "winpty_spawn failed";
      }
      goto cleanup;
    }
  }
  proc->pid = (int)GetProcessId(process_handle);

  if (!RegisterWaitForSingleObject(
      &ptyproc->finish_wait,
      process_handle,
      pty_process_finish1,
      ptyproc,
      INFINITE,
      WT_EXECUTEDEFAULT | WT_EXECUTEONLYONCE)) {
    abort();
  }

  // Wait until pty_process_connect_cb is called.
  while ((in_req != NULL && in_req->handle != NULL)
         || (out_req != NULL && out_req->handle != NULL)) {
    uv_run(&proc->loop->uv, UV_RUN_ONCE);
  }

  (ptyproc->type == kConpty) ?
    (void *)(ptyproc->object.conpty = conpty_object) :
    (void *)(ptyproc->object.winpty = winpty_object);
  ptyproc->process_handle = process_handle;
  winpty_object = NULL;
  conpty_object = NULL;
  process_handle = NULL;

cleanup:
  if (status) {
    // In the case of an error of MultiByteToWideChar or CreateProcessW.
    ELOG("pty_process_spawn: %s: error code: %d", emsg, status);
    status = os_translate_sys_error(status);
  } else if (err != NULL) {
    status = (int)winpty_error_code(err);
    ELOG("pty_process_spawn: %s: error code: %d", emsg, status);
    status = translate_winpty_error(status);
  }
  winpty_error_free(err);
  winpty_config_free(cfg);
  winpty_spawn_config_free(spawncfg);
  winpty_free(winpty_object);
  os_conpty_free(conpty_object);
  xfree(in_name);
  xfree(out_name);
  if (process_handle != NULL) {
    CloseHandle(process_handle);
  }
  xfree(in_req);
  xfree(out_req);
  xfree(cmd_line);
  xfree(env);
  xfree(cwd);
  return status;
}

const char *pty_process_tty_name(PtyProcess *ptyproc)
{
  return "?";
}

void pty_process_resize(PtyProcess *ptyproc, uint16_t width,
                        uint16_t height)
  FUNC_ATTR_NONNULL_ALL
{
  if (ptyproc->type == kConpty
      && ptyproc->object.conpty != NULL) {
    os_conpty_set_size(ptyproc->object.conpty, width, height);
  } else if (ptyproc->object.winpty != NULL) {
    winpty_set_size(ptyproc->object.winpty, width, height, NULL);
  }
}

void pty_process_close(PtyProcess *ptyproc)
  FUNC_ATTR_NONNULL_ALL
{
  Process *proc = (Process *)ptyproc;

  pty_process_close_master(ptyproc);

  if (proc->internal_close_cb) {
    proc->internal_close_cb(proc);
  }
}

void pty_process_close_master(PtyProcess *ptyproc)
  FUNC_ATTR_NONNULL_ALL
{
  if (ptyproc->type == kWinpty
      && ptyproc->object.winpty != NULL) {
    winpty_free(ptyproc->object.winpty);
    ptyproc->object.winpty = NULL;
  }
}

void pty_process_teardown(Loop *loop)
  FUNC_ATTR_NONNULL_ALL
{
}

static void pty_process_connect_cb(uv_connect_t *req, int status)
  FUNC_ATTR_NONNULL_ALL
{
  assert(status == 0);
  req->handle = NULL;
}

static void wait_eof_timer_cb(uv_timer_t *wait_eof_timer)
  FUNC_ATTR_NONNULL_ALL
{
  PtyProcess *ptyproc = wait_eof_timer->data;
  Process *proc = (Process *)ptyproc;

  if (proc->out.closed || !uv_is_readable(proc->out.uvstream)) {
    uv_timer_stop(&ptyproc->wait_eof_timer);
    pty_process_finish2(ptyproc);
  }
}

static void pty_process_finish2(PtyProcess *ptyproc)
  FUNC_ATTR_NONNULL_ALL
{
  Process *proc = (Process *)ptyproc;

  UnregisterWaitEx(ptyproc->finish_wait, NULL);
  uv_close((uv_handle_t *)&ptyproc->wait_eof_timer, NULL);

  DWORD exit_code = 0;
  GetExitCodeProcess(ptyproc->process_handle, &exit_code);
  proc->status = proc->exit_signal ? 128 + proc->exit_signal : (int)exit_code;

  CloseHandle(ptyproc->process_handle);
  ptyproc->process_handle = NULL;

  proc->internal_exit_cb(proc);
}

/// Build the command line to pass to CreateProcessW.
///
/// @param[in]  argv  Array with string arguments.
/// @param[out]  cmd_line  Location where saved builded cmd line.
///
/// @returns zero on success, or error code of MultiByteToWideChar function.
///
static int build_cmd_line(char **argv, wchar_t **cmd_line, bool is_cmdexe)
  FUNC_ATTR_NONNULL_ALL
{
  size_t utf8_cmd_line_len = 0;
  size_t argc = 0;
  QUEUE args_q;

  QUEUE_INIT(&args_q);
  while (*argv) {
    size_t buf_len = is_cmdexe ? (strlen(*argv) + 1) : (strlen(*argv) * 2 + 3);
    ArgNode *arg_node = xmalloc(sizeof(*arg_node));
    arg_node->arg = xmalloc(buf_len);
    if (is_cmdexe) {
      xstrlcpy(arg_node->arg, *argv, buf_len);
    } else {
      quote_cmd_arg(arg_node->arg, buf_len, *argv);
    }
    utf8_cmd_line_len += strlen(arg_node->arg);
    QUEUE_INIT(&arg_node->node);
    QUEUE_INSERT_TAIL(&args_q, &arg_node->node);
    argc++;
    argv++;
  }

  utf8_cmd_line_len += argc;
  char *utf8_cmd_line = xmalloc(utf8_cmd_line_len);
  *utf8_cmd_line = NUL;
  QUEUE *q;
  QUEUE_FOREACH(q, &args_q, {
    ArgNode *arg_node = QUEUE_DATA(q, ArgNode, node);
    xstrlcat(utf8_cmd_line, arg_node->arg, utf8_cmd_line_len);
    xfree(arg_node->arg);
    xfree(arg_node);
    QUEUE_REMOVE(q);
    if (!QUEUE_EMPTY(&args_q)) {
      xstrlcat(utf8_cmd_line, " ", utf8_cmd_line_len);
    }
  })

  int result = utf8_to_utf16(utf8_cmd_line, -1, cmd_line);
  xfree(utf8_cmd_line);
  return result;
}

/// Emulate quote_cmd_arg of libuv and quotes command line argument.
/// Most of the code came from libuv.
///
/// @param[out]  dest  Location where saved quotes argument.
/// @param  dest_remaining  Destination buffer size.
/// @param[in]  src Pointer to argument.
///
static void quote_cmd_arg(char *dest, size_t dest_remaining, const char *src)
  FUNC_ATTR_NONNULL_ALL
{
  size_t src_len = strlen(src);
  bool quote_hit = true;
  char *start = dest;

  if (src_len == 0) {
    // Need double quotation for empty argument.
    snprintf(dest, dest_remaining, "\"\"");
    return;
  }

  if (NULL == strpbrk(src, " \t\"")) {
    // No quotation needed.
    xstrlcpy(dest, src, dest_remaining);
    return;
  }

  if (NULL == strpbrk(src, "\"\\")) {
    // No embedded double quotes or backlashes, so I can just wrap quote marks.
    // around the whole thing.
    snprintf(dest, dest_remaining, "\"%s\"", src);
    return;
  }

  // Expected input/output:
  //   input : 'hello"world'
  //   output: '"hello\"world"'
  //   input : 'hello""world'
  //   output: '"hello\"\"world"'
  //   input : 'hello\world'
  //   output: 'hello\world'
  //   input : 'hello\\world'
  //   output: 'hello\\world'
  //   input : 'hello\"world'
  //   output: '"hello\\\"world"'
  //   input : 'hello\\"world'
  //   output: '"hello\\\\\"world"'
  //   input : 'hello world\'
  //   output: '"hello world\\"'

  assert(dest_remaining--);
  *(dest++) = NUL;
  assert(dest_remaining--);
  *(dest++) = '"';
  for (size_t i = src_len; i > 0; i--) {
    assert(dest_remaining--);
    *(dest++) = src[i - 1];
    if (quote_hit && src[i - 1] == '\\') {
      assert(dest_remaining--);
      *(dest++) = '\\';
    } else if (src[i - 1] == '"') {
      quote_hit = true;
      assert(dest_remaining--);
      *(dest++) = '\\';
    } else {
      quote_hit = false;
    }
  }
  assert(dest_remaining);
  *dest = '"';

  while (start < dest) {
    char tmp = *start;
    *start = *dest;
    *dest = tmp;
    start++;
    dest--;
  }
}

/// Translate winpty error code to libuv error.
///
/// @param[in]  winpty_errno  Winpty error code returned by winpty_error_code
///                           function.
///
/// @returns  Error code of libuv error.
int translate_winpty_error(int winpty_errno)
{
  if (winpty_errno <= 0) {
    return winpty_errno;  // If < 0 then it's already a libuv error.
  }

  switch (winpty_errno) {
    case WINPTY_ERROR_OUT_OF_MEMORY:                return UV_ENOMEM;
    case WINPTY_ERROR_SPAWN_CREATE_PROCESS_FAILED:  return UV_EAI_FAIL;
    case WINPTY_ERROR_LOST_CONNECTION:              return UV_ENOTCONN;
    case WINPTY_ERROR_AGENT_EXE_MISSING:            return UV_ENOENT;
    case WINPTY_ERROR_UNSPECIFIED:                   return UV_UNKNOWN;
    case WINPTY_ERROR_AGENT_DIED:                   return UV_ESRCH;
    case WINPTY_ERROR_AGENT_TIMEOUT:                return UV_ETIMEDOUT;
    case WINPTY_ERROR_AGENT_CREATION_FAILED:        return UV_EAI_FAIL;
    default:                                        return UV_UNKNOWN;
  }
}

typedef struct EnvNode {
  wchar_t *str;
  size_t len;
  QUEUE node;
} EnvNode;

/// Build the environment block to pass to CreateProcessW.
///
/// @param[in]  denv  Dict of environment name/value pairs
/// @param[out]  env  Allocated environment block
///
/// @returns zero on success or error code of MultiByteToWideChar function.
static int build_env_block(dict_T *denv, wchar_t **env_block)
{
  const size_t denv_size = (size_t)tv_dict_len(denv);
  size_t env_block_len = 0;
  int rc;
  char **env = tv_dict_to_env(denv);

  QUEUE *q;
  QUEUE env_q;
  QUEUE_INIT(&env_q);
  // Convert env vars to wchar_t and calculate how big the final env block
  // needs to be
  for (size_t i = 0; i < denv_size; i++) {
    EnvNode *env_node = xmalloc(sizeof(*env_node));
    rc = utf8_to_utf16(env[i], -1, &env_node->str);
    if (rc != 0) {
      goto cleanup;
    }
    env_node->len = wcslen(env_node->str) + 1;
    env_block_len += env_node->len;
    QUEUE_INSERT_TAIL(&env_q, &env_node->node);
  }

  // Additional '\0' after the final entry
  env_block_len++;

  *env_block = xmalloc(sizeof(**env_block) * env_block_len);
  wchar_t *pos = *env_block;

  QUEUE_FOREACH(q, &env_q, {
    EnvNode *env_node = QUEUE_DATA(q, EnvNode, node);
    memcpy(pos, env_node->str, env_node->len * sizeof(*pos));
    pos += env_node->len;
  })

  *pos = L'\0';

cleanup:
  q = QUEUE_HEAD(&env_q);
  while (q != &env_q) {
    QUEUE *next = q->next;
    EnvNode *env_node = QUEUE_DATA(q, EnvNode, node);
    XFREE_CLEAR(env_node->str);
    QUEUE_REMOVE(q);
    xfree(env_node);
    q = next;
  }

  return rc;
}
