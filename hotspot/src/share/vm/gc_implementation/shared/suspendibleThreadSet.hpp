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

#ifndef SHARE_VM_GC_IMPLEMENTATION_SHARED_SUSPENDIBLETHREADSET_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHARED_SUSPENDIBLETHREADSET_HPP

#include "memory/allocation.hpp"



// *** 简单来说，SuspendibleThreadSet用来管理一批可以暂停的线程。这样外部线程可以直接
//     和SuspendibleThreadSet交互来完成线程批量暂停和恢复。
// *** AI告诉我这东西在safepoint期间也会用到。
// *** 需要注意的是， SuspendibleThreadSet(简称STS)只用于虚拟机内部的线程，Java代码创建的线程不参与。
// A SuspendibleThreadSet is a set of threads that can be suspended.
// A thread can join and later leave the set, and periodically yield.
// If some thread (not in the set) requests, via synchronize(), that
// the threads be suspended, then the requesting thread is blocked
// until all the threads in the set have yielded or left the set. Threads
// may not enter the set when an attempted suspension is in progress. The
// suspending thread later calls desynchronize(), allowing the suspended
// threads to continue.
class SuspendibleThreadSet : public AllStatic {
private:
  static uint   _nthreads;
  static uint   _nthreads_stopped;
  static bool   _suspend_all;
  static double _suspend_all_start;

public:
  // Add the current thread to the set. May block if a suspension is in progress.
  static void join();

  // Removes the current thread from the set.
  static void leave();


  // 确认下是否已经有全局暂停的命令发起了(比如有线程发起SuspendibleThreadSet::synchronize())
  // Returns true if an suspension is in progress.
  static bool should_yield() { return _suspend_all; }


  // Suspends the current thread if a suspension is in progress.
  static void yield();

  // Returns when all threads in the set are suspended.
  static void synchronize();

  // Resumes all suspended threads in the set.
  static void desynchronize();
};




class SuspendibleThreadSetJoiner : public StackObj {
public:
  SuspendibleThreadSetJoiner() {
    SuspendibleThreadSet::join();
  }

  ~SuspendibleThreadSetJoiner() {
    SuspendibleThreadSet::leave();
  }

  bool should_yield() {
    return SuspendibleThreadSet::should_yield();
  }

  void yield() {
    SuspendibleThreadSet::yield();
  }
};

#endif // SHARE_VM_GC_IMPLEMENTATION_SHARED_SUSPENDIBLETHREADSET_HPP
