/*
 * ==========================================================================
 * config.h — 配置加载模块头文件
 * ==========================================================================
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 加载 YAML 配置文件并填充 app_cfg_t 结构体
 * @yaml_path：配置文件路径（如 "config/default.yaml"）
 * @cfg：输出参数，填充后的配置
 * 返回 0 成功
 *
 * 文件不存在时不会报错，而是使用默认值降级运行
 */
int config_load(const char* yaml_path, app_cfg_t* cfg);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
