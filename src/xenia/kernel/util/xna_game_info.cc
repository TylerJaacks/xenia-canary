/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/xna_game_info.h"

#include <cstring>

#include "xenia/base/byte_order.h"
#include "xenia/base/string.h"

namespace xe {
namespace kernel {
namespace util {

namespace {

constexpr uint32_t MakeTag(char a, char b, char c, char d) {
  return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) |
         (uint32_t(uint8_t(c)) << 8) | uint32_t(uint8_t(d));
}

constexpr uint32_t kTagExec = MakeTag('E', 'X', 'E', 'C');
constexpr uint32_t kTagComm = MakeTag('C', 'O', 'M', 'M');
constexpr uint32_t kTagTitl = MakeTag('T', 'I', 'T', 'L');

constexpr size_t kExecVirtualTitleIdLength = 33;
constexpr size_t kExecEntryAssemblyLength = 43;
constexpr size_t kExecRuntimeProfileLength = 65;
constexpr size_t kTitlTitleChars = 128;
constexpr size_t kTitlDescriptionChars = 512;

uint32_t LoadBE32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

std::string ReadFixedAnsi(const uint8_t* p, size_t max_length) {
  size_t length = 0;
  while (length < max_length && p[length]) {
    ++length;
  }
  return std::string(reinterpret_cast<const char*>(p), length);
}

std::string ReadFixedUtf16BE(const uint8_t* p, size_t max_chars) {
  std::u16string value;
  for (size_t i = 0; i < max_chars; ++i) {
    char16_t c = char16_t((uint16_t(p[i * 2]) << 8) | p[i * 2 + 1]);
    if (!c) {
      break;
    }
    value.push_back(c);
  }
  return xe::to_utf8(value);
}

}  // namespace

std::string XnaGameInfo::framework_version() const {
  // "Xbox 360.v4.0.HiDef"
  const size_t v = runtime_profile.find(".v");
  if (v == std::string::npos) {
    return {};
  }
  const size_t start = v + 2;
  const size_t end = runtime_profile.rfind('.');
  if (end == std::string::npos || end <= start) {
    return runtime_profile.substr(start);
  }
  return runtime_profile.substr(start, end - start);
}

std::string XnaGameInfo::graphics_profile() const {
  const size_t dot = runtime_profile.rfind('.');
  if (dot == std::string::npos) {
    return {};
  }
  return runtime_profile.substr(dot + 1);
}

std::optional<XnaGameInfo> ParseXnaGameInfo(std::span<const uint8_t> data) {
  XnaGameInfo info;
  bool has_exec = false;

  size_t offset = 0;
  while (offset + 8 <= data.size()) {
    const uint32_t tag = LoadBE32(data.data() + offset);
    const uint32_t size = LoadBE32(data.data() + offset + 4);
    offset += 8;
    if (size > data.size() - offset) {
      break;  // Truncated chunk.
    }
    const uint8_t* body = data.data() + offset;

    if (tag == kTagExec && size >= kExecVirtualTitleIdLength +
                                       kExecEntryAssemblyLength +
                                       kExecRuntimeProfileLength) {
      info.virtual_title_id = ReadFixedAnsi(body, kExecVirtualTitleIdLength);
      body += kExecVirtualTitleIdLength;
      info.entry_assembly = ReadFixedAnsi(body, kExecEntryAssemblyLength);
      body += kExecEntryAssemblyLength;
      info.runtime_profile = ReadFixedAnsi(body, kExecRuntimeProfileLength);
      has_exec = true;
    } else if (tag == kTagComm && size >= 4) {
      info.community_title_id = LoadBE32(body);
    } else if (tag == kTagTitl &&
               size >= (kTitlTitleChars + kTitlDescriptionChars) * 2) {
      info.title = ReadFixedUtf16BE(body, kTitlTitleChars);
      info.description =
          ReadFixedUtf16BE(body + kTitlTitleChars * 2, kTitlDescriptionChars);
    }

    offset += size;
  }

  if (!has_exec || info.entry_assembly.empty()) {
    return std::nullopt;
  }
  return info;
}

}  // namespace util
}  // namespace kernel
}  // namespace xe
