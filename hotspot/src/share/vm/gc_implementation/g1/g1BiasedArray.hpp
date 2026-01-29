/*
 * Copyright (c) 2013, 2014, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_GC_IMPLEMENTATION_G1_G1BIASEDARRAY_HPP
#define SHARE_VM_GC_IMPLEMENTATION_G1_G1BIASEDARRAY_HPP

#include "memory/allocation.hpp"
#include "utilities/debug.hpp"
#include "utilities/ostream.hpp"




// Implements the common base functionality for arrays that contain provisions
// for accessing its elements using a biased index.
// The element type is defined by the instantiating the template.
class G1BiasedMappedArrayBase VALUE_OBJ_CLASS_SPEC {
  friend class VMStructs;
public:
  typedef size_t idx_t;
protected:
  address _base;          // the real base address
  size_t _length;         // the length of the array
  address _biased_base;   // base address biased by "bias" elements
  size_t _bias;           // the bias, i.e. the offset biased_base is located to the right in elements
  uint _shift_by;         // the amount of bits to shift right when mapping to an index of the array.

protected:

  G1BiasedMappedArrayBase() : _base(NULL), _length(0), _biased_base(NULL),
    _bias(0), _shift_by(0) { }




  // 通用的数组分配创建函数(底层利用malloc分配堆外内存，同时考虑了伪共享)
  // Allocate a new array, generic version.
  static address create_new_base_array(size_t length, size_t elem_size);




  // Initialize the members of this class. The biased start address of this array
  // is the bias (in elements) multiplied by the element size.
  void initialize_base(address base, size_t length, size_t bias, size_t elem_size, uint shift_by) {
    assert(base != NULL, "just checking");
    assert(length > 0, "just checking");
    assert(shift_by < sizeof(uintptr_t) * 8, err_msg("Shifting by " SSIZE_FORMAT ", larger than word size?", (size_t) shift_by));
    _base = base;
    _length = length;

    // 这个偏移量的概念看了很久。
    // 本质上，你可以通过内存地址右移拿到数组下标，但这不是真实的下标(没法直接访问)。这里其实解决的就是[右移算出的下标，和真实数组的下标该如何映射]
    // 所以这里的偏移你可以理解成是在当前的base前面补上"虚拟"的节点。这样就可以通过_biased_base直接以数的方式访问数组内容
    _biased_base = base - (bias * elem_size);
    _bias = bias;
    _shift_by = shift_by;
  }




  // 以堆的HeapRegion*数组分配为例，target_elem_size_in_bytes这里的大小为8，实际上就是sizeof(HeapRegion*)
  // Allocate and initialize this array to cover the heap addresses in the range
  // of [bottom, end).
  void initialize(HeapWord* bottom, HeapWord* end, size_t target_elem_size_in_bytes, size_t mapping_granularity_in_bytes) {
    assert(mapping_granularity_in_bytes > 0, "just checking");
    assert(is_power_of_2(mapping_granularity_in_bytes),
      err_msg("mapping granularity must be power of 2, is %zd", mapping_granularity_in_bytes));
    assert((uintptr_t)bottom % mapping_granularity_in_bytes == 0,
      err_msg("bottom mapping area address must be a multiple of mapping granularity %zd, is " PTR_FORMAT,
        mapping_granularity_in_bytes, p2i(bottom)));
    assert((uintptr_t)end % mapping_granularity_in_bytes == 0,
      err_msg("end mapping area address must be a multiple of mapping granularity %zd, is " PTR_FORMAT,
        mapping_granularity_in_bytes, p2i(end)));


    // 以堆的HeapRegion*数组分配为例，这里的内容代表整堆能分配多少个HeapRegion(参数里的mapping_granularity_in_bytes代表了Region大小，比如我指定了16M)
    size_t num_target_elems = pointer_delta(end, bottom, mapping_granularity_in_bytes);


    // 偏移量
    idx_t bias = (uintptr_t)bottom / mapping_granularity_in_bytes;


    // 以堆的HeapRegion*数组分配为例，
    // *** 4GB的Heap和16MB的Region，这里会创建长度为256的数组，每个元素为HeapRegion*(底层利用malloc分配堆外内存，同时考虑了伪共享)
    // *** (这里感觉只是分配了HeapRegion的内部模型用于，并不是真实的堆内存管理分配)
    // *** 4GB的Heap这里理论上分配了256*8=2KB的内存
    address base = create_new_base_array(num_target_elems, target_elem_size_in_bytes);


    initialize_base(base, num_target_elems, bias, target_elem_size_in_bytes, log2_intptr(mapping_granularity_in_bytes));
  }




  size_t bias() const { return _bias; }
  uint shift_by() const { return _shift_by; }

  void verify_index(idx_t index) const PRODUCT_RETURN;
  void verify_biased_index(idx_t biased_index) const PRODUCT_RETURN;
  void verify_biased_index_inclusive_end(idx_t biased_index) const PRODUCT_RETURN;

public:
   // Return the length of the array in elements.
   size_t length() const { return _length; }
};




// Array that provides biased access and mapping from (valid) addresses in the
// heap into this array.
template<class T>
class G1BiasedMappedArray : public G1BiasedMappedArrayBase {
public:
  // 这个idx_t实际上就是size_t，这里的idx意指索引
  typedef G1BiasedMappedArrayBase::idx_t idx_t;


  T* base() const { return (T*)G1BiasedMappedArrayBase::_base; }


  // 明确知道索引下标的情况下，获取元素
  // Return the element of the given array at the given index. Assume
  // the index is valid. This is a convenience method that does sanity
  // checking on the index.
  T get_by_index(idx_t index) const {
    verify_index(index);
    return this->base()[index];
  }


  // 明确知道索引下标的情况下，写元素
  // Set the element of the given array at the given index to the
  // given value. Assume the index is valid. This is a convenience
  // method that does sanity checking on the index.
  void set_by_index(idx_t index, T value) {
    verify_index(index);
    this->base()[index] = value;
  }


  // The raw biased base pointer.
  T* biased_base() const { return (T*)G1BiasedMappedArrayBase::_biased_base; }


  // 不知道索引下标的情况下，基于任意HeapWord知道当前归属于那个元素
  // Return the element of the given array that covers the given word in the
  // heap. Assumes the index is valid.
  T get_by_address(HeapWord* value) const {
    idx_t biased_index = ((uintptr_t)value) >> this->shift_by();
    this->verify_biased_index(biased_index);
    return biased_base()[biased_index];
  }


  // 这个可以不看，目前只有虚拟机内部的test工具使用了
  // Set the value of the array entry that corresponds to the given array.
  void set_by_address(HeapWord * address, T value) {
    idx_t biased_index = ((uintptr_t)address) >> this->shift_by();
    this->verify_biased_index(biased_index);
    biased_base()[biased_index] = value;
  }

protected:


  // 不知道索引下标的情况下，基于任意HeapWord知道当前归属于那个元素的下标
  // Returns the address of the element the given address maps to
  T* address_mapped_to(HeapWord* address) {
    idx_t biased_index = ((uintptr_t)address) >> this->shift_by();
    this->verify_biased_index_inclusive_end(biased_index);
    return biased_base() + biased_index;
  }


public:


  // 实际映射的地址的bottom
  // Return the smallest address (inclusive) in the heap that this array covers.
  HeapWord* bottom_address_mapped() const {
    return (HeapWord*) ((uintptr_t)this->bias() << this->shift_by());
  }


  // 实际映射的地址的end
  // Return the highest address (exclusive) in the heap that this array covers.
  HeapWord* end_address_mapped() const {
    return (HeapWord*) ((uintptr_t)(this->bias() + this->length()) << this->shift_by());
  }


protected:
  virtual T default_value() const = 0;


  // 数组清零
  // Set all elements of the given array to the given value.
  void clear() {
    T value = default_value();
    for (idx_t i = 0; i < length(); i++) {
      set_by_index(i, value);
    }
  }


public:
  G1BiasedMappedArray() {}


  // Allocate and initialize this array to cover the heap addresses in the range
  // of [bottom, end).
  void initialize(HeapWord* bottom, HeapWord* end, size_t mapping_granularity) {
    G1BiasedMappedArrayBase::initialize(bottom, end, sizeof(T), mapping_granularity);
    this->clear();
  }
};

#endif // SHARE_VM_GC_IMPLEMENTATION_G1_G1BIASEDARRAY_HPP
