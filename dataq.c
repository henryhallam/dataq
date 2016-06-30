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
 *
 * NOTES
 *  - Functions use negative integers for failure, and nonnegatives for success
 *  - Many functions use return codes from sysexits.h * -1
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sysexits.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>

// Include forward declarations so compiler detects if out-of-sync
#include "dataq.h"

// Error message printing
#ifdef EPRINT
  #define eprintf(...) fprintf(stderr, __VA_ARGS__)
#else
  #define eprintf(...) ((void)0)
#endif

// Debug message printing
#ifdef DPRINT
  #define dprintf(...) fprintf(stderr, __VA_ARGS__)
#else
  #define dprintf(...) ((void)0)
#endif

// Signal handling
typedef void (*sighandler_t)(int);  // Defn stolen from signal.h
int signalled = 0;
static void trap(int sig)
{
  signalled = sig;
}

/*
 *  DATAQ interface
 */

#define BUFSIZE 512   // Size of receive buffer
#define MAXCHAN 32    // Maximum number of channels

// Send an ASCII command to the device and check for correct echo response
int dataq_cmd(int sockfd, const char *fmt, ...)
{
  char cmd[256] = { 0 };   // Outgoing cmds are prefixed with a null byte
  char resp[256] = { 0 };  // Default response value == ""

  va_list args;
  va_start(args, fmt);
  size_t len = vsnprintf(&cmd[1], sizeof(cmd) - 1, fmt, args);
  va_end(args);

  ssize_t n = write(sockfd, cmd, 1 + len);
  if (n < 0) {
    eprintf("Error writing to socket\n");
    return -EX_IOERR;
  }
  if (n == 0) {
    eprintf("EOF writing to socket\n");
    return -EX_UNAVAILABLE;
  }

  // Echo responses don't have the leading null
  n = recv(sockfd, resp, len, MSG_WAITALL);
  if (n < 0) {
    eprintf("Error reading from socket\n");
    return -EX_IOERR;
  }
  if (n == 0) {
    eprintf("EOF reading from socket\n");
    return -EX_UNAVAILABLE;
  }
  if ((int32_t)n != (int32_t)len) {
    eprintf("Expected %zd bytes, read %zd bytes\n", len, n);
    return -EX_PROTOCOL;
  }
  if (memcmp(&cmd[1], resp, len)) {
    eprintf("Expected '%s', received '%.*s'\n", &cmd[1], (int) len, resp);
    return -EX_PROTOCOL;
  }

  // Echo command
  dprintf("CMD: %s\n", &cmd[1]);

  return EX_OK;
}

// Handy macro for returning on error
#define do_cmd(...) do { \
                         int res = dataq_cmd(__VA_ARGS__); \
                         if (res != EX_OK) return res; \
                       } while (0)

// Connect to and initialize a DATAQ device, and start streaming
// On success, returns socket fd
int dataq_connect(const char *hostname, const uint16_t portno,
                  const int timerscaler, const int rate_divisor,
                  const char *scanlist, const int n_chans)
{
  // Sanity check parameters
  if (n_chans > MAXCHAN) {
    eprintf("Requested %d channels exceeds maximum %d channels\n", n_chans, MAXCHAN);
    return -EX_DATAERR;
  }

  // Create the socket
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    eprintf("Error creating socket\n");
    return -EX_UNAVAILABLE;
  }

  // Socket is opened in blocking mode, but we'll set a timeout for read/recv
  const struct timeval tv = {.tv_sec = 1,.tv_usec = 0 };
  if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1) {
    eprintf("Error setting socket timeout\n");
    return -EX_UNAVAILABLE;
  }

  // DNS lookup
  struct hostent *server;
  server = gethostbyname(hostname);
  if (server == NULL) {
    eprintf("DNS lookup for %s failed, is it plugged in?\n", hostname);
    return -EX_NOHOST;
  }
  struct sockaddr_in serveraddr;
  memset(&serveraddr, 0, sizeof(serveraddr));
  serveraddr.sin_family = AF_INET;
  serveraddr.sin_port = htons(portno);
  memcpy(&serveraddr.sin_addr.s_addr, server->h_addr, server->h_length);

  // Open TCP connection
  if (connect(sockfd, (const struct sockaddr *) &serveraddr,
              sizeof(serveraddr)) < 0) {
    if (errno == EHOSTUNREACH)
      eprintf("Error connecting, is it plugged in?\n");
    else
      eprintf("Error connecting, is someone else using it?\n");
    return -EX_UNAVAILABLE;
  }

  // Tell DAQ to stop streaming, then flush the buffer (might contain unwanted spam)
  if (write(sockfd, "\0T0", 3) == -1) {
    eprintf("Error stopping socket stream\n");
    return -EX_IOERR;
  }
  usleep(222222);
  uint16_t buf[16];
  while (recv(sockfd, buf, sizeof(buf), MSG_DONTWAIT) > 0);

  // Initialization sequence
  do_cmd(sockfd, "X%02X", timerscaler);   // Division from main 14400 Hz timer
  do_cmd(sockfd, "M%04X", rate_divisor);  // Further division on output rate
  do_cmd(sockfd, "L00%s", scanlist);      // Which channels to scan, and options
  do_cmd(sockfd, "C%02X", n_chans);       // Scan first N channels in scanlist
  do_cmd(sockfd, "S3");                   // Start streaming

  return sockfd;
}

