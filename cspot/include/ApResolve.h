#pragma once

#include <string>  // for string
#include <vector>  // for vector
#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#endif

namespace cspot {
class ApResolve {
 public:
  ApResolve(std::string apOverride);

  /**
   * @brief Connects to spotify's servers and returns first valid ap address
   * @returns std::string Address in form of url:port
   */
  std::string fetchFirstApAddress();

  /**
   * @brief Fetches every Spotify access point offered by the resolver.
   * @returns Addresses in host:port form, ordered by Spotify preference.
   */
  std::vector<std::string> fetchApAddresses();

 private:
  std::string apOverride;
};
}  // namespace cspot
