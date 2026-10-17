/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Kernel user mode: KeCreateUserMode / KeEnterUserMode / KeLeaveUserMode.
//
// The XNA runtime host (RuntimeHost.xex, used by Xbox Live Indie Games) runs
// the .NET Compact Framework execution engine (NetCFUserMode.dll) and the
// managed code it JIT-compiles in PowerPC user mode. The kernel side:
//
//   status = KeEnterUserMode(CONTEXT* context, callback, 0, 0);
//
// switches to user mode with the registers in `context`. Every trap taken in
// user mode (a system call `sc`, or an exception) calls
//
//   callback(EXCEPTION_RECORD* record_or_null, KTRAP_FRAME* frame);
//
// in kernel mode, with the user's volatile registers saved in the trap frame
// and its non-volatile registers still live (the callback stores r14-r31 into
// the frame itself). The callback either returns, resuming user mode from the
// frame, or calls KeLeaveUserMode(status), which abandons user mode and makes
// KeEnterUserMode return `status`. RuntimeHost services each system call that
// way: the callback leaves with 1, the host runs the handler for r0, writes
// the result into a copy of the frame, converts it with KeContextFromKframes
// and enters user mode again where the sc left off.
//
// User mode has its own address space. KeCreateUserMode is given the
// descriptor of a page table the host maintains (one 32-bit PTE per 64 KB
// page below 0xE0000000 and per 4 KB page above), and KeFlushUserModeCurrentTb
// lists the user pages whose entries were removed. User pages often show
// different memory than the same kernel address (the user stack at 0x3Dxxxxxx
// is kernel 0xBDxxxxxx). User-mode code here runs with a second guest address
// space as its membase (Memory::CreateAliasSpace) whose pages are bound on
// first access from that page table, like TLB refills.
//
// KeEnterUserMode runs the user code nested on the host stack and
// KeLeaveUserMode jumps back to it.

