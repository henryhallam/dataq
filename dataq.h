#ifndef __DATAQ_H__
#define __DATAQ_H__

int dataq_cmd(int sockfd, const char *fmt, ...);

int dataq_connect(const char *hostname, const uint16_t portno,
                  const int timerscaler, const int rate_divisor,
                  const char *scanlist, const int n_chans);

void dataq_close(int sockfd);

int dataq_recv(int sockfd, float values[], const int n_chans,
               const float fullscale, const float fudge, struct timeval *tv);

const char *dataq_autodiscover(void);

#endif // __DATAQ_H__
