#pragma once

#include <cstdlib>    // for strtol, size_t
#include <stdexcept>  // for invalid_argument
#include <string>     // for string, allocator, operator+, char_traits, oper...

namespace bell {
class URLParser {
 public:
  static std::string urlEncode(const std::string& value) {
    std::string new_str = "";
    static auto hex_digt = "0123456789ABCDEF";

    std::string result;
    result.reserve(value.size() << 1);

    for (auto ch : value) {
      if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= 'a' && ch <= 'z') || ch == '-' || ch == '_' || ch == '!' ||
          ch == '\'' || ch == '(' || ch == ')' || ch == '*' || ch == '~' ||
          ch == '.')  // !'()*-._~
      {
        result.push_back(ch);
      } else {
        result += std::string("%") +
                  hex_digt[static_cast<unsigned char>(ch) >> 4] +
                  hex_digt[static_cast<unsigned char>(ch) & 15];
      }
    }

    return result;
  }
  static std::string urlDecode(const std::string& value) {
    std::string result;
    result.reserve(value.size());

    for (std::size_t i = 0; i < value.size(); ++i) {
      auto ch = value[i];

      if (ch == '%' && (i + 2) < value.size()) {
        auto hex = value.substr(i + 1, 2);
        auto dec = static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
        result.push_back(dec);
        i += 2;
      } else if (ch == '+') {
        result.push_back(' ');
      } else {
        result.push_back(ch);
      }
    }

    return result;
  }

  std::string host;
  int port = -1;

  std::string schema = "http";
  std::string path;

  static URLParser parse(const std::string& url) {
    URLParser parser;

    const auto schemePos = url.find("://");
    size_t authorityStart = 0;
    if (schemePos != std::string::npos) {
      parser.schema = url.substr(0, schemePos);
      authorityStart = schemePos + 3;
    }

    const size_t authorityEnd = url.find_first_of("/?#", authorityStart);
    const std::string authority = url.substr(
        authorityStart,
        authorityEnd == std::string::npos ? std::string::npos
                                          : authorityEnd - authorityStart);
    if (authority.empty()) {
      throw std::invalid_argument("Invalid URL");
    }

    const size_t pathStart =
        authorityEnd == std::string::npos ? url.size() : authorityEnd;
    const size_t fragmentStart = url.find('#', pathStart);
    parser.path = url.substr(
        pathStart, fragmentStart == std::string::npos
                       ? std::string::npos
                       : fragmentStart - pathStart);

    if (!authority.empty() && authority.front() == '[') {
      const size_t closing = authority.find(']');
      if (closing == std::string::npos) {
        throw std::invalid_argument("Invalid URL");
      }
      parser.host = authority.substr(1, closing - 1);
      if (closing + 1 < authority.size()) {
        if (authority[closing + 1] != ':') {
          throw std::invalid_argument("Invalid URL");
        }
        parser.port = std::stoi(authority.substr(closing + 2));
      }
    } else {
      const size_t colon = authority.rfind(':');
      if (colon != std::string::npos &&
          authority.find(':') == colon) {
        parser.host = authority.substr(0, colon);
        parser.port = std::stoi(authority.substr(colon + 1));
      } else {
        parser.host = authority;
      }
    }

    if (parser.host.empty()) {
      throw std::invalid_argument("Invalid URL");
    }

    if (parser.port == -1) {
      parser.port = parser.schema == "http" ? 80 : 443;
    }

    return parser;
  }
};
}  // namespace bell
