#include "myssh.h"

void assert_channel(int rc, const char * what, ssh_channel channel){
  if (rc != SSH_OK){
    char buf[1024];
    strncpy(buf, ssh_get_error(ssh_channel_get_session(channel)), 1023);
    ssh_channel_close (channel);
    ssh_channel_free(channel);
    Rf_errorcall(R_NilValue, "libssh failure at '%s': %s", what, buf);
  }
}

static void R_callback(SEXP fun, const char * buf, ssize_t len){
  if(!Rf_isFunction(fun)) return;
  int ok;
  SEXP str = PROTECT(Rf_allocVector(RAWSXP, len));
  memcpy(RAW(str), buf, len);
  SEXP call = PROTECT(Rf_lcons(fun, Rf_lcons(str, R_NilValue)));
  R_tryEval(call, R_GlobalEnv, &ok);
  UNPROTECT(2);
}

/* Set up tunnel to the target host */
SEXP C_ssh_exec(SEXP ptr, SEXP command, SEXP outfun, SEXP errfun){
  ssh_session ssh = ssh_ptr_get(ptr);
  ssh_channel channel = ssh_channel_new(ssh);
  if(channel == NULL)
    Rf_error("Error in ssh_channel_new(): %s\n", ssh_get_error(ssh));
  assert_channel(ssh_channel_open_session(channel), "ssh_channel_open_session", channel);
  assert_channel(ssh_channel_request_exec(channel, CHAR(STRING_ELT(command, 0))), "ssh_channel_request_exec", channel);

  int nbytes;
  int status = NA_INTEGER;
  static char buffer[1024];
  struct timeval tv = {0, 100000}; //100ms
  while(ssh_channel_is_open(channel) && !ssh_channel_is_eof(channel)){
    ssh_channel readchans[2] = {channel, 0};
    ssh_channel_select(readchans, NULL, NULL, &tv);
    if(pending_interrupt())
      goto cleanup;
    for(int stream = 0; stream < 2; stream++){
      while ((nbytes = ssh_channel_read_nonblocking(channel, buffer, sizeof(buffer), stream)) > 0)
        R_callback(stream ? errfun : outfun, buffer, nbytes);
      assert_channel(nbytes == SSH_ERROR, "ssh_channel_read_nonblocking", channel);
    }
  }
  //this blocks until command has completed
  status = ssh_channel_get_exit_status(channel);

  //jump directly to here for interruptions
cleanup:
  ssh_channel_close(channel);
  ssh_channel_free(channel);
  return Rf_ScalarInteger(status);
}
