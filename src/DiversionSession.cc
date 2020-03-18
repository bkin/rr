/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

#include "DiversionSession.h"

#include "AutoRemoteSyscalls.h"
#include "ReplaySession.h"
#include "core.h"
#include "kernel_metadata.h"
#include "log.h"

using namespace std;

namespace rr {

DiversionSession::DiversionSession() : emu_fs(EmuFs::create()) {}

DiversionSession::~DiversionSession() {
  // We won't permanently leak any OS resources by not ensuring
  // we've cleaned up here, but sessions can be created and
  // destroyed many times, and we don't want to temporarily hog
  // resources.
  kill_all_tasks();
  DEBUG_ASSERT(tasks().size() == 0 && vms().size() == 0);
  DEBUG_ASSERT(emu_fs->size() == 0);
}

static void finish_emulated_syscall_with_ret(Task* t, long ret) {
  t->finish_emulated_syscall();
  Registers r = t->regs();
  r.set_syscall_result(ret);
  t->set_regs(r);
}

/**
 * Execute the syscall contained in |t|'s current register set.  The
 * return value of the syscall is set for |t|'s registers, to be
 * returned to the tracee task.
 */
static void execute_syscall(Task* t) {
  t->finish_emulated_syscall();

  AutoRemoteSyscalls remote(t);
  remote.syscall(remote.regs().original_syscallno(), remote.regs().arg1(),
                 remote.regs().arg2(), remote.regs().arg3(),
                 remote.regs().arg4(), remote.regs().arg5(),
                 remote.regs().arg6());
  remote.regs().set_syscall_result(t->regs().syscall_result());
}

template <typename Arch>
static void process_syscall_arch(Task* t, int syscallno) {
  LOG(debug) << "Processing " << syscall_name(syscallno, Arch::arch());

  if (syscallno == Arch::ioctl && t->is_desched_event_syscall()) {
    // The arm/disarm-desched ioctls are emulated as no-ops.
    // However, because the rr preload library expects these
    // syscalls to succeed and aborts if they don't, we fudge a
    // "0" return value.
    finish_emulated_syscall_with_ret(t, 0);
    return;
  }

  switch (syscallno) {
    // We blacklist these syscalls because the params include
    // namespaced identifiers that are different in replay than
    // recording, and during replay they may refer to different,
    // live resources.  For example, if a recorded tracees kills
    // one of its threads, then during replay that killed pid
    // might refer to a live process outside the tracee tree.  We
    // don't want diversion tracees randomly shooting down other
    // processes!
    //
    // We optimistically assume that filesystem operations were
    // intended by the user.
    //
    // There's a potential problem with "fd confusion": in the
    // diversion tasks, fds returned from open() during replay are
    // emulated.  But those fds may accidentally refer to live fds
    // in the task fd table.  So write()s etc may not be writing
    // to the file the tracee expects.  However, the only real fds
    // that leak into tracees are the stdio fds, and there's not
    // much harm that can be caused by accidental writes to them.
    case Arch::ipc:
    case Arch::kill:
    case Arch::rt_sigqueueinfo:
    case Arch::rt_tgsigqueueinfo:
    case Arch::tgkill:
    case Arch::tkill:
      LOG(debug) << "Suppressing syscall "
                 << syscall_name(syscallno, t->arch());
      return;
  }

  LOG(debug) << "Executing syscall " << syscall_name(syscallno, t->arch());
  return execute_syscall(t);
}

static void process_syscall(Task* t, int syscallno){
  RR_ARCH_FUNCTION(process_syscall_arch, t->arch(), t, syscallno)
}

static void handle_ptrace_exit_event(Task *t) {
  t->did_kill();
  t->detach();
  delete t;
}

/**
 * Advance execution until either a signal is received (including a SIGTRAP
 * generated by a single-step) or a syscall is made.
 */
DiversionSession::DiversionResult DiversionSession::diversion_step(
    Task* t, RunCommand command, int signal_to_deliver) {
  DEBUG_ASSERT(command != RUN_SINGLESTEP_FAST_FORWARD);
  assert_fully_initialized();

  DiversionResult result;

  // An exit might have occurred while processing a previous syscall.
  if (t->ptrace_event() == PTRACE_EVENT_EXIT) {
    handle_ptrace_exit_event(t);
    result.status = DIVERSION_EXITED;
    return result;
  }

  // Disable syscall buffering during diversions
  if (t->preload_globals) {
    t->write_mem(REMOTE_PTR_FIELD(t->preload_globals, in_diversion),
                 (unsigned char)1);
  }
  t->set_syscallbuf_locked(1);

  switch (command) {
    case RUN_CONTINUE:
      LOG(debug) << "Continuing to next syscall";
      t->resume_execution(RESUME_SYSEMU, RESUME_WAIT, RESUME_UNLIMITED_TICKS,
                          signal_to_deliver);
      break;
    case RUN_SINGLESTEP:
      LOG(debug) << "Stepping to next insn/syscall";
      t->resume_execution(RESUME_SYSEMU_SINGLESTEP, RESUME_WAIT,
                          RESUME_UNLIMITED_TICKS, signal_to_deliver);
      break;
    default:
      FATAL() << "Illegal run command " << command;
  }

  if (t->ptrace_event() == PTRACE_EVENT_EXIT) {
    handle_ptrace_exit_event(t);
    result.status = DIVERSION_EXITED;
    return result;
  }

  result.status = DIVERSION_CONTINUE;
  if (t->stop_sig()) {
    LOG(debug) << "Pending signal: " << t->get_siginfo();
    result.break_status = diagnose_debugger_trap(t, command);
    LOG(debug) << "Diversion break at ip=" << (void*)t->ip().register_value()
               << "; break=" << result.break_status.breakpoint_hit
               << ", watch=" << !result.break_status.watchpoints_hit.empty()
               << ", singlestep=" << result.break_status.singlestep_complete;
    ASSERT(t,
           !result.break_status.singlestep_complete ||
               command == RUN_SINGLESTEP);
    return result;
  }

  process_syscall(t, t->regs().original_syscallno());
  check_for_watchpoint_changes(t, result.break_status);
  return result;
}

} // namespace rr
