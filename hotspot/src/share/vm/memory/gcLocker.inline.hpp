/*
 * Copyright (c) 2000, 2012, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_MEMORY_GCLOCKER_INLINE_HPP
#define SHARE_VM_MEMORY_GCLOCKER_INLINE_HPP

#include "memory/gcLocker.hpp"




inline void GC_locker::lock_critical(JavaThread* thread) {
  // 处理这种情况: 如果当前待进入临界点操作的线程，发现有别的线程已经在临界点但是已经有gc请求在等待，那么当前线程暂时不进入(先hold)
  // 待前序临界点全部执行完成后+前序的gc完成后，再执行本次临界点进入操作
  if (!thread->in_critical()) {
    if (needs_gc()) {
      // jni_lock call calls enter_critical under the lock so that the
      // global lock count and per thread count are in agreement.
      jni_lock(thread);
      return;
    }
    // 内部为调试代码不看
    increment_debug_jni_lock_count();
  }


  // 当前线程正常进入临界点(后续safepoint环节会将当前线程进入临界区统计到_jni_lock_count中)
  thread->enter_critical();
}




inline void GC_locker::unlock_critical(JavaThread* thread) {

  // 处理这种情况: 最后一个线程退出临界点，如果有等待执行的GC，本次触发，且唤醒之前可能被wait的线程
  if (thread->in_last_critical()) {
    if (needs_gc()) {
      // jni_unlock call calls exit_critical under the lock so that
      // the global lock count and per thread count are in agreement.
      jni_unlock(thread);
      return;
    }
    // 内部为调试代码不看
    decrement_debug_jni_lock_count();
  }


  // 当前线程正常退出临界点(后续safepoint环节会将当前线程退出临界区统计到_jni_lock_count中)
  thread->exit_critical();
}

#endif // SHARE_VM_MEMORY_GCLOCKER_INLINE_HPP
