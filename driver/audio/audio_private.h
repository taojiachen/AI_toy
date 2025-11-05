#ifndef __AUDIO_PRIVATE_H__
#define __AUDIO_PRIVATE_H__

#if CONFIG_EASYLOGGER_SUPPORT
#include "elog.h"
#define LOG_ERR log_e
#define LOG_INF log_i
#define LOG_WRN log_w
#define LOG_DBG log_d
#else
#define AUDIO_NORMAL_LOG_OUTPUT do {\
    os_printf("%s\r\n", __func__);\
    } while (0)
#define LOG_ERR(...) AUDIO_NORMAL_LOG_OUTPUT
#define LOG_INF(...) AUDIO_NORMAL_LOG_OUTPUT
#define LOG_WRN(...) AUDIO_NORMAL_LOG_OUTPUT
#define LOG_DBG(...) AUDIO_NORMAL_LOG_OUTPUT
#endif /* CONFIG_EASYLOGGER_SUPPORT */
#endif /* __AUDIO_PRIVATE_H__ */