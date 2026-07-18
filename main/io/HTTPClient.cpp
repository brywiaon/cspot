#include "HTTPClient.h"

#include <string.h>   // for memcpy
#include <algorithm>  // for transform
#include <cassert>    // for assert
#include <cctype>     // for tolower
#include <ostream>    // for operator<<, basic_ostream

#include "BellSocket.h"  // for bell
#include "BellUtils.h"

using namespace bell;

void HTTPClient::Response::connect(const std::string& url) {
  urlParser = bell::URLParser::parse(url);

  // Open socket of type
  this->socketStream.open(urlParser.host, urlParser.port,
                          urlParser.schema == "https");
  if (!this->socketStream.isOpen()) {
    this->statusCode = 0;
  }
}

HTTPClient::Response::~Response() {
  if (this->socketStream.isOpen()) {
    this->socketStream.close();
  }
}

void HTTPClient::Response::abortNow() {
  this->socketStream.abortNow();
  this->statusCode = 0;
}

void HTTPClient::Response::rawRequest(const std::string& url,
                                      const std::string& method,
                                      const std::vector<uint8_t>& content,
                                      Headers& headers) {
  urlParser = bell::URLParser::parse(url);
  if (!socketStream.isOpen() || socketStream.fail()) {
    statusCode = 0;
    return;
  }

  // Prepare a request
  const char* reqEnd = "\r\n";

  socketStream << method << " " << urlParser.path << " HTTP/1.1" << reqEnd;
  socketStream << "Host: " << urlParser.host << ":" << urlParser.port << reqEnd;
  socketStream << "Connection: close" << reqEnd;
  socketStream << "Accept: */*" << reqEnd;

  // Write content
  if (content.size() > 0) {
    socketStream << "Content-Length: " << content.size() << reqEnd;
  }

  // Write headers
  for (auto& header : headers) {
    socketStream << header.first << ": " << header.second << reqEnd;
  }

  socketStream << reqEnd;

  // Write request body
  if (content.size() > 0) {
    socketStream.write((const char*)content.data(), content.size());
  }

  socketStream.flush();
  if (!socketStream.good()) {
    statusCode = 0;
    return;
  }

  // Parse response
  readResponseHeaders();
}

void HTTPClient::Response::readResponseHeaders() {
  char *method, *path;
  const char* msgPointer;

  size_t msgLen;
  int pret, minorVersion, status;

  size_t prevbuflen = 0, numHeaders;
  this->httpBufferAvailable = 0;

  while (1) {
    BELL_SLEEP_MS(1);
    socketStream.getline((char*)httpBuffer.data() + httpBufferAvailable,
                         httpBuffer.size() - httpBufferAvailable);

    prevbuflen = httpBufferAvailable;
    httpBufferAvailable += socketStream.gcount();
    if (socketStream.gcount() <= 0 || httpBufferAvailable < 2) {
      statusCode = 0;
      return;
    }

    // Restore delimiters
    memcpy(httpBuffer.data() + httpBufferAvailable - 2, "\r\n", 2);

    // Parse the request
    numHeaders = sizeof(phResponseHeaders) / sizeof(phResponseHeaders[0]);

    pret =
        phr_parse_response((const char*)httpBuffer.data(), httpBufferAvailable,
                           &minorVersion, &status, &msgPointer, &msgLen,
                           phResponseHeaders, &numHeaders, prevbuflen);

    if (pret > 0) {
      break; /* successfully parsed the request */
    } else if (pret == -1) {
      statusCode = 0;
      return;
    }

    /* request is incomplete, continue the loop */
    assert(pret == -2);
    if (httpBufferAvailable == httpBuffer.size()) {
      statusCode = 0;
      return;
    }
  }

  this->responseHeaders = {};
  this->statusCode = status;

  // Headers have benen read
  for (int headerIndex = 0; headerIndex < numHeaders; headerIndex++) {
    this->responseHeaders.push_back(
        ValueHeader{std::string(phResponseHeaders[headerIndex].name,
                                phResponseHeaders[headerIndex].name_len),
                    std::string(phResponseHeaders[headerIndex].value,
                                phResponseHeaders[headerIndex].value_len)});
  }

  std::string contentLengthValue = std::string(header("content-length"));
  if (contentLengthValue.size() > 0) {
    this->hasContentSize = true;
    this->contentSize = std::stoi(contentLengthValue);
  }
}

