/* cecdump — passive HDMI input-name dumper for Samsung Tizen 5.x TVs.
 *
 * Reads /dev/micom-cec, the TV's raw CEC passthrough (mode 0666, SMACK `*`),
 * and learns the real names of devices on each HDMI port from the TV's own
 * discovery dialogue — the same names Samsung's source bar shows. Serves the
 * per-port label map as JSON on http://+:8787/ for a custom launcher such as
 * CleanFrame, and persists learned names so restarts start warm.
 *
 * The micom delivers CEC frames prefixed with a length byte (0x80|len) and
 * accepts writes but never transmits foreign frames, so this is a passive
 * listener: <Set OSD Name> (0x47) names a device by initiator address and
 * <Report Physical Address> (0x84) maps it to an HDMI port (PA top nibble)
 * and its depth behind a receiver. A port's label is the CEC name of the
 * shallowest device on it.
 *
 * Env overrides (used by the container test harness):
 *   CECD_DEV   device or FIFO to read       (default /dev/micom-cec)
 *   CECD_STATE persistence file             (default /opt/usr/share/tvroot/cec/cec-names.txt)
 *   CECD_PORT  HTTP listen port             (default 8787)
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_DEV 16
#define NAME_MAX 48
#define FRAME_MAX 64

static const char *dev_path = "/dev/micom-cec";
static const char *state_path = "/opt/usr/share/tvroot/cec/cec-names.txt";
static int port = 8787;
/* The micom read parks in an uninterruptible driver wait forever, so the
   reader thread must never exist unless the listener is already up — a
   failed bind must exit before the device is opened, or the D-state thread
   strands the process and every retry (and the shell waiting on it). */
static int listen_sock = -1;
static int micom_fd = -1;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char names[MAX_DEV][NAME_MAX];
static int port_by_addr[MAX_DEV];
static int depth_by_addr[MAX_DEV];
static char frames[64][FRAME_MAX * 2 + 1];
static int frame_n;

static void log_line(const char *msg) {
  fprintf(stderr, "cecdump: %s\n", msg);
  fflush(stderr);
}

/* Persist the same line format as the original CecService TPK so a future
   swap between the two stays seamless: N<addr>|<name>  P<addr>|<port>|<depth>. */
static void save_names(void) {
  FILE *f = fopen(state_path, "w");
  int i;
  if (!f) { log_line("state open failed"); return; }
  for (i = 0; i < MAX_DEV; i++) {
    if (names[i][0]) fprintf(f, "N%d|%s\n", i, names[i]);
    if (port_by_addr[i]) fprintf(f, "P%d|%d|%d\n", i, port_by_addr[i], depth_by_addr[i]);
  }
  fclose(f);
}

static void load_names(void) {
  FILE *f = fopen(state_path, "r");
  char line[128];
  if (!f) return;
  while (fgets(line, sizeof line, f)) {
    int a, p, d, n;
    if (sscanf(line, "N%d|%n", &a, &n) == 1 && a >= 0 && a < MAX_DEV) {
      char *s = line + n, *e = strpbrk(s, "\r\n");
      if (e) *e = '\0';
      if (s[0]) { strncpy(names[a], s, NAME_MAX - 1); names[a][NAME_MAX - 1] = '\0'; }
    } else if (sscanf(line, "P%d|%d|%d", &a, &p, &d) == 3 && a >= 0 && a < MAX_DEV) {
      port_by_addr[a] = p; depth_by_addr[a] = d;
    }
  }
  fclose(f);
}

/* Frames are [0x80|len][header][op][args…]. Only record 0x47 Set OSD Name
   (by initiator) and 0x84 Report Physical Address (PA top nibble = HDMI
   port; depth = count of non-zero PA nibbles below the port). */
