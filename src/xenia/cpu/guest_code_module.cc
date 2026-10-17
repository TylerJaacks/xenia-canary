/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_code_module.h"

#include <algorithm>

#include "xenia/base/threading.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/processor.h"

namespace xe {
namespace cpu {

GuestCodeModule::GuestCodeModule(Processor* processor) : Module(processor) {}

GuestCodeModule::~GuestCodeModule() = default;

bool GuestCodeModule::ContainsAddress(uint32_t address) {
  if (!cvars::guest_code_generation || (address & 3)) {
    return false;
  }
  BaseHeap* heap = memory_->LookupHeap(address);
  if (!heap) {
    return false;
  }
  uint32_t protect = 0;
  if (!heap->QueryProtect(address, &protect)) {
    return false;
  }
  // The frontend reads instructions through the host mapping, so the page has
  // to be readable. Execute-only pages are mapped readable by the kernel.
  return (protect & kMemoryProtectRead) != 0;
}

Symbol::Status GuestCodeModule::DeclareFunction(uint32_t address,
                                                Function** out_function) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = live_.find(address);
  if (it != live_.end()) {
    Function* function = it->second;
    while (function->status() == Symbol::Status::kDeclaring) {
      global_lock.unlock();
      xe::threading::Sleep(std::chrono::microseconds(100));
      global_lock.lock();
    }
    *out_function = function;
    return function->status();
  }

  std::unique_ptr<Function> function = CreateFunction(address);
  Function* raw = function.get();
  owned_.push_back(std::move(function));
  live_[address] = raw;
  *out_function = raw;
  return Symbol::Status::kNew;
}

std::vector<uint32_t> GuestCodeModule::Invalidate(uint32_t low, uint32_t high) {
  std::vector<uint32_t> removed;
  if (high <= low) {
    return removed;
  }
  auto global_lock = global_critical_region_.Acquire();
  if (live_.empty()) {
    return removed;
  }
  // A function overlaps [low, high) when start < high and end > low. Starts
  // are ordered, and no function is longer than max_span_, so only functions
  // starting in [low - max_span_, high) can overlap.
  const uint32_t search_low = low > max_span_ ? low - max_span_ : 0;
  auto it = live_.lower_bound(search_low);
  while (it != live_.end() && it->first < high) {
    Function* function = it->second;
    uint32_t start = it->first;
    uint32_t end = function->end_address();
    if (end <= start) {
      // Not defined yet (or a single instruction); assume one instruction.
      end = start + 4;
    }
    if (end > low) {
      removed.push_back(start);
      it = live_.erase(it);
    } else {
      ++it;
    }
  }
  return removed;
}

size_t GuestCodeModule::live_function_count() {
  auto global_lock = global_critical_region_.Acquire();
  return live_.size();
}

std::unique_ptr<Function> GuestCodeModule::CreateFunction(uint32_t address) {
  return std::unique_ptr<Function>(
      processor_->backend()->CreateGuestFunction(this, address));
}

void GuestCodeModule::NoteFunctionSpan(uint32_t start, uint32_t end) {
  if (end <= start) {
    return;
  }
  auto global_lock = global_critical_region_.Acquire();
  max_span_ = std::max(max_span_, end - start);
}

}  // namespace cpu
}  // namespace xe
