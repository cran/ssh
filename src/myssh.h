#define R_NO_REMAP
#define STRICT_R_HEADERS

#include <Rinternals.h>
#include <libssh/libssh.h>
#include <libssh/callbacks.h>
#include <stdlib.h>
#include <string.h>

#define make_string(x) x ? Rf_mkString(x) : Rf_ScalarString(NA_STRING)
ssh_session ssh_ptr_get(SEXP ptr);
int pending_interrupt();
void assert_channel(int rc, const char * what, ssh_channel channel);
