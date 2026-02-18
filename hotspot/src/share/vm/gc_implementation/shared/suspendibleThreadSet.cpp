/*
 * Copyright (c) 2014, Oracle and/or its affiliates. All rights reserved.
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
#include "gc_implementation/shared/suspendibleThreadSet.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/thread.inline.hpp"

uint   SuspendibleThreadSet::_nthreads          = 0;
uint   SuspendibleThreadSet::_nthreads_stopped  = 0;
bool   SuspendibleThreadSet::_suspend_all       = false;
double SuspendibleThreadSet::_suspend_all_start = 0.0;




// *** 在CMConcurrentMarkingTask处有调用
void SuspendibleThreadSet::join() {
  MonitorLockerEx ml(STS_lock, Mutex::_no_safepoint_check_flag);

  // 如果已经处于暂停阶段，本次join会被迫等待
  while (_suspend_all) {
    ml.wait(Mutex::_no_safepoint_check_flag);
  }

  // 这里仅更新总的参与可暂停线程的数量
  _nthreads++;
}




void SuspendibleThreadSet::leave() {
  MonitorLockerEx ml(STS_lock, Mutex::_no_safepoint_check_flag);
  assert(_nthreads > 0, "Invalid");

  // 这里进攻性总的参与可暂停线程的数量
  _nthreads--;

  // 表示当前线程已经执行结束脱离暂停管控，尝试唤醒STS_lock(但STS_lock有while循环，可能不会在本次唤醒彻底解锁)
  if (_suspend_all) {
    ml.notify_all();
  }
}




// SuspendibleThreadSet::synchronize()被上游调用后，_suspend_all标记会被置为false
void SuspendibleThreadSet::yield() {
  if (_suspend_all) {
    MonitorLockerEx ml(STS_lock, Mutex::_no_safepoint_check_flag);
    if (_suspend_all) {
      _nthreads_stopped++;


      // 因为ConcGCYieldTimeout这个参数的默认值为0，所以这里我们可以先不看
      if (_nthreads_stopped == _nthreads) {
        if (ConcGCYieldTimeout > 0) {
          double now = os::elapsedTime();
          guarantee((now - _suspend_all_start) * 1000.0 < (double)ConcGCYieldTimeout, "Long delay");
        }
      }


      // 通知上游的管理线程，比如safepoint的VMThread，表示当前线程已经暂停了，你可以再次检查是否所有线程已经暂停
      ml.notify_all();



      while (_suspend_all) {
        ml.wait(Mutex::_no_safepoint_check_flag);
      }
      assert(_nthreads_stopped > 0, "Invalid");
      _nthreads_stopped--;
      ml.notify_all();
    }
  }
}




// *** 在safepoint开始执行的函数SafepointSynchronize::begin()会走到这里
void SuspendibleThreadSet::synchronize() {
  assert(Thread::current()->is_VM_thread(), "Must be the VM thread");


  // 因为ConcGCYieldTimeout这个参数的默认值为0，所以这里我们可以先不看
  if (ConcGCYieldTimeout > 0) {
    _suspend_all_start = os::elapsedTime();
  }


  MonitorLockerEx ml(STS_lock, Mutex::_no_safepoint_check_flag);
  assert(!_suspend_all, "Only one at a time");


  // 可暂停的线程全部挂起
  _suspend_all = true;


  // _nthreads是被管理的总线程数，_nthreads_stopped是已经暂停的线程数，如果全部暂停，这两个值是相等的，所以这里死等到全部暂停
  while (_nthreads_stopped < _nthreads) {
    ml.wait(Mutex::_no_safepoint_check_flag);
  }
}




void SuspendibleThreadSet::desynchronize() {
  assert(Thread::current()->is_VM_thread(), "Must be the VM thread");
  MonitorLockerEx ml(STS_lock, Mutex::_no_safepoint_check_flag);
  assert(_nthreads_stopped == _nthreads, "Invalid");
  _suspend_all = false;
  ml.notify_all();
}
