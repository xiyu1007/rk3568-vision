#ifndef FPS_H
#define FPS_H
#ifdef __cplusplus
extern "C" {
#endif
void   fps_tick(void);
double fps_get(void);
void   fps_reset(void);
#ifdef __cplusplus
}
#endif
#endif