// Stop streaming and disconnect from a DATAQ device
void dataq_close(int sockfd)
{
  // Close cleanly by stopping the stream and flushing
  if (write(sockfd, "\0T0", 3) == -1) {
    // Ignore errors on close
  }
  usleep(222222);
  uint16_t buf[16];
  while (recv(sockfd, buf, sizeof(buf), MSG_DONTWAIT) > 0);
  close(sockfd);
}

// Receive and parse data from a DATAQ device
// Assumes values[] is of length == n_chans
// NOTE if tv != NULL, will populate from gettimeofday()
int dataq_recv(int sockfd, float values[], const int n_chans,
               const float fullscale, const float fudge, struct timeval *tv)
{
  static uint16_t buf[BUFSIZE];

  int n_bytes = 2 * n_chans;  // How many bytes per recv()

  // Receive some data
  // NOTE catching common signals so we can abort cleanly
  sighandler_t sigint = signal(SIGINT, &trap);
  sighandler_t sighup = signal(SIGHUP, &trap);
  sighandler_t sigterm = signal(SIGTERM, &trap);
  int n = recv(sockfd, buf, n_bytes, MSG_WAITALL);
  signal(SIGINT, sigint);
  signal(SIGHUP, sighup);
  signal(SIGTERM, sigterm);

  // If we caught the signal, let the original handler run and then abort
  if (signalled) {
    raise(signalled);
    eprintf("Caught %s signal during receive\n", strsignal(signalled));
    return -EX_UNAVAILABLE;
  }

  if (tv != NULL)
    gettimeofday(tv, NULL);

  if (n < 0) {
    eprintf("Error reading from socket\n");
    return -EX_IOERR;
  }
  if (n == 0) {
    eprintf("EOF reading from socket\n");
    return -EX_UNAVAILABLE;
  }
  if (n != n_bytes) {
    eprintf("Expected %d bytes, read %d bytes\n", n_bytes, n);
    return -EX_PROTOCOL;
  }

  uint8_t c;
  for (c = 0; c < n_chans; c++) {
    uint16_t v = buf[c];

    // Check for expected sync flags in least significant bits
    uint16_t lsbs = v & 0x0101;
    if ((c == 0 && lsbs != 0x0100)
        || (c != 0 && lsbs != 0x0101)) {
      eprintf("LSB mismatch @ %d: %04X\n", c, v);
      return -EX_PROTOCOL;
    }

    // Extract 14-bit unsigned value
    uint16_t v14 = ((v & 0xFE00) >> 2) | ((v & 0x00FE) >> 1);

    // Scale to floating point in desired units
    float conv = fudge * fullscale * (((1.0 * v14) / (1 << 13)) - 1);

    values[c] = conv;
  }

  return EX_OK;
}

// Discover a DATAQ device
const char *dataq_autodiscover(void)
{
  fprintf(stderr, "Sorry, autodiscovery unimplemented.\n"
          "If you're at Kitty Hawk, just specify 'di718b' as the hostname and let the DHCP\n"
          "server do the work.  Otherwise, you can use the 'DATAQ Instruments Hardware Manager'\n"
          "utility provided with WinDAQ, or check your DHCP logs for MAC addresses starting with\n"
          "00:80:A3, or implement http://wiki.lantronix.com/developer/Lantronix_Discovery_Protocol\n");
  return NULL;
}

/*
 *  Main program (included optionally)
 */

#ifdef USE_MAIN

const int n_chans = 6;          // We'll poll the first N channels
const float fullscale = 20.0;   // "Engineering units" full-scale (depends on installed input amplifier module)
const uint16_t portno = 10001;
const int timerscaler = 2;
const int rate_divisor = 0;
const char scanlist[] = "E000E001E002E003E004E005E006E007";
const float fudge = 1.0;        // Converted values don't seem to quite agree with WinDAQ... Try 1.018 here??

int main(int argc, char **argv)
{
  if (argc != 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
    fprintf(stderr,
            "Simple client for DATAQ DI-718B-E(S) laboratory data acquisition system\n"
            "Usage:\n"
            "    %s <HOST>\n"
            "    %s -a, --auto\n"
            "where HOST is the hostname or IP address of the DAQ unit, or '-a' to autodiscover.\n",
            argv[0], argv[0]);
    exit(EX_USAGE);
  }

  const char *hostname;
  if (!strcmp(argv[1], "-a") || !strcmp(argv[1], "--auto")) {
    hostname = dataq_autodiscover();
    if (hostname == NULL)
      exit(EX_UNAVAILABLE);
  }
  else
    hostname = argv[1];

  int sockfd;
  if ((sockfd = dataq_connect(hostname, portno, timerscaler,
                              rate_divisor, scanlist, n_chans)) < 0)
    exit(-sockfd);

  signal(SIGINT, &trap);
  signal(SIGHUP, &trap);
  signal(SIGTERM, &trap);

  while (!signalled) {
    float values[MAXCHAN];
    struct timeval tv;

    int ret = dataq_recv(sockfd, values, n_chans, fullscale, fudge, &tv);
    if (signalled)
      break;
    if (ret < 0)
      continue;

    printf("%llu.%06llu", (unsigned long long int)tv.tv_sec,
                          (unsigned long long int)tv.tv_usec);

    uint8_t c;
    for (c = 0; c < n_chans; c++)
      printf(" %.3f", values[c]);
    printf("\n");
  }

  dataq_close(sockfd);
  return 0;
}

#endif // USE_MAIN