#include <atomic>
#include <csetjmp>
#include <cstring>
#include <memory>

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/vec128.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/memory.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xboxkrnl {

using xe::cpu::ppc::PPCContext;

// Matches the kernel's KTRAP_FRAME.
struct X_KTRAP_FRAME {
  uint8_t header[0x50];                // 0x000 STACK_FRAME_HEADER
  xe::be<uint64_t> gpr[32];            // 0x050
  uint8_t exception_record[80];        // 0x150
  xe::be<uint64_t> ctr;                // 0x1A0
  xe::be<uint32_t> lr;                 // 0x1A8
  xe::be<uint32_t> iar;                // 0x1AC
  xe::be<uint32_t> cr;                 // 0x1B0
  xe::be<uint32_t> msr;                // 0x1B4
  xe::be<uint32_t> xer;                // 0x1B8
  xe::be<uint32_t> user_mode_control;  // 0x1BC
};
static_assert_size(X_KTRAP_FRAME, 0x1C0);

// Matches the PowerPC CONTEXT record.
struct X_CONTEXT {
  xe::be<uint32_t> context_flags;      // 0x000
  xe::be<uint32_t> msr;                // 0x004
  xe::be<uint32_t> iar;                // 0x008
  xe::be<uint32_t> lr;                 // 0x00C
  xe::be<uint64_t> ctr;                // 0x010
  xe::be<uint64_t> gpr[32];            // 0x018
  xe::be<uint32_t> cr;                 // 0x118
  xe::be<uint32_t> xer;                // 0x11C
  xe::be<uint64_t> fpscr;              // 0x120
  xe::be<uint64_t> fpr[32];            // 0x128
  xe::be<uint32_t> user_mode_control;  // 0x228
  xe::be<uint32_t> fill;               // 0x22C
  xe::be<uint32_t> vscr[4];            // 0x230
  xe::be<uint32_t> vr[128][4];         // 0x240
};
static_assert_size(X_CONTEXT, 0xA40);

constexpr uint32_t X_CONTEXT_CONTROL = 0x1;
constexpr uint32_t X_CONTEXT_FLOATING_POINT = 0x2;
constexpr uint32_t X_CONTEXT_INTEGER = 0x4;
constexpr uint32_t X_CONTEXT_VECTOR = 0x10;

// MSR reported for user-mode trap frames: EE, PR, FP, ME, IR, DR, RI.
constexpr uint32_t kUserModeMsr = 0x0000F032;

// User-mode page table descriptor (0x1000 bytes) passed to KeCreateUserMode.
// The table itself holds one PTE (physical page | protection) per user page:
// entries [0, 0xE000) for the 64 KB pages below 0xE0000000, then entries for
// the 4 KB pages above. It is physically contiguous in 8 KB chunks whose
// physical addresses >> 13 the descriptor lists as halfwords: 0x1C at +0x800
// (the 64 KB part), then 0x40 at +0x380.
constexpr uint32_t kTbChunks64K = 0x1C;
constexpr uint32_t kTbChunks4K = 0x40;
constexpr uint32_t kTbChunkList64K = 0x800;
constexpr uint32_t kTbChunkList4K = 0x380;
// Pages whose entries were removed, for KeFlushUserModeCurrentTb.
constexpr uint32_t kTbFlushCount = 0x880;
constexpr uint32_t kTbFlushList = 0x884;
constexpr uint32_t kTbFlushListCapacity = 0x19F;

namespace {

std::atomic<uint32_t> user_mode_tb_descriptor{0};

uint32_t UserPageSize(uint32_t address) {
  return address < 0xE0000000 ? 0x10000 : 0x1000;
}

bool ResolveUserModePage(void* context, uint32_t address,
                         Memory::AliasSpacePage* out_page) {
  auto memory = static_cast<Memory*>(context);
  const uint32_t descriptor = user_mode_tb_descriptor.load();
  if (!descriptor) {
    return false;
  }
  const uint32_t page_size = UserPageSize(address);
  const uint32_t index = address < 0xE0000000
                             ? address >> 16
                             : 0xE000 + ((address - 0xE0000000) >> 12);
  const uint32_t byte_offset = index * 4;
  const uint32_t chunk = byte_offset >> 13;
  uint32_t chunk_entry;
  if (chunk < kTbChunks64K) {
    chunk_entry = descriptor + kTbChunkList64K + chunk * 2;
  } else if (chunk < kTbChunks64K + kTbChunks4K) {
    chunk_entry = descriptor + kTbChunkList4K + (chunk - kTbChunks64K) * 2;
  } else {
    return false;
  }
  const uint32_t chunk_physical = uint32_t(xe::load_and_swap<uint16_t>(
                                      memory->TranslateVirtual(chunk_entry)))
                                  << 13;
  const uint32_t pte = xe::load_and_swap<uint32_t>(
      memory->TranslatePhysical(chunk_physical + (byte_offset & 0x1FFF)));
  if (!pte) {
    return false;
  }
  const uint32_t page = address & ~(page_size - 1);
  const uint32_t physical = pte & ~(page_size - 1);
  out_page->guest_address = page;
  out_page->length = page_size;
  if (page >= 0x80000000 && page < 0x90000000 &&
      physical == page - 0x80000000) {
    // A page of an XEX image mapped where it is loaded. Its "physical" page
    // came from KeGetImagePageTableEntry; Xenia keeps images outside guest
    // physical memory, so show the image itself.
    out_page->source_is_physical = false;
    out_page->source_address = page;
  } else {
    out_page->source_is_physical = true;
    out_page->source_address = physical;
  }
  return true;
}

uint32_t PackXer(const PPCContext* ctx) {
  return (uint32_t(ctx->xer_so & 1) << 31) | (uint32_t(ctx->xer_ov & 1) << 30) |
         (uint32_t(ctx->xer_ca & 1) << 29);
}

void UnpackXer(PPCContext* ctx, uint32_t xer) {
  ctx->xer_so = (xer >> 31) & 1;
  ctx->xer_ov = (xer >> 30) & 1;
  ctx->xer_ca = (xer >> 29) & 1;
}

// Floating-point and vector state of the user-mode side of a thread, saved
// at each trap (the trap frame only carries integer state).
//
// On the console user and kernel mode share the FPU across a system call:
// RuntimeHost's dispatcher passes the service only r3-r10 from the trap frame
// and calls it with the floating-point registers as user mode left them, and
// re-enters user mode with an integer/control-only context, so a float result
// comes back in f1 (e.g. SoundEffectInstance.Volume takes an f1 argument).
// So the volatile FP registers (f0-f13) and FPSCR flow across both ways; only
// the non-volatile ones (f14-f31) and the vector state are kept per mode.
constexpr int kFirstNonVolatileFpr = 14;

struct UserFloatingState {
  double f[32];
  uint32_t fpscr;
  xe::vec128_t v[128];
  xe::vec128_t vscr_vec;
  uint8_t vscr_sat;
  uint32_t vrsave;

  void Save(const PPCContext* ctx) {
    std::memcpy(f, ctx->f, sizeof(f));
    fpscr = ctx->fpscr.value;
    std::memcpy(v, ctx->v, sizeof(v));
    vscr_vec = ctx->vscr_vec;
    vscr_sat = ctx->vscr_sat;
    vrsave = ctx->vrsave;
  }
  // Restores what user mode keeps across a system call: the non-volatile FP
  // registers and the vector state. f0-f13 and FPSCR stay as the kernel left
  // them (results).
  void LoadPreserved(PPCContext* ctx) const {
    std::memcpy(&ctx->f[kFirstNonVolatileFpr], &f[kFirstNonVolatileFpr],
                (32 - kFirstNonVolatileFpr) * sizeof(double));
    std::memcpy(ctx->v, v, sizeof(v));
    ctx->vscr_vec = vscr_vec;
    ctx->vscr_sat = vscr_sat;
    ctx->vrsave = vrsave;
  }
  // Hands the volatile FP registers and FPSCR (arguments) to kernel mode.
  void StoreVolatileInto(PPCContext* ctx) const {
    std::memcpy(ctx->f, f, kFirstNonVolatileFpr * sizeof(double));
    ctx->fpscr.value = fpscr;
  }
};

// One KeEnterUserMode call that has not returned yet.
struct UserModeSession {
  UserModeSession* previous = nullptr;
  uint32_t callback = 0;
  uint32_t leave_status = 0;
  bool in_callback = false;
  uint32_t backend_state = 0;
  // Kernel-mode registers at the KeEnterUserMode call.
  std::unique_ptr<PPCContext> kernel;
  std::jmp_buf leave_jmp;
};

struct UserModeThread {
  UserModeSession* session = nullptr;
  bool has_user_floating = false;
  UserFloatingState user_floating;
};

// Allocated the first time a thread enters user mode.
thread_local std::unique_ptr<UserModeThread> user_mode_thread;

UserModeThread* GetUserModeThread() {
  if (!user_mode_thread) {
    user_mode_thread = std::make_unique<UserModeThread>();
  }
  return user_mode_thread.get();
}

void LoadContextIntoRegisters(PPCContext* ctx, const X_CONTEXT* context) {
  const uint32_t flags = context->context_flags;
  // The integer and control parts are always taken: RuntimeHost does not set
  // ContextFlags for the first entry into user mode.
  ctx->lr = uint32_t(context->lr);
  ctx->ctr = context->ctr;
  for (int i = 0; i < 32; ++i) {
    ctx->r[i] = context->gpr[i];
  }
  ctx->set_cr(uint32_t(context->cr));
  UnpackXer(ctx, context->xer);
  if (flags & X_CONTEXT_FLOATING_POINT) {
    for (int i = 0; i < 32; ++i) {
      uint64_t bits = context->fpr[i];
      std::memcpy(&ctx->f[i], &bits, sizeof(bits));
    }
    ctx->fpscr.value = uint32_t(uint64_t(context->fpscr));
  }
  if (flags & X_CONTEXT_VECTOR) {
    for (int i = 0; i < 128; ++i) {
      for (int j = 0; j < 4; ++j) {
        ctx->v[i].u32[j] = context->vr[i][j];
      }
    }
  }
}

void RunUserCode(PPCContext* ctx, uint32_t entry) {
  auto processor = ctx->processor;
  auto function = processor->ResolveFunction(entry);
  if (!function) {
    XELOGE("KeEnterUserMode: no code at user-mode entry {:08X}", entry);
    return;
  }
  // The return address is a sentinel nothing returns to: user code ends by
  // returning to the LR it was given (an exit stub that makes a system call),
  // which the backend treats as a tail call.
  function->Call(ctx->thread_state, 0xBCBCBCBC);
  XELOGE("KeEnterUserMode: user code at {:08X} returned without leaving",
         entry);
}

// sc from guest code. Only user-mode code makes system calls.
bool HandleUserModeSyscall(void* userdata, PPCContext* ctx,
                           uint32_t sc_address) {
  UserModeThread* thread = user_mode_thread.get();
  UserModeSession* session = thread ? thread->session : nullptr;
  if (!session || session->in_callback) {
    return false;
  }

  thread->user_floating.Save(ctx);
  thread->has_user_floating = true;

  // The trap callback runs in kernel mode, in the kernel's address space.
  uint8_t* const user_membase = ctx->virtual_membase;
  ctx->virtual_membase = session->kernel->virtual_membase;

  // Build the trap frame on the kernel stack, below the frame of the code
  // that called KeEnterUserMode.
  const uint32_t kernel_sp = static_cast<uint32_t>(session->kernel->r[1]);
  const uint32_t frame_address =
      (kernel_sp - 0x100 - uint32_t(sizeof(X_KTRAP_FRAME))) & ~uint32_t(0xF);
  auto frame = ctx->TranslateVirtual<X_KTRAP_FRAME*>(frame_address);
  std::memset(frame, 0, sizeof(*frame));
  for (int i = 0; i < 32; ++i) {
    frame->gpr[i] = ctx->r[i];
  }
  frame->ctr = ctx->ctr;
  frame->lr = static_cast<uint32_t>(ctx->lr);
  frame->iar = sc_address + 4;
  frame->cr = static_cast<uint32_t>(ctx->cr());
  frame->msr = kUserModeMsr;
  frame->xer = PackXer(ctx);

  // Call the trap callback in kernel mode: kernel stack, kernel r2 and r13
  // (the PCR). r14-r31 keep the user's values; the callback saves them.
  ctx->r[1] = frame_address;
  ctx->r[2] = session->kernel->r[2];
  ctx->r[13] = session->kernel->r[13];
  ctx->r[3] = 0;  // No exception record: this is a system call.
  ctx->r[4] = frame_address;
  session->in_callback = true;
  ctx->processor->Execute(ctx->thread_state, session->callback);
  session->in_callback = false;

  // The callback returned instead of leaving user mode: resume from the
  // frame. Non-volatile registers are already the user's.
  for (int i = 0; i < 14; ++i) {
    ctx->r[i] = frame->gpr[i];
  }
  ctx->ctr = frame->ctr;
  ctx->lr = uint32_t(frame->lr);
  ctx->set_cr(uint32_t(frame->cr));
  UnpackXer(ctx, frame->xer);
  if (frame->iar != sc_address + 4) {
    XELOGE(
        "User-mode trap callback redirected execution from {:08X} to {:08X}; "
        "this is not supported yet",
        sc_address + 4, uint32_t(frame->iar));
  }
  ctx->virtual_membase = user_membase;
  return true;
}

}  // namespace

dword_result_t KeCreateUserMode_entry(dword_t arg0, lpvoid_t tb_descriptor) {
  auto memory = kernel_memory();
  user_mode_tb_descriptor.store(tb_descriptor.guest_address());
  uint8_t* membase = memory->CreateAliasSpace(ResolveUserModePage, memory);
  memory->InvalidateAliasSpace();
  XELOGI("KeCreateUserMode({:08X}, {:08X}): user address space at {}",
         uint32_t(arg0), tb_descriptor.guest_address(),
         static_cast<void*>(membase));
  if (!membase) {
    XELOGE(
        "KeCreateUserMode: no user address space; user mode will see kernel "
        "addresses");
  }
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(KeCreateUserMode, kThreading, kImplemented);

dword_result_t KeDeleteUserMode_entry(dword_t arg0) {
  user_mode_tb_descriptor.store(0);
  kernel_memory()->InvalidateAliasSpace();
  return X_STATUS_SUCCESS;
}
DECLARE_XBOXKRNL_EXPORT1(KeDeleteUserMode, kThreading, kImplemented);

void KeFlushUserModeCurrentTb_entry(dword_t arg0) {
  auto memory = kernel_memory();
  const uint32_t descriptor = user_mode_tb_descriptor.load();
  if (!descriptor) {
    return;
  }
  const uint32_t count = xe::load_and_swap<uint32_t>(
      memory->TranslateVirtual(descriptor + kTbFlushCount));
  if (count > kTbFlushListCapacity) {
    memory->InvalidateAliasSpace();
    return;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t address = xe::load_and_swap<uint32_t>(
        memory->TranslateVirtual(descriptor + kTbFlushList + i * 4));
    const uint32_t page_size = UserPageSize(address);
    memory->InvalidateAliasSpace(address & ~(page_size - 1), page_size);
  }
}
DECLARE_XBOXKRNL_EXPORT2(KeFlushUserModeCurrentTb, kThreading, kImplemented,
                         kHighFrequency);

void KeFlushUserModeTb_entry(dword_t arg0) {
  kernel_memory()->InvalidateAliasSpace();
}
DECLARE_XBOXKRNL_EXPORT2(KeFlushUserModeTb, kThreading, kImplemented,
                         kHighFrequency);

dword_result_t KeEnterUserMode_entry(pointer_t<X_CONTEXT> context,
                                     dword_t callback, dword_t arg2,
                                     dword_t arg3,
                                     const ppc_context_t& ppc_context) {
  PPCContext* ctx = ppc_context.value();
  auto processor = ctx->processor;
  UserModeThread* thread = GetUserModeThread();

  UserModeSession session;
  session.previous = thread->session;
  session.callback = callback;
  session.kernel = std::make_unique<PPCContext>(*ctx);

  if (thread->has_user_floating) {
    thread->user_floating.LoadPreserved(ctx);
  }
  LoadContextIntoRegisters(ctx, context);
  const uint32_t entry = context->iar;
  if (uint8_t* user_membase = kernel_memory()->alias_space_membase()) {
    ctx->virtual_membase = user_membase;
  }

  session.backend_state = processor->backend()->EnterNestedGuestStack(ctx);
  thread->session = &session;

  bool left = false;
  if (setjmp(session.leave_jmp) == 0) {
    RunUserCode(ctx, entry);
  } else {
    left = true;
  }

  thread->session = session.previous;
  processor->backend()->LeaveNestedGuestStack(ctx, session.backend_state);
  // Back to the kernel-mode registers; the shim stores the result in r3. After
  // a trap the kernel sees the user's volatile FP registers, as on hardware.
  if (left && thread->has_user_floating) {
    thread->user_floating.StoreVolatileInto(session.kernel.get());
  }
  *ctx = *session.kernel;
  processor->backend()->SetGuestRoundingMode(ctx, ctx->fpscr.value & 7);
  return left ? session.leave_status : 0;
}
DECLARE_XBOXKRNL_EXPORT2(KeEnterUserMode, kThreading, kImplemented,
                         kHighFrequency);

void KeLeaveUserMode_entry(dword_t status) {
  UserModeThread* thread = user_mode_thread.get();
  UserModeSession* session = thread ? thread->session : nullptr;
  if (!session) {
    XELOGE("KeLeaveUserMode({:08X}) called outside user mode",
           uint32_t(status));
    return;
  }
  session->leave_status = status;
  session->in_callback = false;
  // Everything above KeEnterUserMode is abandoned: generated code, thunks
  // and host frames with nothing to destroy (the syscall handler,
  // Processor::Execute, this export's shim).
  // - POSIX: jump back without unwinding. RuntimeHost leaves user mode for
  //   every system call, unwinding through generated code each time is slow,
  //   and libgcc's unwinder was seen looping in it.
  // - Windows: MSVC's longjmp unwinds with RtlUnwindEx (as in
  //   XThread::Reenter). It must: with hardware-enforced stack protection
  //   (CET shadow stacks) a non-unwinding longjmp (null jmp_buf Frame) is
  //   rejected with FAST_FAIL_INVALID_SET_OF_CONTEXT.
  std::longjmp(session->leave_jmp, 1);
}
DECLARE_XBOXKRNL_EXPORT2(KeLeaveUserMode, kThreading, kImplemented,
                         kHighFrequency);

void KeContextFromKframes_entry(pointer_t<X_KTRAP_FRAME> frame,
                                pointer_t<X_CONTEXT> context,
                                const ppc_context_t& ppc_context) {
  const uint32_t flags = context->context_flags;
  if (flags & X_CONTEXT_CONTROL) {
    context->msr = frame->msr;
    context->iar = frame->iar;
    context->lr = frame->lr;
    context->ctr = frame->ctr;
  }
  if (flags & X_CONTEXT_INTEGER) {
    for (int i = 0; i < 32; ++i) {
      context->gpr[i] = frame->gpr[i];
    }
    context->cr = frame->cr;
    context->xer = frame->xer;
  }
  if (flags & X_CONTEXT_FLOATING_POINT) {
    // Floating-point state is not in the trap frame; report the user side's.
    UserModeThread* thread = user_mode_thread.get();
    const PPCContext* ctx = ppc_context.value();
    const double* f = (thread && thread->has_user_floating)
                          ? thread->user_floating.f
                          : ctx->f;
    for (int i = 0; i < 32; ++i) {
      uint64_t bits;
      std::memcpy(&bits, &f[i], sizeof(bits));
      context->fpr[i] = bits;
    }
    context->fpscr = (thread && thread->has_user_floating)
                         ? thread->user_floating.fpscr
                         : ctx->fpscr.value;
  }
  context->user_mode_control = frame->user_mode_control;
}
DECLARE_XBOXKRNL_EXPORT2(KeContextFromKframes, kThreading, kImplemented,
                         kHighFrequency);

void RegisterUserModeExports(xe::cpu::ExportResolver* export_resolver,
                             KernelState* kernel_state) {
  kernel_state->processor()->SetSyscallHandler(HandleUserModeSyscall,
                                               kernel_state);
}

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe
