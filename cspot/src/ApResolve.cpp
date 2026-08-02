#include "ApResolve.h"

#include <initializer_list>  // for initializer_list
#include <map>               // for operator!=, operator==
#include <memory>            // for allocator, unique_ptr
#include <string_view>       // for string_view
#include <vector>            // for vector

#include "HTTPClient.h"  // for HTTPClient, HTTPClient::Response
#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif

using namespace cspot;

ApResolve::ApResolve(std::string apOverride) {
  this->apOverride = apOverride;
}

std::string ApResolve::fetchFirstApAddress() {
  if (apOverride != "") {
    return apOverride;
  }

  auto request = bell::HTTPClient::get("https://apresolve.spotify.com/");
  if (!request || request->status() < 200 || request->status() >= 300) {
    return {};
  }
  std::string_view responseStr = request->body();
  if (responseStr.empty()) return {};

  // parse json with nlohmann
#ifdef BELL_ONLY_CJSON
  cJSON* json = cJSON_ParseWithLength(responseStr.data(), responseStr.size());
  cJSON* list = json ? cJSON_GetObjectItem(json, "ap_list") : nullptr;
  cJSON* first = list ? cJSON_GetArrayItem(list, 0) : nullptr;
  if (!cJSON_IsString(first) || !first->valuestring) {
    cJSON_Delete(json);
    return {};
  }
  auto ap_string = std::string(first->valuestring);
  cJSON_Delete(json);
  return ap_string;
#else
  auto json = nlohmann::json::parse(responseStr, nullptr, false);
  if (json.is_discarded() || !json.contains("ap_list") ||
      !json["ap_list"].is_array() || json["ap_list"].empty() ||
      !json["ap_list"][0].is_string()) {
    return {};
  }
  return json["ap_list"][0].get<std::string>();
#endif
}
