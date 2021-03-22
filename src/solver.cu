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

#include <iostream>
#include <algorithm>
#include <stdio.h>
#include <new>
#include <chrono>

#include "solver.cuh"
#include "vstore.cuh"
#include "propagators.cuh"
#include "cuda_helper.hpp"
#include "statistics.cuh"
#include "status.cuh"
#include "search.cuh"

__device__ int decomposition = 0;

#define OR_NODES 48
#define AND_NODES 256
#define SUB_PROBLEMS_POWER 12 // 2^N
// #define SHMEM_SIZE 65536
#define SHMEM_SIZE 44000

#define IN_GLOBAL_MEMORY

CUDA_GLOBAL void search_k(
    Array<Pointer<TreeAndPar>>* trees,
    VStore* root,
    Array<Pointer<Propagator>>* props,
    Array<Var>* branching_vars,
    Pointer<Interval>* best_bound,
    Array<VStore>* best_sols,
    Var minimize_x,
    Array<Statistics>* stats)
{
  #ifndef IN_GLOBAL_MEMORY
    extern __shared__ int shmem[];
    const int n = SHMEM_SIZE;
  #endif
  int tid = threadIdx.x;
  int nodeid = blockIdx.x;
  int stride = blockDim.x;
  __shared__ int curr_decomposition;
  __shared__ int decomposition_size;
  int sub_problems = pow(2, SUB_PROBLEMS_POWER);

  if (tid == 0) {
    decomposition_size = SUB_PROBLEMS_POWER;
    INFO(printf("decomposition = %d, %d\n", decomposition_size, sub_problems));
    #ifdef IN_GLOBAL_MEMORY
      GlobalAllocator allocator;
    #else
      SharedAllocator allocator(shmem, n);
    #endif
    (*trees)[nodeid].reset(new(allocator) TreeAndPar(
      *root, *props, *branching_vars, **best_bound, minimize_x, allocator));
    curr_decomposition = atomicAdd(&decomposition, 1);
  }
  __syncthreads();
  while(curr_decomposition < sub_problems) {
    INFO(if(tid == 0) printf("Block %d with decomposition %d.\n", nodeid, curr_decomposition));
    (*trees)[nodeid]->search(tid, stride, *root, curr_decomposition, decomposition_size);
    if (tid == 0) {
      Statistics latest = (*trees)[nodeid]->statistics();
      if(latest.best_bound != -1 && latest.best_bound < (*stats)[nodeid].best_bound) {
        (*best_sols)[nodeid].reset((*trees)[nodeid]->best());
      }
      (*stats)[nodeid].join(latest);
      curr_decomposition = atomicAdd(&decomposition, 1);
    }
    __syncthreads();
  }
  INFO(if(tid == 0) printf("Block %d quits %d.\n", nodeid, (*stats)[nodeid].best_bound));
  // if(tid == 0)
   // printf("%d: Block %d quits %d.\n", tid, nodeid, (*stats)[nodeid].best_bound);
}

void solve(VStore* vstore, Constraints constraints, Var minimize_x, int timeout)
{
  // INFO(constraints.print(*vstore));

  Array<Var>* branching_vars = constraints.branching_vars();

  LOG(std::cout << "Start transfering propagator to device memory." << std::endl);
  auto t1 = std::chrono::high_resolution_clock::now();
  Array<Pointer<Propagator>>* props = new(managed_allocator) Array<Pointer<Propagator>>(constraints.size());
  LOG(std::cout << "props created " << props->size() << std::endl);
  for (auto p : constraints.propagators) {
    LOG(p->print(*vstore));
    LOG(std::cout << std::endl);
    (*props)[p->uid].reset(p->to_device());
  }
  auto t2 = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>( t2 - t1 ).count();
  LOG(std::cout << "Finish transfering propagators to device memory (" << duration << " ms)" << std::endl);

  t1 = std::chrono::high_resolution_clock::now();

  Array<Pointer<TreeAndPar>>* trees = new(managed_allocator) Array<Pointer<TreeAndPar>>(OR_NODES);
  Pointer<Interval>* best_bound = new(managed_allocator) Pointer<Interval>(Interval());
  Array<VStore>* best_sols = new(managed_allocator) Array<VStore>(*vstore, OR_NODES);
  Array<Statistics>* stats = new(managed_allocator) Array<Statistics>(OR_NODES);

  // cudaFuncSetAttribute(search_k, cudaFuncAttributeMaxDynamicSharedMemorySize, SHMEM_SIZE);
  int and_nodes = min((int)props->size(), AND_NODES);
  search_k<<<OR_NODES, and_nodes
    #ifndef IN_GLOBAL_MEMORY
      , SHMEM_SIZE
    #endif
  >>>(trees, vstore, props, branching_vars, best_bound, best_sols, minimize_x, stats);
  CUDIE(cudaDeviceSynchronize());

  t2 = std::chrono::high_resolution_clock::now();
  duration = std::chrono::duration_cast<std::chrono::milliseconds>( t2 - t1 ).count();

  // Gather statistics and best bound.
  Statistics statistics;
  for(int i = 0; i < stats->size(); ++i) {
    statistics.join((*stats)[i]);
  }

  statistics.print();
  // if(timeout != INT_MAX && duration > timeout * 1000) {
  std::cout << "solveTime=" << duration << std::endl;
  // if(statistics.nodes == NODES_LIMIT) {
  //   std::cout << "solveTime=timeout (" << duration/1000 << "." << duration % 1000 << "s)" << std::endl;
  // }
  // else {
  //   std::cout << "solveTime=" << duration/1000 << "." << duration % 1000 << "s" << std::endl;
  // }

  operator delete(best_bound, managed_allocator);
  operator delete(props, managed_allocator);
  operator delete(trees, managed_allocator);
  operator delete(branching_vars, managed_allocator);
  operator delete(best_bound, managed_allocator);
  operator delete(best_sols, managed_allocator);
}
