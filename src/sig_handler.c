#include "sig_handler.h"
#include <signal.h>
static volatile int g_shutdown = 0;
static void handler(int sig) { (void)sig; g_shutdown = 1; }
void signal_setup_handlers(void) { signal(SIGINT,handler); signal(SIGTERM,handler); }
int  signal_is_shutdown(void)   { return g_shutdown; }
void signal_request_shutdown(void) { g_shutdown = 1; }
