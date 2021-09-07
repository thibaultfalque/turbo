// Copyright 2021 Pierre Talbot, Frédéric Pinel

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef VSTORE_HPP
#define VSTORE_HPP

#include <cmath>
#include <cstdio>
#include <cassert>
#include <vector>
#include <string>
#include "cuda_helper.hpp"
#include "memory.hpp"

typedef int Var;

class Interval {

  atomic_int l;
  atomic_int u;

public:

  CUDA Interval(): l(limit_min()), u(limit_max()) {}

  CUDA Interval(int l, int u): l(l), u(u) {}
  CUDA Interval(const Interval& itv): l(itv.lb()), u(itv.ub()) {}

  CUDA INLINE int lb() const {
    return l.load(memory_order_relaxed);
  }

  CUDA INLINE int ub() const {
    return u.load(memory_order_relaxed);
  }

  CUDA INLINE void store_lb(int new_lb) {
    l.store(new_lb, memory_order_relaxed);
  }

  CUDA INLINE void store_ub(int new_ub) {
    u.store(new_ub, memory_order_relaxed);
  }

  CUDA INLINE void store_min_ub(int new_ub) {
    int prev_ub = ub();
    while(prev_ub > new_ub &&
      !u.compare_exchange_weak(prev_ub, new_ub))
    {}
  }

  CUDA void inplace_join(const Interval &b) {
    store_lb(::max<int>(lb(), b.lb()));
    store_ub(::min<int>(ub(), b.ub()));
  }

  CUDA bool is_assigned() const {
    return lb() == ub();
  }

  CUDA bool is_top() const {
    return lb() > ub();
  }

  CUDA Interval neg() const {
    return Interval(-ub(), -lb());
  }

  CUDA bool operator==(int x) const {
    return lb() == x && ub() == x;
  }

  CUDA bool operator==(const Interval& x) const {
    return lb() == x.lb() && ub() == x.ub();
  }

  CUDA bool operator!=(const Interval& other) const {
    return lb() != other.lb() || ub() != other.ub();
  }

  CUDA Interval& operator=(const Interval& other) {
    store_lb(other.lb());
    store_ub(other.ub());
    return *this;
  }

  CUDA void print() const {
    printf("[%d..%d]", lb(), ub());
  }
};

class VStore {
  Array<Interval> data;
  atomic_bool top;

  // The names don't change during solving. We want to avoid useless copies.
// Unfortunately, static member are not supported in CUDA, so we use an instance variable which is never copied.
  char** names;
  size_t names_len;
public:

  void init_names(std::vector<std::string>& vnames) {
    names_len = vnames.size();
    malloc2_managed(names, names_len);
    for(int i=0; i < names_len; ++i) {
      int len = vnames[i].size();
      malloc2_managed(names[i], len + 1);
      for(int j=0; j < len; ++j) {
        names[i][j] = vnames[i][j];
      }
      names[i][len] = '\0';
    }
  }

  void free_names() {
    for(int i = 0; i < names_len; ++i) {
      free2(names[i]);
    }
    free2(names);
  }

  template<typename Allocator>
  CUDA VStore(int nvar, Allocator& allocator):
    data(nvar, allocator), top(false)
  {}

  template<typename Allocator>
  CUDA VStore(const VStore& other, Allocator& allocator):
    data(other.data, allocator),
    top(false),
    names(other.names), names_len(other.names_len)
  {}

  VStore(int nvar): data(nvar), top(false) {}
  VStore(const VStore& other): data(other.data), top(false),
    names(other.names), names_len(other.names_len) {}

  VStore(): data(0), top(false) {}

  CUDA void reset(const VStore& other) {
    assert(size() == other.size());
    for(int i = 0; i < size(); ++i) {
      data[i].store_lb(other.data[i].lb());
      data[i].store_ub(other.data[i].ub());
    }
    top.store(other.is_top(), memory_order_relaxed);
  }

  CUDA bool all_assigned() const {
    for(int i = 0; i < size(); ++i) {
      if(!data[i].is_assigned()) {
        return false;
      }
    }
    return true;
  }

  CUDA bool is_top() const {
    return top.load(memory_order_relaxed);
  }

  CUDA bool is_top(Var x) const {
    return data[x].is_top();
  }

  CUDA const char* name_of(Var x) const {
    return names[x];
  }

  CUDA void print_var(Var x) const {
    printf("%s", names[x]);
  }

  CUDA void print_view(Var* vars) const {
    for(int i=0; vars[i] != -1; ++i) {
      print_var(vars[i]);
      printf(" = ");
      data[vars[i]].print();
      printf("\n");
    }
  }

  CUDA void print() const {
    // The first variable is the fake one, c.f. `ModelBuilder` constructor.
    for(int i=1; i < size(); ++i) {
      print_var(i);
      printf(" = ");
      data[i].print();
      printf("\n");
    }
  }

private:

  CUDA void update_top(Var x) {
    if(data[x].is_top()) {
      top.store(true, memory_order_relaxed);
    }
  }

public:

  // lb <= x <= ub
  CUDA void dom(Var x, Interval itv) {
    data[x].store_lb(itv.lb());
    data[x].store_ub(itv.ub());
    update_top(x);
  }

  CUDA bool update_lb(Var i, int lb) {
    if (data[i].lb() < lb) {
      LOG(printf("Update LB(%s) with %d (old = %d) in %p\n", names[i], lb, data[i].lb(), this));
      data[i].store_lb(lb);
      update_top(i);
      return true;
    }
    return false;
  }

  CUDA bool update_ub(Var i, int ub) {
    if (data[i].ub() > ub) {
      LOG(printf("Update UB(%s) with %d (old = %d) in %p\n", names[i], ub, data[i].ub(), this));
      data[i].store_ub(ub);
      update_top(i);
      return true;
    }
    return false;
  }

  CUDA bool update(Var i, Interval itv) {
    bool has_changed = update_lb(i, itv.lb());
    has_changed |= update_ub(i, itv.ub());
    return has_changed;
  }

  CUDA bool assign(Var i, int v) {
    return update(i, {v, v});
  }

  CUDA Interval& operator[](size_t i) {
    return data[i];
  }

  CUDA const Interval& operator[](size_t i) const {
    return data[i];
  }

  CUDA int lb(Var i) const {
    return data[i].lb();
  }

  CUDA int ub(Var i) const {
    return data[i].ub();
  }

  CUDA size_t size() const { return data.size(); }
};

#endif
