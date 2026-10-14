// SPDX-License-Identifier: MIT
#pragma once

#include <cstdarg>

namespace oa2dp {

enum class LogLevel { Debug, Info, Warn, Error };

void log_init(bool console);
void log_write(LogLevel level, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#define LOGD(...) ::oa2dp::log_write(::oa2dp::LogLevel::Debug, __VA_ARGS__)
#define LOGI(...) ::oa2dp::log_write(::oa2dp::LogLevel::Info, __VA_ARGS__)
#define LOGW(...) ::oa2dp::log_write(::oa2dp::LogLevel::Warn, __VA_ARGS__)
#define LOGE(...) ::oa2dp::log_write(::oa2dp::LogLevel::Error, __VA_ARGS__)

}  // namespace oa2dp
