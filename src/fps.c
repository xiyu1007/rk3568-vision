#include "fps.h"
#include <time.h>
static double g_last=0; static long long g_cnt=0; static double g_fps=0; static int g_init=0;
void fps_tick(void){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);double n=ts.tv_sec+ts.tv_nsec*1e-9;if(!g_init){g_last=n;g_cnt=0;g_init=1;return;}g_cnt++;double e=n-g_last;if(e>=0.5){g_fps=g_cnt/e;g_last=n;g_cnt=0;}}
double fps_get(void){return g_fps;}
void fps_reset(void){g_last=0;g_cnt=0;g_fps=0;g_init=0;}
