/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_UTIL_XNA_GAME_INFO_H_
#define XENIA_KERNEL_UTIL_XNA_GAME_INFO_H_

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace xe {
namespace kernel {
namespace util {

// GameInfo.bin is stored in the root of every Xbox Live Indie Game (XBLIG /
// XNA Creators Club) package. The package itself is ordinary STFS marketplace
// content (type 0x00000002) owned by the XNA host title (584E07D2 "Indie
// Games", or 584E07D1 for XNA Game Studio Connect deployments). It contains no
// native code: the game is a set of .NET CF assemblies that the XNA host loads.
//
// Layout (big endian, a sequence of tagged chunks: fourcc tag, be32 size):
//   EXEC (141 bytes): char virtual_title_id[33]   GUID string, NUL terminated
//                     char entry_assembly[43]     e.g. "BasicXNAProject.exe"
//                     char runtime_profile[65]    e.g. "Xbox 360.v4.0.HiDef"
//   COMM (4 bytes):   be32 title_id               per-game "XU" title ID
//   TITL (1280 bytes): char16be title[128], char16be description[512]
// Unknown chunks are skipped.
struct XnaGameInfo {
  std::string virtual_title_id;
  std::string entry_assembly;
  std::string runtime_profile;
  uint32_t community_title_id = 0;
  std::string title;
  std::string description;

  // "Xbox 360.v4.0.HiDef" -> "4.0".
  std::string framework_version() const;
  // "Xbox 360.v4.0.HiDef" -> "HiDef".
  std::string graphics_profile() const;
};

std::optional<XnaGameInfo> ParseXnaGameInfo(std::span<const uint8_t> data);

}  // namespace util
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_UTIL_XNA_GAME_INFO_H_
