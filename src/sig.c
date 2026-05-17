#include "sig.h"
#include <signal.h>
#include <string.h>

static volatile sig_atomic_t g_shutdown = 0;

static void sig_handler(int sig) {
    (void)sig;
    g_shutdown = 1;
}

void sig_setup(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sa.sa_flags   = 0;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* ignore SIGPIPE (prevent crash on broken RTMP connection) */
    signal(SIGPIPE, SIG_IGN);
}

int sig_shutdown(void) {
    return g_shutdown;
}

void sig_request_shutdown(void) {
    g_shutdown = 1;
}