static void parse_frame(const unsigned char *b, int n) {
  int len, init, op, i;
  if (n < 3) return;
  len = b[0] & 0x0F;
  if (1 + len > n) return;
  init = (b[1] >> 4) & 0x0F;
  op = b[2];
  if (op == 0x47 && len >= 3) {
    int name_len = len - 2;
    if (name_len >= NAME_MAX) name_len = NAME_MAX - 1;
    for (i = 0; i < name_len; i++) {
      char c = (char)b[3 + i];
      if (c == '\0' || !isprint((unsigned char)c)) { name_len = i; break; }
    }
    while (name_len > 0 && isspace((unsigned char)b[3 + name_len - 1])) name_len--;
    memcpy(names[init], b + 3, (size_t)name_len);
    names[init][name_len] = '\0';
  } else if (op == 0x84 && len >= 4) {
    int p = (b[3] >> 4) & 0x0F, d = 0;
    if ((b[3] & 0x0F) != 0) d++;
    if ((b[4] >> 4) != 0) d++;
    if ((b[4] & 0x0F) != 0) d++;
    if (p >= 1 && p <= 4) { port_by_addr[init] = p; depth_by_addr[init] = d; }
  }
}

static void record_frame(const unsigned char *b, int n) {
  char hex[FRAME_MAX * 2 + 1];
  int i;
  if (n > FRAME_MAX) n = FRAME_MAX;
  for (i = 0; i < n; i++) sprintf(hex + i * 2, "%02x", b[i]);
  hex[n * 2] = '\0';
  strncpy(frames[frame_n % 64], hex, sizeof frames[0] - 1);
  frames[frame_n % 64][sizeof frames[0] - 1] = '\0';
  frame_n++;
}

static void *reader_thread(void *unused) {
  unsigned char buf[FRAME_MAX];
  unsigned char residue[FRAME_MAX + 16];
  int fd, n, res_n = 0, i;
  (void)unused;
  fd = open(dev_path, O_RDWR);
  if (fd < 0) fd = open(dev_path, O_RDONLY);
  if (fd < 0) { log_line("cannot open CEC device; HTTP-only"); return NULL; }
  micom_fd = fd;
  log_line("passive cec listen");
  for (;;) {
    n = (int)read(fd, buf, sizeof buf);
    if (n <= 0) break; /* EOF on a FIFO; the micom read otherwise blocks forever */
    if (res_n + n > (int)sizeof residue) res_n = 0; /* overflow: drop, resync */
    memcpy(residue + res_n, buf, (size_t)n);
    res_n += n;
    pthread_mutex_lock(&lock);
    /* Reads can deliver several frames at once (or a split one); walk the
       stream and keep any partial tail for the next read. */
    i = 0;
    while (i < res_n) {
      int len = residue[i] & 0x0F;
      if (i + 1 + len > res_n) break;
      parse_frame(residue + i, 1 + len);
      record_frame(residue + i, 1 + len);
      i += 1 + len;
    }
    if (i > 0) { memmove(residue, residue + i, (size_t)(res_n - i)); res_n -= i; }
    save_names();
    pthread_mutex_unlock(&lock);
  }
  close(fd);
  log_line("cec read ended");
  return NULL;
}

static void json_escape(char *out, size_t cap, const char *s) {
  size_t o = 0;
  out[0] = '\0';
  for (; *s && o + 3 < cap; s++) {
    if (*s == '"' || *s == '\\') { out[o++] = '\\'; out[o++] = *s; }
    else out[o++] = *s;
  }
  out[o] = '\0';
}

/* Port labels: the CEC name of the shallowest device on the port, or null
   until one is learned (the launcher falls back to "HDMI N"). */
