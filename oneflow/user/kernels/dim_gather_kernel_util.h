/*
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#ifndef ONEFLOW_USER_KERNELS_DIM_GATHER_KERNEL_UTIL_H_
#define ONEFLOW_USER_KERNELS_DIM_GATHER_KERNEL_UTIL_H_
#include "oneflow/user/kernels/dim_gather_scatter_util.h"

// Steps for adding a binary operation on gathers are as follows:
// 1. implment binop in DeviceBinOp, for example "Mul":
//    OF_DEVICE_FUNC static void Mul(const T* x, T* y) { *y *= *x; }
//
// 2. Declare Functor in dim_gather_kernel_util.h:
//    DECLARE_DIMGATHER_FUNCTOR(Mul);
//
// 3. Implement functors in dim_gather_kernel_util.cu and cpp file:
//    in .cu file:
//      IMPLEMENT_DIMGATHER_GPUFUNCTOR(Mul);
//      INSTANTIATE_DIM_GATHER_GPUFUNCTORS(Mul);
//    in .cpp file:
//      IMPLEMENT_DIMGATHER_CPUFUNCTOR(Mul);
//      INSTANTIATE_DIM_GATHER_CPUFUNCTORS(Mul);
//
// 4. Implement kernels in dim_gather_kernels.cpp:
//    IMPLEMENT_DIMGATHER_KERNEL_CLASS(Mul);
//
// 5. Register kernels in dim_gather_kernels.cpp:
//    REGISTER_GATHER_OUTPLACE_KERNEL("dim_gather_mul_like", Mul);
#ifdef WITH_CUDA
#include "oneflow/core/cuda/atomic.cuh"
#endif  // WITH_CUDA
#include "oneflow/core/ndarray/xpu_util.h"
#include "oneflow/core/common/nd_index_offset_helper.h"

namespace oneflow {
namespace user_op {

DECLARE_DIMGATHER_FUNCTOR(Update);

template<typename IN_T, typename IDX_T>
OF_DEVICE_FUNC void DoDimGatherBinop(const DimOpIndexNdHelper<IDX_T>& input_nd_helper,
                                     const DimOpIndexNdHelper<IDX_T>& index_nd_helper, int ndim,
                                     int64_t elem_cnt, int32_t dim, const IDX_T* index,
                                     const IN_T* input, IN_T* output, BinaryOpFn<IN_T> bin_op) {
  XPU_1D_KERNEL_LOOP(index_offset, elem_cnt) {
    IDX_T coordinate[kDimGatherMaxDimCount] = {0};
    const IDX_T x = index[index_offset];
    index_nd_helper.OffsetToNdIndex(index_offset, coordinate, ndim);
    coordinate[dim] = x;

    IDX_T input_offset = input_nd_helper.NdIndexToOffset(coordinate, ndim);
    bin_op(input + input_offset, output + index_offset);
    output[index_offset] = input[input_offset];
  }
}

template<typename T>
struct DeviceAdd {
  OF_DEVICE_FUNC static void Invoke(const T* x, T* y) {
#ifdef __CUDA_ARCH__
    cuda::atomic::Add(y, *x);  // TODO:(YaoChi), refine add using float16 -> half -> float -> half
#else
    *y += *x;
#endif
  };
};

template<typename IN_T, typename IDX_T>
OF_DEVICE_FUNC void DoDimScatterAdd(const DimOpIndexNdHelper<IDX_T>& input_nd_helper,
                                    const DimOpIndexNdHelper<IDX_T>& output_nd_helper, int ndim,
                                    int64_t elem_cnt, int32_t dim, const IDX_T* index,
                                    const IN_T* input, IN_T* output) {
  XPU_1D_KERNEL_LOOP(input_offset, elem_cnt) {
    IDX_T coordinate[kDimGatherMaxDimCount] = {0};
    input_nd_helper.OffsetToNdIndex(input_offset, coordinate, ndim);
    coordinate[dim] = index[input_offset];

    IDX_T output_offset = output_nd_helper.NdIndexToOffset(coordinate, ndim);
    DeviceAdd<IN_T>::Invoke(input + input_offset, output + output_offset);
  }
}

#define INSTANTIATE_DIM_GATHER_FUNCTOR(devicetype, dtype, itype, binop) \
  template struct DimGather##binop##Functor<devicetype, dtype, itype>;

#define INSTANTIATE_DIM_GATHER_GPUFUNCTORS(binop)                           \
  INSTANTIATE_DIM_GATHER_FUNCTOR(DeviceType::kGPU, int32_t, int32_t, binop) \
  INSTANTIATE_DIM_GATHER_FUNCTOR(DeviceType::kGPU, float, int32_t, binop)   \
  INSTANTIATE_DIM_GATHER_FUNCTOR(DeviceType::kGPU, double, int32_t, binop)  \
  INSTANTIATE_DIM_GATHER_FUNCTOR(DeviceType::kGPU, float16, int32_t, binop) \
                                                                            \
  INSTANTIATE_DIM_GATHER_FUNCTOR(DeviceType::kGPU, int32_t, int64_t, binop) \
  INSTANTIATE_DIM_GATHER_FUNCTOR(DeviceType::kGPU, float, int64_t, binop)   \
  INSTANTIATE_DIM_GATHER_FUNCTOR(DeviceType::kGPU, double, int64_t, binop)  \
  INSTANTIATE_DIM_GATHER_FUNCTOR(DeviceType::kGPU, float16, int64_t, binop)

#define INSTANTIATE_DIM_GATHER_CPUFUNCTORS(binop)                           \
  INSTANTIATE_DIM_GATHER_FUNCTOR(DeviceType::kCPU, int32_t, int32_t, binop) \
  INSTANTIATE_DIM_GATHER_FUNCTOR(DeviceType::kCPU, float, int32_t, binop)   \
  INSTANTIATE_DIM_GATHER_FUNCTOR(DeviceType::kCPU, double, int32_t, binop)  \
                                                                            \
  INSTANTIATE_DIM_GATHER_FUNCTOR(DeviceType::kCPU, int32_t, int64_t, binop) \
  INSTANTIATE_DIM_GATHER_FUNCTOR(DeviceType::kCPU, float, int64_t, binop)   \
  INSTANTIATE_DIM_GATHER_FUNCTOR(DeviceType::kCPU, double, int64_t, binop)

}  // namespace user_op
}  // namespace oneflow

#endif  // ONEFLOW_USER_KERNELS_DIM_GATHER_KERNEL_UTIL_H_
