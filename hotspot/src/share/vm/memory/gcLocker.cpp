/*
 * Copyright (c) 1997, 2014, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "memory/gcLocker.inline.hpp"
#include "memory/resourceArea.hpp"
#include "memory/sharedHeap.hpp"
#include "runtime/thread.inline.hpp"

volatile jint GC_locker::_jni_lock_count = 0;
volatile bool GC_locker::_needs_gc       = false;
volatile bool GC_locker::_doing_gc       = false;
unsigned int  GC_locker::_total_collections = 0;


// 调试代码，可不用看
#ifdef ASSERT
volatile jint GC_locker::_debug_jni_lock_count = 0;
#endif


// 调试代码，可不用看
#ifdef ASSERT
void GC_locker::verify_critical_count() {
  if (SafepointSynchronize::is_at_safepoint()) {
    assert(!needs_gc() || _debug_jni_lock_count == _jni_lock_count, "must agree");
    int count = 0;
    // Count the number of threads with critical operations in progress
    for (JavaThread* thr = Threads::first(); thr; thr = thr->next()) {
      if (thr->in_critical()) {
        count++;
      }
    }
    if (_jni_lock_count != count) {
      tty->print_cr("critical counts don't match: %d != %d", _jni_lock_count, count);
      for (JavaThread* thr = Threads::first(); thr; thr = thr->next()) {
        if (thr->in_critical()) {
          tty->print_cr(INTPTR_FORMAT " in_critical %d", p2i(thr), thr->in_critical());
        }
      }
    }
    assert(_jni_lock_count == count, "must be equal");
  }
}
#endif




// 如果当前存在进入临界区还未释放的线程返回true(顺便会设置_needs_gc标记让GCLocker感知)，否则返回false。
bool GC_locker::check_active_before_gc() {
  assert(SafepointSynchronize::is_at_safepoint(), "only read at safepoint");

  // 当前存在进入临界区还未释放的线程，且当前时刻没有GC触发
  if (is_active() && !_needs_gc) {
    // 调试代码，可不用看
    verify_critical_count();


    // 唯一的重点: 那就告诉GCLocker，当前有GC请求
    _needs_gc = true;


    // PrintJNIGCStalls的默认值为false，这段日志默认不会打印。
    if (PrintJNIGCStalls && PrintGCDetails) {
      ResourceMark rm; // JavaThread::name() allocates to convert to UTF8
      gclog_or_tty->print_cr("%.3f: Setting _needs_gc. Thread \"%s\" %d locked.",
                             gclog_or_tty->time_stamp().seconds(), Thread::current()->name(), _jni_lock_count);
    }

  }
  return is_active();
}




void GC_locker::stall_until_clear() {
  assert(!JavaThread::current()->in_critical(), "Would deadlock");
  MutexLocker   ml(JNICritical_lock);

  if (needs_gc()) {
    if (PrintJNIGCStalls && PrintGCDetails) {
      ResourceMark rm; // JavaThread::name() allocates to convert to UTF8
      gclog_or_tty->print_cr("%.3f: Allocation failed. Thread \"%s\" is stalled by JNI critical section, %d locked.",
                             gclog_or_tty->time_stamp().seconds(), Thread::current()->name(), _jni_lock_count);
    }
  }

  // Wait for _needs_gc  to be cleared
  while (needs_gc()) {
    JNICritical_lock->wait();
  }
}




// 本质就是对比下GCLocker期间是否已经有别的GC已经发生过了，如果发生过就不再执行本次GC，避免无谓的GC停顿
bool GC_locker::should_discard(GCCause::Cause cause, uint total_collections) {
  return (cause == GCCause::_gc_locker) &&
         (_total_collections != total_collections);
}




// 处理这种情况: 如果当前待进入临界点操作的线程，发现有别的线程已经在临界点但是已经有gc请求在等待，那么当前线程暂时不进入(先hold)
// 待前序临界点全部执行完成后+前序的gc完成后，再执行本次临界点进入操作
void GC_locker::jni_lock(JavaThread* thread) {
  assert(!thread->in_critical(), "shouldn't currently be in a critical region");
  MutexLocker mu(JNICritical_lock);
  // Block entering threads if we know at least one thread is in a
  // JNI critical region and we need a GC.
  // We check that at least one thread is in a critical region before
  // blocking because blocked threads are woken up by a thread exiting
  // a JNI critical region.
  while (is_active_and_needs_gc() || _doing_gc) {
    JNICritical_lock->wait();
  }
  thread->enter_critical();
  _jni_lock_count++;   // 这里可以不++么...enter_critical内部处理过了safepoint时候会汇总的


  // 调试代码，不做关注
  increment_debug_jni_lock_count();
}




// 处理这种情况: 最后一个线程退出临界点，如果有等待执行的GC，本次触发，且唤醒之前可能被wait的线程
void GC_locker::jni_unlock(JavaThread* thread) {
  assert(thread->in_last_critical(), "should be exiting critical region");
  MutexLocker mu(JNICritical_lock);

  // 当前线程退出临界点
  _jni_lock_count--;


  // 调试代码，不做关注
  decrement_debug_jni_lock_count();


  // 临界点标记清零
  thread->exit_critical();


  // 有等待进行的GC且所有线程已经退出临界点
  if (needs_gc() && !is_active_internal()) {
    // We're the last thread out. Request a GC.
    // Capture the current total collections, to allow detection of
    // other collections that make this one unnecessary.  The value of
    // total_collections() is only changed at a safepoint, so there
    // must not be a safepoint between the lock becoming inactive and
    // getting the count, else there may be unnecessary GCLocker GCs.
    _total_collections = Universe::heap()->total_collections();
    _doing_gc = true;
    {
      // Must give up the lock while at a safepoint
      MutexUnlocker munlock(JNICritical_lock);


      // PrintJNIGCStalls的默认值为false，这段日志默认不会打印。
      if (PrintJNIGCStalls && PrintGCDetails) {
        ResourceMark rm; // JavaThread::name() allocates to convert to UTF8
        gclog_or_tty->print_cr("%.3f: Thread \"%s\" is performing GC after exiting critical section, %d locked",
            gclog_or_tty->time_stamp().seconds(), Thread::current()->name(), _jni_lock_count);
      }


      // 触发一次原因为_gc_locker的GC。
      Universe::heap()->collect(GCCause::_gc_locker);
    }
    _doing_gc = false;
    _needs_gc = false;


    // 唤醒下一批之前已经被wait的临界区线程
    JNICritical_lock->notify_all();
  }
}




// ===================== 下面的可以全部都不看 =====================
// Implementation of No_GC_Verifier

#ifdef ASSERT

No_GC_Verifier::No_GC_Verifier(bool verifygc) {
  _verifygc = verifygc;
  if (_verifygc) {
    CollectedHeap* h = Universe::heap();
    assert(!h->is_gc_active(), "GC active during No_GC_Verifier");
    _old_invocations = h->total_collections();
  }
}


No_GC_Verifier::~No_GC_Verifier() {
  if (_verifygc) {
    CollectedHeap* h = Universe::heap();
    assert(!h->is_gc_active(), "GC active during No_GC_Verifier");
    if (_old_invocations != h->total_collections()) {
      fatal("collection in a No_GC_Verifier secured function");
    }
  }
}

Pause_No_GC_Verifier::Pause_No_GC_Verifier(No_GC_Verifier * ngcv) {
  _ngcv = ngcv;
  if (_ngcv->_verifygc) {
    // if we were verifying, then make sure that nothing is
    // wrong before we "pause" verification
    CollectedHeap* h = Universe::heap();
    assert(!h->is_gc_active(), "GC active during No_GC_Verifier");
    if (_ngcv->_old_invocations != h->total_collections()) {
      fatal("collection in a No_GC_Verifier secured function");
    }
  }
}


Pause_No_GC_Verifier::~Pause_No_GC_Verifier() {
  if (_ngcv->_verifygc) {
    // if we were verifying before, then reenable verification
    CollectedHeap* h = Universe::heap();
    assert(!h->is_gc_active(), "GC active during No_GC_Verifier");
    _ngcv->_old_invocations = h->total_collections();
  }
}


// JRT_LEAF rules:
// A JRT_LEAF method may not interfere with safepointing by
//   1) acquiring or blocking on a Mutex or JavaLock - checked
//   2) allocating heap memory - checked
//   3) executing a VM operation - checked
//   4) executing a system call (including malloc) that could block or grab a lock
//   5) invoking GC
//   6) reaching a safepoint
//   7) running too long
// Nor may any method it calls.
JRT_Leaf_Verifier::JRT_Leaf_Verifier()
  : No_Safepoint_Verifier(true, JRT_Leaf_Verifier::should_verify_GC())
{
}

JRT_Leaf_Verifier::~JRT_Leaf_Verifier()
{
}

bool JRT_Leaf_Verifier::should_verify_GC() {
  switch (JavaThread::current()->thread_state()) {
  case _thread_in_Java:
    // is in a leaf routine, there must be no safepoint.
    return true;
  case _thread_in_native:
    // A native thread is not subject to safepoints.
    // Even while it is in a leaf routine, GC is ok
    return false;
  default:
    // Leaf routines cannot be called from other contexts.
    ShouldNotReachHere();
    return false;
  }
}
#endif