void HTTPClient::Response::get(const std::string& url, Headers headers) {
  std::string method = "GET";
  return this->rawRequest(url, method, {}, headers);
}

void HTTPClient::Response::post(const std::string& url, Headers headers,
                                const std::vector<uint8_t>& body) {
  std::string method = "POST";
  return this->rawRequest(url, method, body, headers);
}

size_t HTTPClient::Response::contentLength() {
  return contentSize;
}

std::string_view HTTPClient::Response::header(const std::string& headerName) {
  for (auto& header : this->responseHeaders) {
    std::string headerValue = header.first;
    std::transform(headerValue.begin(), headerValue.end(), headerValue.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (headerName == headerValue) {
      return header.second;
    }
  }

  return "";
}

size_t HTTPClient::Response::totalLength() {
  auto rangeHeader = header("content-range");

  if (rangeHeader.find("/") != std::string::npos) {
    return std::stoi(
        std::string(rangeHeader.substr(rangeHeader.find("/") + 1)));
  }

  return this->contentLength();
}

void HTTPClient::Response::readRawBody() {
  if (rawBody.size() != 0) {
    return;
  }

  if (contentSize > 0) {
    rawBody.resize(contentSize);
    size_t bytesReadTotal = 0;
    while (bytesReadTotal < contentSize) {
      auto chunkSize = std::min<size_t>(1024, contentSize - bytesReadTotal);
      socketStream.read((char*)rawBody.data() + bytesReadTotal, chunkSize);
      auto bytesRead = socketStream.gcount();
      if (bytesRead <= 0) break;
      bytesReadTotal += bytesRead;
      BELL_SLEEP_MS(1);
    }
    rawBody.resize(bytesReadTotal);
    return;
  }

  std::vector<uint8_t> bufferedBody;
  char buffer[1024];
  while (socketStream.good()) {
    socketStream.read(buffer, sizeof(buffer));
    auto bytesRead = socketStream.gcount();
    if (bytesRead <= 0) break;
    bufferedBody.insert(bufferedBody.end(), buffer, buffer + bytesRead);
    BELL_SLEEP_MS(1);
  }

  auto transferEncoding = header("transfer-encoding");
  if (transferEncoding.find("chunked") == std::string_view::npos) {
    rawBody.clear();
    rawBody.insert(rawBody.end(), bufferedBody.begin(), bufferedBody.end());
    return;
  }

  size_t position = 0;
  while (position < bufferedBody.size()) {
    size_t lineEnd = position;
    while (lineEnd + 1 < bufferedBody.size() &&
           !(bufferedBody[lineEnd] == '\r' && bufferedBody[lineEnd + 1] == '\n')) {
      lineEnd++;
    }
    if (lineEnd + 1 >= bufferedBody.size()) break;

    size_t chunkSize = 0;
    bool validChunkSize = true;
    for (size_t i = position; i < lineEnd; i++) {
      char c = (char)bufferedBody[i];
      if (c == ';') break;
      uint8_t nibble = 0;
      if (c >= '0' && c <= '9') {
        nibble = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        nibble = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        nibble = c - 'A' + 10;
      } else {
        validChunkSize = false;
        break;
      }
      chunkSize = (chunkSize << 4) | nibble;
    }
    if (!validChunkSize) break;

    position = lineEnd + 2;
    if (chunkSize == 0 || position + chunkSize > bufferedBody.size()) break;
    rawBody.insert(rawBody.end(), bufferedBody.begin() + position,
                   bufferedBody.begin() + position + chunkSize);
    position += chunkSize + 2;
  }
}

std::string_view HTTPClient::Response::body() {
  readRawBody();
  return std::string_view((char*)rawBody.data(), rawBody.size());
}

std::vector<uint8_t> HTTPClient::Response::bytes() {
  readRawBody();
  return std::vector<uint8_t>(rawBody.begin(), rawBody.end());
}
