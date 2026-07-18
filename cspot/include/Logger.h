#pragma once

#include <BellLogger.h>

#define CSPOT_LOG_error(...)                                                \
  do {                                                                      \
    if (bell::bellGlobalLogger) {                                           \
      bell::bellGlobalLogger->error(__FILE__, __LINE__, "cspot",            \
                                    __VA_ARGS__);                           \
    }                                                                       \
  } while (0)

#define CSPOT_LOG_info(...)                                                 \
  do {                                                                      \
  } while (0)

#define CSPOT_LOG_debug(...)                                                \
  do {                                                                      \
  } while (0)

#define CSPOT_LOG(type, ...) CSPOT_LOG_##type(__VA_ARGS__)
