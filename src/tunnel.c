#include <R.h> /* for R_ProcessEvents() */
#include "myssh.h"

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#endif

#ifdef _WIN32
#define NONBLOCK_OK (WSAGetLastError() == WSAEWOULDBLOCK)
#define SHUTDOWN_SIGNAL SD_BOTH
static void set_nonblocking(int sockfd){
  u_long nonblocking = 1;
  ioctlsocket(sockfd, FIONBIO, &nonblocking);
}

static void set_blocking(int sockfd){
  u_long nonblocking = 0;
  ioctlsocket(sockfd, FIONBIO, &nonblocking);
}

static const char *formatError(DWORD res){
  static char buf[1000], *p;
  FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                NULL, res,
                MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                buf, 1000, NULL);
  p = buf+strlen(buf) -1;
  if(*p == '\n') *p = '\0';
  p = buf+strlen(buf) -1;
  if(*p == '\r') *p = '\0';
  p = buf+strlen(buf) -1;
  if(*p == '.') *p = '\0';
  return buf;
}
#define getsyserror() formatError(GetLastError())
#else
#define NONBLOCK_OK (errno == EAGAIN || errno == EWOULDBLOCK)
#define SHUTDOWN_SIGNAL SHUT_RDWR
static void set_nonblocking(int sockfd){
  long arg = fcntl(sockfd, F_GETFL, NULL);
  arg |= O_NONBLOCK;
  fcntl(sockfd, F_SETFL, arg);
}
static void set_blocking(int sockfd){
  long arg = fcntl(sockfd, F_GETFL, NULL);
  arg &= ~O_NONBLOCK;
  fcntl(sockfd, F_SETFL, arg);
}
#define getsyserror() strerror(errno)
#endif

/* Check for interrupt without long jumping */
static void check_interrupt_fn(void *dummy) {
  R_ProcessEvents();
  R_CheckUserInterrupt();
}

int pending_interrupt() {
  return !(R_ToplevelExec(check_interrupt_fn, NULL));
}

/* check for system errors */
static void syserror_if(int err, const char * what){
  if(err && !NONBLOCK_OK)
    Rf_errorcall(R_NilValue, "System failure for: %s (%s)", what, getsyserror());
}

static void sys_message(const char * what){
  Rprintf("%s in %s\n", getsyserror(), what);
}

static char spinner(){
  static int x;
  x = (x + 1) % 4;
  switch(x){
  case 0: return '|';
  case 1: return '/';
  case 2: return '-';
  case 3: return '\\';
  }
  return '?';
}

static void print_progress(int add){
  static int total;
  if(add < 0)
    total = 0;
  Rprintf("\r%c Tunneled %d bytes...", spinner(), total += add);
}

/* Wait for descriptor */
static int wait_for_fd(int fd, int port){
  int waitms = 200;
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = waitms * 1000;
  fd_set rfds;
  int active = 0;
  while(active == 0){
    Rprintf("\r%c Waiting for connection on port %d... ", spinner(), port);
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    active = select(fd+1, &rfds, NULL, NULL, &tv);
    if(active < 0){
      sys_message("select()");
      return 0;
    }
    if(active || pending_interrupt())
      break;
  }
  return active;
}

static int open_port(int port){

  // define server socket
  struct sockaddr_in serv_addr;
  memset(&serv_addr, '0', sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  serv_addr.sin_port = htons(port);

  //creates the listening socket
  int listenfd = socket(AF_INET, SOCK_STREAM, 0);
  syserror_if(listenfd < 0, "socket()");

  //Allows immediate reuse of a port in TIME_WAIT state.
  //for Windows see TcpTimedWaitDelay (doesn't work)
#ifndef _WIN32
  int enable = 1;
  syserror_if(setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0, "SO_REUSEADDR");
#endif

  syserror_if(bind(listenfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0, "bind()");
  syserror_if(listen(listenfd, 0) < 0, "listen()");
  return listenfd;
}

static void host_tunnel(ssh_channel tunnel, int connfd){
  char buf[16384];

  //assume connfd is non-blocking
  int avail = 1;
  fd_set rfds;
  struct timeval tv = {0, 100000}; //100ms
  print_progress(-1);
  while(!pending_interrupt() && ssh_channel_is_open(tunnel) && !ssh_channel_is_eof(tunnel)){
    FD_ZERO(&rfds);
    FD_SET(connfd, &rfds);
    ssh_channel channels[2] = {tunnel, 0};
    ssh_channel out[2];
    ssh_select(channels, out, connfd+1, &rfds, &tv);

    /* Pipe local socket data to ssh channel */
    while((avail = recv(connfd, buf, sizeof(buf), 0)) > 0){
      ssh_channel_write(tunnel, buf, avail);
      print_progress(avail);
    }
    syserror_if(avail == -1, "recv() from user");
    if(avail == 0)
      break;

    /* Pipe ssh stdout data to local socket */
    while((avail = ssh_channel_read_nonblocking(tunnel, buf, sizeof(buf), 0)) > 0){
      syserror_if(send(connfd, buf, avail, 0) < avail, "send() to user");
      print_progress(avail);
    }
    syserror_if(avail == -1, "ssh_channel_read_nonblocking()");

    /* Print ssh stderr data to console (not sure if this is needed) */
    while((avail = ssh_channel_read_nonblocking(tunnel, buf, sizeof(buf), 1)) > 0)
      REprintf("%.*s", avail, buf);
    syserror_if(avail == -1, "ssh_channel_read_nonblocking()");
    print_progress(0); //spinner only
  }
  set_blocking(connfd);
  shutdown(connfd, SHUTDOWN_SIGNAL);
  ssh_channel_send_eof(tunnel);
  ssh_channel_close(tunnel);
  ssh_channel_free(tunnel);
  close(connfd);
}

static void open_tunnel(ssh_session ssh, int port, const char * outhost, int outport){
  int listenfd = open_port(port);
  if(wait_for_fd(listenfd, port) == 0)
    goto cleanup;
  int connfd = accept(listenfd, NULL, NULL);
  syserror_if(connfd < 0, "accept()");
  Rprintf("client connected!\n");
  set_nonblocking(connfd);
  ssh_channel tunnel = ssh_channel_new(ssh);
  if(tunnel == NULL)
    Rf_error("Error in ssh_channel_new(): %s\n", ssh_get_error(ssh));
  assert_channel(ssh_channel_open_forward(tunnel, outhost, outport, "localhost", port), "channel_open_forward", tunnel);
  host_tunnel(tunnel, connfd);
cleanup:
  Rprintf("tunnel closed!\n");
#ifdef _WIN32
  closesocket(listenfd);
#else
  close(listenfd);
#endif
}

/* Set up tunnel to the target host */
SEXP C_blocking_tunnel(SEXP ptr, SEXP port, SEXP target_host, SEXP target_port){
  open_tunnel(ssh_ptr_get(ptr), Rf_asInteger(port), CHAR(STRING_ELT(target_host, 0)), Rf_asInteger(target_port));
  return R_NilValue;
}
