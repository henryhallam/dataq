/* Simple Linux client for DATAQ DI-718B-E(S) lab data acquisition system
 * by Henry Hallam <henry@pericynthion.org>, 2016-06-07
 * Licensed under CC0 https://creativecommons.org/publicdomain/zero/1.0/
 * Or alternately under MIT License https://www.debian.org/legal/licenses/mit
 * 
 * You will probably want to adjust n_chans and fullscale below.
 *
 * This should be portable to any POSIX system (e.g. OSX, Windows with Cygwin),
 * but it's only been tested on Linux.
 *
 * Protocol reverse-engineered from WinDAQ via Wireshark, with help from
 * http://www.ultimaserial.com/hack710.html
 *
 * Perhaps it will also work with the DI-718Bx 16-channel units or the DI-710 series?
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sysexits.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h> 
#include <errno.h> 

const int n_chans = 6;         // We'll poll the first N channels
const float fullscale = 20.0;  // "Engineering units" full-scale (depends on installed input amplifier module)

#define BUFSIZE 512
const uint16_t portno = 10001;
const int timerscaler = 2;
const int rate_divisor = 0;
const char scanlist[] = "E000E001E002E003E004E005E006E007";
const float fudge = 1.0;  // Converted values don't seem to quite agree with WinDAQ... Try 1.018 here??

int sockfd;
int signalled = 0;

static void trap(int signal) {
  signalled = 1;
}

static void error(char *msg) {
    perror(msg);
    exit(EX_UNAVAILABLE);
}

// Send an ASCII command to the device and check for correct echo response
static void do_cmd(const char *fmt, ...) {
  char cmd[256] = {0};
  char resp[256] = {0};
  va_list args;
  va_start (args, fmt);
  // Outgoing cmds are prefixed with a null byte
  size_t len = vsnprintf(&cmd[1], sizeof(cmd) - 1, fmt, args);
  va_end (args);
  ssize_t n = write(sockfd, cmd, 1 + len);
  if (n < 0) 
    error("ERROR writing to socket");
  if (n == 0) {
    fprintf(stderr, "EOF writing to socket\n");
    exit(EX_UNAVAILABLE);
  }

  // Echo responses don't have the leading null
  n = recv(sockfd, resp, len, MSG_WAITALL);
  if (n < 0) 
    error("ERROR reading from socket");
  if (n == 0) {
    fprintf(stderr, "EOF reading from socket\n");
    exit(EX_UNAVAILABLE);
  }
  if (n != len) {
    fprintf(stderr, "Expected %zd bytes, read %zd bytes\n", len, n);
    exit(EX_PROTOCOL);
  }

  if (memcmp(&cmd[1], resp, len)) {
    fprintf(stderr, "Expected '%s', received '%.*s'\n",
	    &cmd[1], (int)len, resp);
    exit(EX_PROTOCOL);
  }
  fprintf(stderr, "%s\n", &cmd[1]);
}

static const char* autodiscover(void) {
  fprintf(stderr, "Sorry, autodiscovery unimplemented.\n"
	  "If you're at Kitty Hawk, just specify 'di718b' as the hostname and let the DHCP\n"
	  "server do the work.  Otherwise, you can use the 'DATAQ Instruments Hardware Manager'\n"
	  "utility provided with WinDAQ, or check your DHCP logs for MAC addresses starting with\n"
	  "00:80:A3, or implement http://wiki.lantronix.com/developer/Lantronix_Discovery_Protocol\n");
  exit(EX_UNAVAILABLE);
}

int main(int argc, char **argv) {
  const char *hostname;
  uint16_t buf[BUFSIZE];

  if (argc != 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
    fprintf(stderr, "Simple client for DATAQ DI-718B-E(S) laboratory data acquisition system\n"
	    "Usage:\n"
	    "\t%s <HOST>\n"
	    "\t%s -a, --auto\n"
	    "where HOST is the hostname or IP address of the DAQ unit, or '-a' to autodiscover.\n",
	    argv[0], argv[0]);
    exit(EX_USAGE);
  }
  if (!strcmp(argv[1], "-a") || !strcmp(argv[1], "--auto"))
    hostname = autodiscover();
  else
    hostname = argv[1];
  
  // Create the socket
  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) 
    error("Error creating socket");

  // Socket is opened in blocking mode, but we'll set a timeout for read/recv
  const struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  // DNS lookup
  struct hostent *server;
  server = gethostbyname(hostname);
  if (server == NULL) {
    fprintf(stderr, "DNS lookup for %s failed, is it plugged in?\n", hostname);
    exit(EX_NOHOST);
  }
  struct sockaddr_in serveraddr = {
    .sin_family = AF_INET,
    .sin_port = htons(portno)};
  memcpy(&serveraddr.sin_addr.s_addr, server->h_addr, server->h_length);

  // Open TCP connection
  if (connect(sockfd, (const struct sockaddr *)&serveraddr,
	      sizeof(serveraddr)) < 0) {
    if (errno == EHOSTUNREACH)
      error("Error connecting, is it plugged in?");
    error("Error connecting, is someone else using it?");
  }

  // Tell DAQ to stop streaming, then flush the buffer (might contain unwanted spam)
  write(sockfd, "\0T0", 3);
  usleep(222222);
  while (recv(sockfd, buf, sizeof(buf), MSG_DONTWAIT) > 0);

  // Initialization sequence
  do_cmd("X%02X", timerscaler);  // Division from main 14400 Hz timer
  do_cmd("M%04X", rate_divisor); // Further division on output rate
  do_cmd("L00%s", scanlist);     // Which channels to scan, and options
  do_cmd("C%02X", n_chans);      // Scan first N channels in scanlist
  do_cmd("S3");                  // Start streaming

  signal(SIGINT, &trap);
  signal(SIGHUP, &trap);
  signal(SIGTERM, &trap);
  
  int n_rows = sizeof(buf) / (2 * n_chans);  // How many sets of samples per recv()
  while (!signalled) {
    int n = recv(sockfd, buf, n_rows * 2 * n_chans, MSG_WAITALL);
    if (signalled) break;  // recv() will terminate early if signal occurs
    if (n < 0)
      error("Error reading from socket");
    if (n == 0) {
      fprintf(stderr, "EOF reading from socket\n");
      exit(EX_UNAVAILABLE);
    }
    if (n != n_rows * 2 * n_chans) {
      fprintf(stderr, "Expected %d bytes, read %d bytes\n", n_rows * 2 * n_chans, n);
      exit(EX_PROTOCOL);
    }
    for (int i = 0; i < n_rows; i++) {
      for (int j = 0; j < n_chans; j++) {
	uint16_t v = buf[i * n_chans + j];
	// Check for expected sync flags in least significant bits
	uint16_t lsbs = v & 0x0101;
	if ((j == 0 && lsbs != 0x0100)
	    || (j != 0 && lsbs != 0x0101)) {
	  fprintf(stderr, "LSB mismatch @ %d, %d: %04X\n", i, j, v);
	  break;
	}
	// Extract 14-bit unsigned value
	uint16_t v14 = ((v & 0xFE00) >> 2) | ((v & 0x00FE) >> 1);
	// Scale to floating point in desired units
	float conv = fudge * fullscale * (((1.0 * v14) / (1<<13)) - 1);
	printf("%.3f ", conv);
      }
      printf("\n");
    }
  }
  
  // Shut down cleanly by stopping the stream and flushing
  write(sockfd, "\0T0", 3);
  usleep(222222);
  while (recv(sockfd, buf, sizeof(buf), MSG_DONTWAIT) > 0);
  close(sockfd);
  return 0;
}
