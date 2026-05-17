#ifndef CONFIG_H
#define CONFIG_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

int config_load(const char* yaml_path, app_cfg_t* cfg);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
