// sys_lib.c
#include <sys/stat.h>
#include <errno.h>
#include <string.h>

/**
 * 递归创建目录
 * @param path 目录路径
 * @return 0:成功, -1:失败
 */
int mkdir_p(const char *path) {
    if (!path || !*path) return -1;
    
    char buf[512];
    char *p;
    
    // 复制路径
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    
    // 逐级创建
    for (p = buf + (buf[0] == '/'); *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    
    // 创建最后一级
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    
    return 0;
}

/**
 * 创建文件所在目录
 * @param filepath 文件路径
 * @return 0:成功, -1:失败
 */
int mkdir_for_file(const char *filepath) {
    if (!filepath || !*filepath) return -1;
    
    char buf[512];
    char *p;
    
    // 复制路径
    strncpy(buf, filepath, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    
    // 找到最后一个 '/' 并截断
    p = strrchr(buf, '/');
    if (!p) return 0;  // 没有路径，只在当前目录
    
    *p = '\0';  // 去掉文件名，只保留目录
    
    // 递归创建目录
    return mkdir_p(buf);
}