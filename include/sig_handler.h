#ifndef SIGNAL_H
#define SIGNAL_H
#ifdef __cplusplus
extern "C" {
#endif
void signal_setup_handlers(void);
int  signal_is_shutdown(void);
void signal_request_shutdown(void);
#ifdef __cplusplus
}
#endif
#endif
