#ifndef SIG_H
#define SIG_H

#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

void sig_setup(void);
int  sig_shutdown(void);
void sig_request_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* SIG_H */
