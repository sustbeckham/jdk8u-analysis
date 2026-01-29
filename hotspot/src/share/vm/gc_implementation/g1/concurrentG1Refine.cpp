/*
 * Copyright (c) 2001, 2013, Oracle and/or its affiliates. All rights reserved.
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
#include "gc_implementation/g1/concurrentG1Refine.hpp"
#include "gc_implementation/g1/concurrentG1RefineThread.hpp"
#include "gc_implementation/g1/g1CollectedHeap.inline.hpp"
#include "gc_implementation/g1/g1HotCardCache.hpp"
#include "runtime/java.hpp"




ConcurrentG1Refine::ConcurrentG1Refine(G1CollectedHeap* g1h, CardTableEntryClosure* refine_closure) :
  _threads(NULL), _n_threads(0),
  _hot_card_cache(g1h)
{
  // 在8核机器上暂认为G1ConcRefinementGreenZone=8即可
  // Ergomonically select initial concurrent refinement parameters
  if (FLAG_IS_DEFAULT(G1ConcRefinementGreenZone)) {
    FLAG_SET_DEFAULT(G1ConcRefinementGreenZone, MAX2<int>(ParallelGCThreads, 1));
  }
  set_green_zone(G1ConcRefinementGreenZone);


  // 在8核机器上暂认为G1ConcRefinementYellowZone=24即可
  if (FLAG_IS_DEFAULT(G1ConcRefinementYellowZone)) {
    FLAG_SET_DEFAULT(G1ConcRefinementYellowZone, green_zone() * 3);
  }
  set_yellow_zone(MAX2<int>(G1ConcRefinementYellowZone, green_zone()));


  // 在8核机器上暂认为G1ConcRefinementRedZone=48即可
  if (FLAG_IS_DEFAULT(G1ConcRefinementRedZone)) {
    FLAG_SET_DEFAULT(G1ConcRefinementRedZone, yellow_zone() * 2);
  }
  set_red_zone(MAX2<int>(G1ConcRefinementRedZone, yellow_zone()));


  // 默认是0，但虚拟机会自适应(暂时认为8核下线程是8)，函数具体实现在当前类
  _n_worker_threads = thread_num();


  // 需要一个额外的线程去年轻代做rset的采样(那就暂时认为8核下线程是9)
  // We need one extra thread to do the young gen rset size sampling.
  _n_threads = _n_worker_threads + 1;


  // ?
  reset_threshold_step();


  // NEW_C_HEAP_ARRAY是使用malloc直接分配内存的宏(也就是说这块内存不在堆上，算是堆外内存了)
  // 给9个RefineThread*申请内存(那就暂时认为8核下线程是9)
  _threads = NEW_C_HEAP_ARRAY(ConcurrentG1RefineThread*, _n_threads, mtGC);


  // 线程ID初始化。这里实际拿的是当前的线程数。
  uint worker_id_offset = DirtyCardQueueSet::num_par_ids();


  // 上面线程的内存已经申请好，这里具体初始化这些线程。详见concurrentG1RefineThread.cpp
  ConcurrentG1RefineThread *next = NULL;
  for (uint i = _n_threads - 1; i != UINT_MAX; i--) {
    ConcurrentG1RefineThread* t = new ConcurrentG1RefineThread(this, next, refine_closure, worker_id_offset, i);
    assert(t != NULL, "Conc refine should have been created");
    if (t->osthread() == NULL) {
        vm_shutdown_during_initialization("Could not create ConcurrentG1RefineThread");
    }

    assert(t->cg1r() == this, "Conc refine thread should refer to this");
    _threads[i] = t;
    next = t;
  }
}




void ConcurrentG1Refine::reset_threshold_step() {
  if (FLAG_IS_DEFAULT(G1ConcRefinementThresholdStep)) {
    // 没有几个研发知道G1ConcRefinementThresholdStep的，所以这里不会去设置，默认走这个分支
    // 8核下=(24-8)/9=1，所以这个阈值是1
    // 暂时不明白这个阈值的具体含义(源码也没注释)，后面再看...
    _thread_threshold_step = (yellow_zone() - green_zone()) / (worker_thread_num() + 1);
  } else {
    _thread_threshold_step = G1ConcRefinementThresholdStep;
  }
}




void ConcurrentG1Refine::init(G1RegionToSpaceMapper* card_counts_storage) {
  _hot_card_cache.initialize(card_counts_storage);
}

void ConcurrentG1Refine::stop() {
  if (_threads != NULL) {
    for (uint i = 0; i < _n_threads; i++) {
      _threads[i]->stop();
    }
  }
}

void ConcurrentG1Refine::reinitialize_threads() {
  reset_threshold_step();
  if (_threads != NULL) {
    for (uint i = 0; i < _n_threads; i++) {
      _threads[i]->initialize();
    }
  }
}

ConcurrentG1Refine::~ConcurrentG1Refine() {
  if (_threads != NULL) {
    for (uint i = 0; i < _n_threads; i++) {
      delete _threads[i];
    }
    FREE_C_HEAP_ARRAY(ConcurrentG1RefineThread*, _threads, mtGC);
  }
}

void ConcurrentG1Refine::threads_do(ThreadClosure *tc) {
  if (_threads != NULL) {
    for (uint i = 0; i < _n_threads; i++) {
      tc->do_thread(_threads[i]);
    }
  }
}

void ConcurrentG1Refine::worker_threads_do(ThreadClosure * tc) {
  if (_threads != NULL) {
    for (uint i = 0; i < worker_thread_num(); i++) {
      tc->do_thread(_threads[i]);
    }
  }
}




// 默认是0，但虚拟机会自适应(暂时认为8核线程下是8)
uint ConcurrentG1Refine::thread_num() {
  return G1ConcRefinementThreads;
}




void ConcurrentG1Refine::print_worker_threads_on(outputStream* st) const {
  for (uint i = 0; i < _n_threads; ++i) {
    _threads[i]->print_on(st);
    st->cr();
  }
}

ConcurrentG1RefineThread * ConcurrentG1Refine::sampling_thread() const {
  return _threads[worker_thread_num()];
}
