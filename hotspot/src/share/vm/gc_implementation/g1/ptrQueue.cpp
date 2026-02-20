/*
 * Copyright (c) 2001, 2014, Oracle and/or its affiliates. All rights reserved.
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
#include "gc_implementation/g1/ptrQueue.hpp"
#include "memory/allocation.hpp"
#include "memory/allocation.inline.hpp"
#include "runtime/mutex.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/thread.inline.hpp"
#include "utilities/ostream.hpp"

PtrQueue::PtrQueue(PtrQueueSet* qset, bool perm, bool active) :
  _qset(qset), _buf(NULL), _index(0), _sz(0), _active(active),
  _perm(perm), _lock(NULL)
{}

PtrQueue::~PtrQueue() {
  assert(_perm || (_buf == NULL), "queue must be flushed before delete");
}

void PtrQueue::flush_impl() {
  if (!_perm && _buf != NULL) {
    if (_index == _sz) {
      // No work to do.
      qset()->deallocate_buffer(_buf);
    } else {
      // We must NULL out the unused entries, then enqueue.
      for (size_t i = 0; i < _index; i += oopSize) {
        _buf[byte_index_to_index((int)i)] = NULL;
      }

      // 将线程私有的队列转移到全局队列中(STAB、卡表)
      qset()->enqueue_complete_buffer(_buf);
    }
    _buf = NULL;
    _index = 0;
  }
}




void PtrQueue::enqueue_known_active(void* ptr) {
  assert(0 <= _index && _index <= _sz, "Invariant.");
  assert(_index == 0 || _buf != NULL, "invariant");


  //  回头验证下这里, 眼见为实...
  if (_lock) {
   tty->print_cr("AAAAAAAAAAAAAAAAAA-I HAVE A LOCK");
  }else{
   tty->print_cr("BBBBBBBBBBBBBBBBBB-I HAVE NOTHING");
  }


  // 处理队列满了的情况
  while (_index == 0) {
    handle_zero_index();
  }


  // 入队列
  assert(_index > 0, "postcondition");
  _index -= oopSize;
  _buf[byte_index_to_index((int)_index)] = ptr;
  assert(0 <= _index && _index <= _sz, "Invariant.");
}




// 将线程私有的队列转移到全局队列中(STAB、卡表)
void PtrQueue::locking_enqueue_completed_buffer(void** buf) {
  assert(_lock->owned_by_self(), "Required.");


  // 他妈的这里就释放锁了，怪不得外层要做多线程的相关容错
  // We have to unlock _lock (which may be Shared_DirtyCardQ_lock) before
  // we acquire DirtyCardQ_CBL_mon inside enqeue_complete_buffer as they
  // have the same rank and we may get the "possible deadlock" message
  _lock->unlock();


  // *** 这个函数的实现依然在ptrQueue.cpp里
  // *** 将线程私有的队列转移到全局队列中(STAB、卡表)
  qset()->enqueue_complete_buffer(buf);


  // We must relock only because the caller will unlock, for the normal
  // case.
  _lock->lock_without_safepoint_check();
}




PtrQueueSet::PtrQueueSet(bool notify_when_complete) :
  _max_completed_queue(0),
  _cbl_mon(NULL), _fl_lock(NULL),
  _notify_when_complete(notify_when_complete),
  _sz(0),
  _completed_buffers_head(NULL),
  _completed_buffers_tail(NULL),
  _n_completed_buffers(0),
  _process_completed_threshold(0), _process_completed(false),
  _buf_free_list(NULL), _buf_free_list_sz(0)
{
  _fl_owner = this;
}




// 当线程私有的队列满了后，从全局队列的空闲缓冲区分配队列给到线程私有队列。这样做的好处是统一化且可以减少不必要的内存申请。
void** PtrQueueSet::allocate_buffer() {
  assert(_sz > 0, "Didn't set a buffer size.");
  MutexLockerEx x(_fl_owner->_fl_lock, Mutex::_no_safepoint_check_flag);


  if (_fl_owner->_buf_free_list != NULL) {
    // 这里取得应该取的是空闲列表的首个缓冲区
    void** res = BufferNode::make_buffer_from_node(_fl_owner->_buf_free_list);

    // 调整空闲列表的链表关系，移除了首个缓冲区，对应的总数量-1
    _fl_owner->_buf_free_list = _fl_owner->_buf_free_list->next();
    _fl_owner->_buf_free_list_sz--;
    return res;
  } else {
    // 如果空闲列表为空，被迫申请内存(这里申请的内存就是堆外内存了)
    // Allocate space for the BufferNode in front of the buffer.
    char *b =  NEW_C_HEAP_ARRAY(char, _sz + BufferNode::aligned_size(), mtGC);
    return BufferNode::make_buffer_from_block(b);
  }
}




void PtrQueueSet::deallocate_buffer(void** buf) {
  assert(_sz > 0, "Didn't set a buffer size.");
  MutexLockerEx x(_fl_owner->_fl_lock, Mutex::_no_safepoint_check_flag);
  BufferNode *node = BufferNode::make_node_from_buffer(buf);
  node->set_next(_fl_owner->_buf_free_list);
  _fl_owner->_buf_free_list = node;
  _fl_owner->_buf_free_list_sz++;
}

void PtrQueueSet::reduce_free_list() {
  assert(_fl_owner == this, "Free list reduction is allowed only for the owner");
  // For now we'll adopt the strategy of deleting half.
  MutexLockerEx x(_fl_lock, Mutex::_no_safepoint_check_flag);
  size_t n = _buf_free_list_sz / 2;
  while (n > 0) {
    assert(_buf_free_list != NULL, "_buf_free_list_sz must be wrong.");
    void* b = BufferNode::make_block_from_node(_buf_free_list);
    _buf_free_list = _buf_free_list->next();
    FREE_C_HEAP_ARRAY(char, b, mtGC);
    _buf_free_list_sz --;
    n--;
  }
}




// 处理队列满了的情况，将线程私有的队列转移到全局队列中
void PtrQueue::handle_zero_index() {
  assert(_index == 0, "Precondition.");

  // 这里的注释其实算是当前函数的综述:
  // *** 1. 将当前已经满的buffer数据记录下来(实际记录到全局buffer里)
  // *** 2. 为当前线程重新分配新的buffer数据内存区域
  // This thread records the full buffer and allocates a new one (while
  // holding the lock if there is one).
  if (_buf != NULL) {
    if (!should_enqueue_buffer()) {
      assert(_index > 0, "the buffer can only be re-used if it's not full");
      return;
    }


    // *** C++中，指针可以直接用在if条件中，这里其实等价于if(_lock != NULL)
    // *** 由于SATB和DCQS的全局队列都是并发的，所以这里的_lock肯定有值
    if (_lock) {
      assert(_lock->owned_by_self(), "Required.");

      // The current PtrQ may be the shared dirty card queue and
      // may be being manipulated by more than one worker thread
      // during a pause. Since the enqueuing of the completed
      // buffer unlocks the Shared_DirtyCardQ_lock more than one
      // worker thread can 'race' on reading the shared queue attributes
      // (_buf and _index) and multiple threads can call into this
      // routine for the same buffer. This will cause the completed
      // buffer to be added to the CBL multiple times.

      // We "claim" the current buffer by caching value of _buf in
      // a local and clearing the field while holding _lock. When
      // _lock is released (while enqueueing the completed buffer)
      // the thread that acquires _lock will skip this code,
      // preventing the subsequent the multiple enqueue, and
      // install a newly allocated buffer below.


      // 这里的操作是为了避免多线程数据重复提交，下面的locking_enqueue_completed_buffer内部一开始就特么把当前lock给释放了
      void** buf = _buf;   // local pointer to completed buffer
      _buf = NULL;         // clear shared _buf field


      // 将线程私有的队列转移到全局队列中(STAB、卡表)
      locking_enqueue_completed_buffer(buf);  // enqueue completed buffer


      // While the current thread was enqueuing the buffer another thread
      // may have a allocated a new buffer and inserted it into this pointer
      // queue. If that happens then we just return so that the current
      // thread doesn't overwrite the buffer allocated by the other thread
      // and potentially losing some dirtied cards.
      //
      // *** 这里是避免多线程并发问题的，算是一层数据安全兜底，先忽略
      if (_buf != NULL) return;
    } else {
      // 这个分支可以直接不看，这里肯定是并发的场景
      if (qset()->process_or_enqueue_complete_buffer(_buf)) {
        // Recycle the buffer. No allocation.
        _sz = qset()->buffer_size();
        _index = _sz;
        return;
      }
    }
  }


  // Reallocate the buffer
  // *** allocate_buffer()的定义就在当前文件,
  // *** allocate_buffer()意思是当线程私有的队列满了后，从全局队列的空闲缓冲区分配队列给到线程私有队列。这样做的好处是统一化且可以减少不必要的内存申请。
  // *** buffer_size()在虚拟机初始化时(见ConcurrentMark::ConcurrentMark)确定，大小为1K
  _buf = qset()->allocate_buffer();
  _sz = qset()->buffer_size();


  // *** 由于是反向队列，所以此处初始化index为整个队列的大小(index==0才意味着队列满了)
  _index = _sz;
  assert(0 <= _index && _index <= _sz, "Invariant.");
}




bool PtrQueueSet::process_or_enqueue_complete_buffer(void** buf) {
  if (Thread::current()->is_Java_thread()) {
    // We don't lock. It is fine to be epsilon-precise here.
    if (_max_completed_queue == 0 || _max_completed_queue > 0 &&
        _n_completed_buffers >= _max_completed_queue + _completed_queue_padding) {
      bool b = mut_process_buffer(buf);
      if (b) {
        // True here means that the buffer hasn't been deallocated and the caller may reuse it.
        return true;
      }
    }
  }
  // The buffer will be enqueued. The caller will have to get a new one.
  enqueue_complete_buffer(buf);
  return false;
}




// 将线程私有的队列转移到全局队列中(STAB、卡表)
void PtrQueueSet::enqueue_complete_buffer(void** buf, size_t index) {
  MutexLockerEx x(_cbl_mon, Mutex::_no_safepoint_check_flag);


  // 将当前线程私有的模型转换为BufferNode模型
  BufferNode* cbn = BufferNode::new_from_buffer(buf);


  // 我看index默认传入都是0...
  cbn->set_index(index);


  if (_completed_buffers_tail == NULL) {
    // 当前PtrQueueSet第一次新增数据的场景，头和尾都是新的这个cbn
    assert(_completed_buffers_head == NULL, "Well-formedness");
    _completed_buffers_head = cbn;
    _completed_buffers_tail = cbn;
  } else {
    // 链表尾插，新来的节点为尾结点
    _completed_buffers_tail->set_next(cbn);
    _completed_buffers_tail = cbn;
  }
  // 链表元素新增，要记住这里的元素不是单个元素，而代表的是单个线程已经满了的私有STAB队列！
  _n_completed_buffers++;


  // *** !_process_completed: 当前数据未堆积
  // *** _process_completed_threshold: STAB缓冲区阈值为20
  if (!_process_completed && _process_completed_threshold >= 0 &&
      _n_completed_buffers >= _process_completed_threshold) {

    // 当全局队列里的缓冲区个数大于等于20个，全局标记设置为[堆积中]，后续G1会识别到然后暂停相关的CMTask
    _process_completed = true;

    // 从SATB的分析来看这个值是false... ? TODO
    if (_notify_when_complete)
      _cbl_mon->notify();
  }


  // 调试相关的忽略
  debug_only(assert_completed_buffer_list_len_correct_locked());
}




int PtrQueueSet::completed_buffers_list_length() {
  int n = 0;
  BufferNode* cbn = _completed_buffers_head;
  while (cbn != NULL) {
    n++;
    cbn = cbn->next();
  }
  return n;
}

void PtrQueueSet::assert_completed_buffer_list_len_correct() {
  MutexLockerEx x(_cbl_mon, Mutex::_no_safepoint_check_flag);
  assert_completed_buffer_list_len_correct_locked();
}

void PtrQueueSet::assert_completed_buffer_list_len_correct_locked() {
  guarantee(completed_buffers_list_length() ==  _n_completed_buffers,
            "Completed buffer length is wrong.");
}

void PtrQueueSet::set_buffer_size(size_t sz) {
  assert(_sz == 0 && sz > 0, "Should be called only once.");
  _sz = sz * oopSize;
}

// Merge lists of buffers. Notify the processing threads.
// The source queue is emptied as a result. The queues
// must share the monitor.
void PtrQueueSet::merge_bufferlists(PtrQueueSet *src) {
  assert(_cbl_mon == src->_cbl_mon, "Should share the same lock");
  MutexLockerEx x(_cbl_mon, Mutex::_no_safepoint_check_flag);
  if (_completed_buffers_tail == NULL) {
    assert(_completed_buffers_head == NULL, "Well-formedness");
    _completed_buffers_head = src->_completed_buffers_head;
    _completed_buffers_tail = src->_completed_buffers_tail;
  } else {
    assert(_completed_buffers_head != NULL, "Well formedness");
    if (src->_completed_buffers_head != NULL) {
      _completed_buffers_tail->set_next(src->_completed_buffers_head);
      _completed_buffers_tail = src->_completed_buffers_tail;
    }
  }
  _n_completed_buffers += src->_n_completed_buffers;

  src->_n_completed_buffers = 0;
  src->_completed_buffers_head = NULL;
  src->_completed_buffers_tail = NULL;

  assert(_completed_buffers_head == NULL && _completed_buffers_tail == NULL ||
         _completed_buffers_head != NULL && _completed_buffers_tail != NULL,
         "Sanity");
}

void PtrQueueSet::notify_if_necessary() {
  MutexLockerEx x(_cbl_mon, Mutex::_no_safepoint_check_flag);
  if (_n_completed_buffers >= _process_completed_threshold || _max_completed_queue == 0) {
    _process_completed = true;
    if (_notify_when_complete)
      _cbl_mon->notify();
  }
}
