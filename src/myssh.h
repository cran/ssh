#define R_NO_REMAP
#define STRICT_R_HEADERS

#include <Rinternals.h>
#include <stdlib.h>
#include <string.h>
#include <libssh/libssh.h>
#include <libssh/callbacks.h>


/* Debian ships with libssh 0.8.0 which has a bug so it looks like 0.7.0 */
#if defined(LIBSSH_VERSION_INT) && (LIBSSH_VERSION_INT >= SSH_VERSION_INT(0,8,0) || LIBSSH_VERSION_INT == SSH_VERSION_INT(0,7,0))
#define myssh_get_publickey ssh_get_server_publickey
#else
#define myssh_get_publickey ssh_get_publickey
#endif

#define make_string(x) x ? Rf_mkString(x) : Rf_ScalarString(NA_STRING)
ssh_session ssh_ptr_get(SEXP ptr);
int pending_interrupt();
void assert_channel(int rc, const char * what, ssh_channel channel);

/* Workaround from libcurl: https://github.com/curl/curl/pull/9383/files */
#if defined(__GNUC__) && (LIBSSH_VERSION_MINOR >= 10) || (LIBSSH_VERSION_MAJOR > 0)
# pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
