#ifndef __DATAQ_H__
#define __DATAQ_H__

// Recommend allocating something like uint16_t buffer[DATAQ_BUFSIZE] for receives
#define DATAQ_BUFSIZE 512

int dataq_cmd(int sockfd, const char *fmt, ...);

int dataq_connect(const char *hostname, const uint16_t portno,
                  const uint32_t timerscaler, const uint32_t rate_divisor,
                  const char *scanlist, const uint32_t n_chans);

void dataq_close(int sockfd);

int dataq_recv(int sockfd, uint16_t buf[], const uint32_t bufsize,
               const uint32_t n_chans, struct timeval *tv);

int dataq_parserow(float values[], const uint16_t buf[], const uint32_t n_chans,
                   uint32_t row, const float fullscale, const float fudge);

const char *dataq_autodiscover(void);

#endif // __DATAQ_H__