static void build_json(char *out, size_t cap) {
  char esc[NAME_MAX * 2 + 1], *p = out, *end = out + cap;
  int port_name[MAX_DEV], port_depth[MAX_DEV], i;
  for (i = 0; i < MAX_DEV; i++) { port_name[i] = -1; port_depth[i] = 99; }
  for (i = 0; i < MAX_DEV; i++) {
    if (!names[i][0] || !port_by_addr[i]) continue;
    if (depth_by_addr[i] <= port_depth[port_by_addr[i] - 1]) {
      port_depth[port_by_addr[i] - 1] = depth_by_addr[i];
      port_name[port_by_addr[i] - 1] = i;
    }
  }
  p += snprintf(p, (size_t)(end - p), "{");
  for (i = 0; i < 4; i++) {
    p += snprintf(p, (size_t)(end - p), "%s\"hdmi%d\":", i ? "," : "", i + 1);
    if (port_name[i] >= 0) { json_escape(esc, sizeof esc, names[port_name[i]]); p += snprintf(p, (size_t)(end - p), "\"%s\"", esc); }
    else p += snprintf(p, (size_t)(end - p), "null");
  }
  p += snprintf(p, (size_t)(end - p), ",\"devices\":{");
  {
    int first = 1;
    for (i = 0; i < MAX_DEV; i++) {
      if (!names[i][0]) continue;
      json_escape(esc, sizeof esc, names[i]);
      if (port_by_addr[i])
        p += snprintf(p, (size_t)(end - p), "%s\"a%d\":{\"name\":\"%s\",\"port\":%d}", first ? "" : ",", i, esc, port_by_addr[i]);
      else
        p += snprintf(p, (size_t)(end - p), "%s\"a%d\":{\"name\":\"%s\"}", first ? "" : ",", i, esc);
      first = 0;
    }
  }
  p += snprintf(p, (size_t)(end - p), "},\"cec\":[");
  for (i = 0; i < frame_n && i < 64; i++) {
    p += snprintf(p, (size_t)(end - p), "%s\"%s\"", i ? "," : "", frames[i % 64]);
  }
  p += snprintf(p, (size_t)(end - p), "]}");
  (void)p;
}

/* The listener comes up before the micom is touched (see the note at the
   top of this file), so a failed bind never strands the reader thread. */
static int open_listener(void) {
  int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
  struct sockaddr_in sa;
  if (ls < 0) { log_line("socket failed"); return -1; }
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET; sa.sin_port = htons((unsigned short)port); sa.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(ls, (struct sockaddr *)&sa, sizeof sa) < 0) {
    fprintf(stderr, "cecdump: bind errno=%d (%s)\n", errno, strerror(errno)); close(ls); return -1;
  }
  if (listen(ls, 8) < 0) {
    fprintf(stderr, "cecdump: listen errno=%d (%s)\n", errno, strerror(errno)); close(ls); return -1;
  }
  log_line("listening");
  return ls;
}

static void serve(void) {
  int ls = listen_sock;
  if (ls < 0) return;
  for (;;) {
    char json[16384], head[128], req[1024];
    int c = accept(ls, NULL, NULL), n;
    ssize_t r;
    if (c < 0) continue;
    r = recv(c, req, sizeof req - 1, 0); (void)r; /* request content is irrelevant */
    pthread_mutex_lock(&lock);
    build_json(json, sizeof json);
    pthread_mutex_unlock(&lock);
    n = snprintf(head, sizeof head,
                 "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                 strlen(json));
    send(c, head, (size_t)n, MSG_NOSIGNAL);
    send(c, json, strlen(json), MSG_NOSIGNAL);
    close(c);
  }
}

static void on_signal(int sig) {
  (void)sig;
  log_line("exiting");
  /* Free the listener so a restart can rebind even if the reader thread's
     D-state wait strands the process. */
  if (listen_sock >= 0) { close(listen_sock); listen_sock = -1; }
  if (micom_fd >= 0) { close(micom_fd); micom_fd = -1; }
  exit(0);
}

int main(void) {
  pthread_t t;
  const char *env;
  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);
  signal(SIGPIPE, SIG_IGN);
  if ((env = getenv("CECD_DEV"))) dev_path = env;
  if ((env = getenv("CECD_STATE"))) state_path = env;
  if ((env = getenv("CECD_PORT"))) port = atoi(env);
  pthread_mutex_lock(&lock);
  load_names();
  pthread_mutex_unlock(&lock);
  /* Listener first: if the port is taken, exit cleanly with no reader. */
  listen_sock = open_listener();
  if (listen_sock < 0) return 1;
  /* Only now touch the micom: the reader parks in an uninterruptible wait
     and must never exist in a process that is about to exit. */
  if (pthread_create(&t, NULL, reader_thread, NULL) != 0) log_line("reader thread failed");
  else pthread_detach(t);
  serve();
  return 0;
}
