#pragma once

#include <cstdio>
#include <cstdint>
#include <vector>

#include "pb_decode.h"

template <typename T>
bool pbDecodeChecked(T& result, const pb_msgdesc_t* fields,
                     const std::vector<uint8_t>& data) {
  pb_istream_t stream =
      pb_istream_from_buffer(data.empty() ? nullptr : data.data(), data.size());
  if (!pb_decode(&stream, fields, &result)) {
    std::printf("Decode failed: %s\n", PB_GET_ERROR(&stream));
    return false;
  }
  return true;
}
