#ifndef MONITOR_H
#define MONITOR_H

#ifdef __cplusplus
extern "C" {
#endif

void monitor_start(void);
void monitor_stop(void);
float monitor_cpu(void);
float monitor_mem(void);
float monitor_temp(void);

#ifdef __cplusplus
}
#endif

#endif /* MONITOR_H */
