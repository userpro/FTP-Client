#ifndef LOG_H_
#define LOG_H_

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOGI(format, ...)                            \
    do {                                             \
        fprintf(stdout,                              \
                "[file: %s / func: %s / Line: %d] ", \
                __FILE__,                            \
                __func__,                            \
                __LINE__);                           \
        fprintf(stdout, format, ##__VA_ARGS__);      \
    } while (0)

#define LOGE(format, ...)                                       \
    do {                                                        \
        fprintf(stdout,                                         \
                "[file: %s / func: %s / Line: %d] <error %s> ", \
                __FILE__,                                       \
                __func__,                                       \
                __LINE__,                                       \
                strerror(errno));                               \
        fprintf(stdout, format, ##__VA_ARGS__);                 \
    } while (0)

#endif  // LOG_H_