#pragma once

#include <spdlog/spdlog.h>

// =================================================================================================

#define logwi(ec, ...)                                                                             \
   do                                                                                              \
   {                                                                                               \
      if (ec && spdlog::default_logger_raw()->should_log(spdlog::level::warn))                     \
         spdlog::default_logger_raw()->warn(__VA_ARGS__);                                          \
      else if (spdlog::default_logger_raw()->should_log(spdlog::level::info))                      \
         spdlog::default_logger_raw()->info(__VA_ARGS__);                                          \
   } while (false)

#define loge(...)                                                                                  \
   do                                                                                              \
   {                                                                                               \
      if (spdlog::default_logger_raw()->should_log(spdlog::level::err))                            \
         spdlog::default_logger_raw()->error(__VA_ARGS__);                                         \
   } while (false)

#define logw(...)                                                                                  \
   do                                                                                              \
   {                                                                                               \
      if (spdlog::default_logger_raw()->should_log(spdlog::level::warn))                           \
         spdlog::default_logger_raw()->warn(__VA_ARGS__);                                          \
   } while (false)

#define logi(...)                                                                                  \
   do                                                                                              \
   {                                                                                               \
      if (spdlog::default_logger_raw()->should_log(spdlog::level::info))                           \
         spdlog::default_logger_raw()->info(__VA_ARGS__);                                          \
   } while (false)

#define logd(...)                                                                                  \
   do                                                                                              \
   {                                                                                               \
      if (spdlog::default_logger_raw()->should_log(spdlog::level::debug))                          \
         spdlog::default_logger_raw()->debug(__VA_ARGS__);                                         \
   } while (false)

#define mloge(x, ...) loge("[{}] " x, logPrefix() __VA_OPT__(, ) __VA_ARGS__)
#define mlogd(x, ...) logd("[{}] " x, logPrefix() __VA_OPT__(, ) __VA_ARGS__)
#define mlogi(x, ...) logi("[{}] " x, logPrefix() __VA_OPT__(, ) __VA_ARGS__)
#define mlogw(x, ...) logw("[{}] " x, logPrefix() __VA_OPT__(, ) __VA_ARGS__)

// =================================================================================================
