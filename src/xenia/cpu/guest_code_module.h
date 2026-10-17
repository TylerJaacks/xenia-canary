/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_CODE_MODULE_H_
#define XENIA_CPU_GUEST_CODE_MODULE_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "xenia/base/mutex.h"
#include "xenia/cpu/module.h"

namespace xe {
namespace cpu {

// Pseudo-module that owns functions whose code was produced by the guest at
// runtime (for example the PowerPC JIT inside the .NET Compact Framework that
// hosts XNA / Xbox Live Indie Games titles).
//
// Unlike XexModule/RawModule it has no image: it claims any committed, readable
// guest address that no real module claims (only while
// --guest_code_generation is enabled), and it can forget functions when the
// guest invalidates the instruction cache for a range so that they get
// re-translated from the new bytes on their next call.
//
// Function objects are never freed: host code for a stale translation may
// still be on some thread's call stack, and the code cache never frees code.
class GuestCodeModule : public Module {
 public:
  explicit GuestCodeModule(Processor* processor);
  ~GuestCodeModule() override;

  const std::string& name() const override { return name_; }
  bool is_executable() const override { return true; }

  bool ContainsAddress(uint32_t address) override;

  Symbol::Status DeclareFunction(uint32_t address,
                                 Function** out_function) override;

  // Forgets every live function that overlaps [low, high) and returns their
  // start addresses so the processor can drop its own references.
  std::vector<uint32_t> Invalidate(uint32_t low, uint32_t high);

  // Called once a function has been translated and its extent is known.
  void NoteFunctionSpan(uint32_t start, uint32_t end);

  size_t live_function_count();

 protected:
  std::unique_ptr<Function> CreateFunction(uint32_t address) override;

 private:
  std::string name_ = "GuestGeneratedCode";
  xe::global_critical_region global_critical_region_;
  // Start address -> current translation for that address.
  std::map<uint32_t, Function*> live_;
  // Every function ever created, including invalidated ones.
  std::vector<std::unique_ptr<Function>> owned_;
  // Largest [start, end) span seen, used to bound the overlap search.
  uint32_t max_span_ = 4;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_GUEST_CODE_MODULE_H_
