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
#include "oneflow/core/common/balanced_splitter.h"
#include "oneflow/core/framework/nd_sbp.h"
#include "oneflow/core/job/nd_sbp_util.h"
#include "oneflow/core/common/switch_func.h"
#include "oneflow/core/framework/framework.h"
#include "oneflow/user/kernels/slice_util.h"
#include "oneflow/core/kernel/kernel_util.h"
#include "oneflow/user/kernels/op_kernel_wrapper.h"
#include "oneflow/core/kernel/cuda_graph_support.h"

namespace oneflow {

namespace {

const int SPLIT_AXIS_FOR_NON_SPLIT = -1;

// [start, end)
int64_t GetSizeInSlice(const int64_t start, const int64_t end, const int64_t step) {
  if (end <= start) { return 0; }
  return (end - start - 1) / step + 1;
}

struct SliceContext final {
  SliceContext(int64_t split_axis, int64_t lower, int64_t upper, int64_t logical_length)
      : split_axis(split_axis), lower(lower), upper(upper), logical_length(logical_length) {}

  // These fields shows how the logical tensor is splited.
  // The logical tensor is splited on the axis `split_axis`
  // The physical tensor on current device is in the range [lower, upper)
  // The length of the logical tensor on `split_axis` is `logical_length`
  // Example:
  // Variable shape = (8, 7, 6, 5), sbp = S(0), on 4 devices, then on the first card:
  // split_axis = 0
  // lower = 0
  // upper = 2
  // logical_length = 8
  const int64_t split_axis;
  const int64_t lower;
  const int64_t upper;
  const int64_t logical_length;
};

void ConstructSliceParamsLarge(const SliceContext& ctx, const std::vector<int64_t>& start_vec,
                               const std::vector<int64_t>& stop_vec,
                               const std::vector<int64_t>& step_vec, const ShapeView& shape,
                               SliceParams* slice_param) {
  const int64_t ndim = shape.NumAxes();
  CHECK_LE(ndim, kSliceMaxDims);
  CHECK_EQ(start_vec.size(), ndim);
  CHECK_EQ(stop_vec.size(), ndim);
  CHECK_EQ(step_vec.size(), ndim);

  std::memset(slice_param, 0, sizeof(SliceParams));
  slice_param->ndim = ndim;
  FOR_RANGE(int, i, 0, slice_param->ndim) {
    const int64_t dim_size = shape.At(i);
    const int64_t start_in_full_large = start_vec.at(i);
    const int64_t stop_in_full_large = stop_vec.at(i);
    const int64_t step = step_vec.at(i);
    CHECK_GT(step, 0);
    int64_t start_in_splitted_large = start_in_full_large;
    int64_t stop_in_splitted_large = stop_in_full_large;
    // large tensor has split sbp attribute
    if (i == ctx.split_axis) {
      if (start_in_splitted_large < ctx.lower) {
        start_in_splitted_large =
            ctx.lower + (step - (ctx.lower - start_in_splitted_large) % step) % step;
      }
      start_in_splitted_large = std::min(std::max(start_in_splitted_large, ctx.lower), ctx.upper);
      stop_in_splitted_large = std::min(std::max(stop_in_splitted_large, ctx.lower), ctx.upper);
      start_in_splitted_large -= ctx.lower;
      stop_in_splitted_large -= ctx.lower;
    }
    const int64_t slice_size =
        GetSizeInSlice(start_in_splitted_large, stop_in_splitted_large, step);
    slice_param->dims[i] = dim_size;
    slice_param->start[i] = start_in_splitted_large;
    slice_param->step[i] = step;
    slice_param->size[i] = slice_size;
  }
}

void ConstructSliceParamsSmall(const SliceContext& ctx, const std::vector<int64_t>& start_vec,
                               const std::vector<int64_t>& stop_vec,
                               const std::vector<int64_t>& step_vec, const ShapeView& shape,
                               SliceParams* slice_param) {
  const int64_t ndim = shape.NumAxes();
  CHECK_LE(ndim, kSliceMaxDims);
  CHECK_EQ(start_vec.size(), ndim);
  CHECK_EQ(stop_vec.size(), ndim);
  CHECK_EQ(step_vec.size(), ndim);

  std::memset(slice_param, 0, sizeof(SliceParams));
  slice_param->ndim = ndim;
  FOR_RANGE(int, i, 0, slice_param->ndim) {
    const int64_t start_in_full_large = start_vec.at(i);
    const int64_t step = step_vec.at(i);
    CHECK_GT(step, 0);
    // small tensor has broadcast/partialsum sbp attribute
    const int64_t dim_size = shape.At(i);
    int64_t start_in_full_small = 0;
    int64_t stop_in_full_small = dim_size;
    if (i == ctx.split_axis) {
      start_in_full_small = GetSizeInSlice(start_in_full_large, ctx.lower, step);
      stop_in_full_small = GetSizeInSlice(start_in_full_large, ctx.upper, step);
      start_in_full_small = std::min(std::max<int64_t>(start_in_full_small, 0), dim_size);
      stop_in_full_small = std::min(std::max<int64_t>(stop_in_full_small, 0), dim_size);
    }
    const int64_t slice_size = stop_in_full_small - start_in_full_small;
    slice_param->dims[i] = dim_size;
    slice_param->start[i] = start_in_full_small;
    slice_param->step[i] = 1;
    slice_param->size[i] = slice_size;
  }
}

SliceParams ConstructSliceParams(user_op::KernelComputeContext* ctx, const user_op::Tensor* entire,
                                 const user_op::Tensor* sliced) {
  const auto& start_vec = ctx->Attr<std::vector<int64_t>>("start");
  const auto& stop_vec = ctx->Attr<std::vector<int64_t>>("stop");
  const auto& step_vec = ctx->Attr<std::vector<int64_t>>("step");
  const int64_t ndim = entire->shape().NumAxes();
  CHECK_LE(ndim, kSliceMaxDims);
  if (entire->shape().NumAxes() == 1) {
    CHECK_LE(sliced->shape().NumAxes(), 1);
  } else {
    CHECK_EQ(sliced->shape().NumAxes(), ndim);
  }
  CHECK_EQ(start_vec.size(), ndim);
  CHECK_EQ(stop_vec.size(), ndim);
  CHECK_EQ(step_vec.size(), ndim);

  SliceParams params;
  std::memset(&params, 0, sizeof(SliceParams));
  if (entire->shape().NumAxes() == 1 && sliced->shape().NumAxes() == 0) {
    params.ndim = ndim;
    params.dims[0] = entire->shape().At(0);
    params.start[0] = RegulateSliceStart(start_vec.at(0), entire->shape().At(0));
    params.step[0] = step_vec.at(0);
    params.size[0] = 1;
    return params;
  }
  params.ndim = ndim;
  FOR_RANGE(int, i, 0, params.ndim) {
    const int64_t dim_size = entire->shape().At(i);
    const int64_t slice_size = sliced->shape().At(i);
    const int64_t step = step_vec.at(i);
    CHECK_NE(step, 0);
    const int64_t start = RegulateSliceStart(start_vec.at(i), dim_size);
    const int64_t stop = RegulateSliceStop(stop_vec.at(i), dim_size);
    if (step > 0) {
      CHECK_LT(start + step * (slice_size - 1), stop);
    } else {
      CHECK_GT(start + step * (slice_size - 1), stop);
    }
    params.dims[i] = dim_size;
    params.start[i] = start;
    params.step[i] = step;
    params.size[i] = slice_size;
  }
  return params;
}

}  // namespace

template<DeviceType device_type, typename T>
class SliceKernel final : public user_op::OpKernel, public user_op::CudaGraphSupport {
 public:
  SliceKernel() = default;
  ~SliceKernel() = default;

 private:
  void Compute(user_op::KernelComputeContext* ctx) const override {
    const user_op::Tensor* x_tensor = ctx->Tensor4ArgNameAndIndex("x", 0);
    user_op::Tensor* y_tensor = ctx->Tensor4ArgNameAndIndex("y", 0);
    SliceParams params = ConstructSliceParams(ctx, x_tensor, y_tensor);
    SliceKernelUtil<device_type, T>::Forward(ctx->stream(), params, x_tensor->dptr<T>(),
                                             y_tensor->mut_dptr<T>());
  }
  bool AlwaysComputeWhenAllOutputsEmpty() const override { return false; }
};

template<DeviceType device_type, typename T>
class SliceGradKernel final : public user_op::OpKernel, public user_op::CudaGraphSupport {
 public:
  SliceGradKernel() = default;
  ~SliceGradKernel() = default;

 private:
  void Compute(user_op::KernelComputeContext* ctx) const override {
    const user_op::Tensor* dy_tensor = ctx->Tensor4ArgNameAndIndex("dy", 0);
    user_op::Tensor* dx_tensor = ctx->Tensor4ArgNameAndIndex("dx", 0);
    size_t dx_byte_size = dx_tensor->shape().elem_cnt() * sizeof(T);
    Memset<device_type>(ctx->stream(), dx_tensor->mut_dptr<T>(), 0, dx_byte_size);
    if (dy_tensor->shape().elem_cnt() == 0) { return; }
    SliceParams params = ConstructSliceParams(ctx, dx_tensor, dy_tensor);
    SliceKernelUtil<device_type, T>::Backward(ctx->stream(), params, dy_tensor->dptr<T>(),
                                              dx_tensor->mut_dptr<T>());
  }
  bool AlwaysComputeWhenAllOutputsEmpty() const override { return false; }
};

template<int NDIM, typename T>
void WriteSlice(user_op::KernelComputeContext* ctx, const user_op::Tensor* src,
                user_op::Tensor* dst, const SliceContext& slice_ctx,
                const bool from_large_to_small) {
  const user_op::Tensor* large = from_large_to_small ? src : dst;
  const user_op::Tensor* small = from_large_to_small ? dst : src;
  if (slice_ctx.split_axis != SPLIT_AXIS_FOR_NON_SPLIT) {
    CHECK_EQ(large->shape().At(slice_ctx.split_axis), slice_ctx.upper - slice_ctx.lower);
  }

  std::vector<int64_t> positive_start_vec;
  std::vector<int64_t> positive_stop_vec;
  const std::vector<int64_t> start_attr = ctx->Attr<std::vector<int64_t>>("start");
  const std::vector<int64_t> stop_attr = ctx->Attr<std::vector<int64_t>>("stop");
  const std::vector<int64_t> step_attr = ctx->Attr<std::vector<int64_t>>("step");
  const int64_t ndim = start_attr.size();
  for (int i = 0; i < ndim; i++) {
    const int64_t dim_size = large->shape().At(i);
    positive_start_vec.emplace_back(RegulateSliceStart(
        start_attr.at(i), i == slice_ctx.split_axis ? slice_ctx.logical_length : dim_size));
    positive_stop_vec.emplace_back(RegulateSliceStop(
        stop_attr.at(i), i == slice_ctx.split_axis ? slice_ctx.logical_length : dim_size));
  }
  SliceParams large_slice_param;
  SliceParams small_slice_param;
  ConstructSliceParamsLarge(slice_ctx, positive_start_vec, positive_stop_vec, step_attr,
                            large->shape(), &large_slice_param);
  ConstructSliceParamsSmall(slice_ctx, positive_start_vec, positive_stop_vec, step_attr,
                            small->shape(), &small_slice_param);
  CHECK_EQ(large_slice_param.elem_cnt(), small_slice_param.elem_cnt());

  const int64_t elem_cnt = large_slice_param.elem_cnt();
  SliceIndexHelper<NDIM> entire_splitted_large_idx_cvtr(large_slice_param.dims);
  SliceIndexHelper<NDIM> sliced_splitted_large_idx_cvtr(large_slice_param.size);
  SliceIndexHelper<NDIM> entire_full_small_idx_cvtr(small_slice_param.dims);
  SliceIndexHelper<NDIM> sliced_full_small_idx_cvtr(small_slice_param.size);
  // Calculate the length of continuous part
  int cnt = 1;
  for (int i = NDIM - 1; i >= 0; i--) {
    if (large_slice_param.step[i] == 1) { cnt *= large_slice_param.size[i]; }
    if (!large_slice_param.IsFullSlice(i) || !small_slice_param.IsFullSlice(i)) { break; }
  }
  const auto* src_ptr = src->dptr<T>();
  auto* dst_ptr = dst->mut_dptr<T>();
  for (int i = 0; i < elem_cnt; i += cnt) {
    const int64_t large_offset = SliceOffsetToEntireOffset<NDIM>(
        i, large_slice_param, entire_splitted_large_idx_cvtr, sliced_splitted_large_idx_cvtr);
    const int64_t small_offset = SliceOffsetToEntireOffset<NDIM>(
        i, small_slice_param, entire_full_small_idx_cvtr, sliced_full_small_idx_cvtr);
    const int64_t src_offset = from_large_to_small ? large_offset : small_offset;
    const int64_t dst_offset = from_large_to_small ? small_offset : large_offset;
    AutoMemcpy(ctx->stream(), dst_ptr + dst_offset, src_ptr + src_offset,
               cnt * GetSizeOfDataType(src->data_type()), src->mem_case(), dst->mem_case());
  }
}

#define MAKE_WRITE_SLICE_SWITCH_ENTRY(func_name, N, T) func_name<N, T>
DEFINE_STATIC_SWITCH_FUNC(
    void, WriteSlice, MAKE_WRITE_SLICE_SWITCH_ENTRY, MAKE_NDIM_CTRV_SEQ(DIM_SEQ),
    MAKE_DATA_TYPE_CTRV_SEQ(ARITHMETIC_DATA_TYPE_SEQ UNSIGNED_INT_DATA_TYPE_SEQ BOOL_DATA_TYPE_SEQ
#if defined(WITH_CUDA)
                                HALF_DATA_TYPE_SEQ
#endif
                            ));
#undef MAKE_WRITE_SLICE_SWITCH_ENTRY

std::shared_ptr<user_op::OpKernelCache> CreateSliceCache(user_op::KernelCacheContext* ctx,
                                                         const std::string& large_tensor_name) {
  if (ctx->parallel_ctx().parallel_num() == 1) {
    // split_axis == SPLIT_AXIS_FOR_NON_SPLIT means the sbp attribute is not 'split'
    return std::make_shared<OpKernelCacheWrapper<SliceContext>>(SPLIT_AXIS_FOR_NON_SPLIT, 0, 0, 0);
  }
  // TODO(wyg): support nd_sbp SliceContext
  const NdSbp& in_nd_sbp = ctx->NdSbp4ArgNameAndIndex(large_tensor_name, 0);
  if (in_nd_sbp.sbp_parallel_size() > 1) {
    CHECK(std::all_of(in_nd_sbp.sbp_parallel().begin(), in_nd_sbp.sbp_parallel().end(),
                      [](const SbpParallel& sbp) {
                        return sbp.has_broadcast_parallel() || sbp.has_partial_sum_parallel();
                      }))
        << large_tensor_name << "'s nd_sbp must be broadcast or partial_sum";
    return std::make_shared<OpKernelCacheWrapper<SliceContext>>(SPLIT_AXIS_FOR_NON_SPLIT, 0, 0, 0);
  } else {
    const auto& in_sbp = in_nd_sbp.sbp_parallel(0);
    if (in_sbp.has_split_parallel()) {
      const user_op::TensorDesc* in_logical_desc =
          ctx->LogicalTensorDesc4ArgNameAndIndex(large_tensor_name, 0);
      const auto split_axis = in_sbp.split_parallel().axis();
      const int64_t split_dim_size = in_logical_desc->shape().At(split_axis);
      const int64_t parallel_id = ctx->parallel_ctx().parallel_id();
      BalancedSplitter bs(split_dim_size, ctx->parallel_ctx().parallel_num());
      return std::make_shared<OpKernelCacheWrapper<SliceContext>>(
          split_axis, bs.At(parallel_id).begin(), bs.At(parallel_id).end(), split_dim_size);
    } else if (in_sbp.has_broadcast_parallel() || in_sbp.has_partial_sum_parallel()) {
      return std::make_shared<OpKernelCacheWrapper<SliceContext>>(SPLIT_AXIS_FOR_NON_SPLIT, 0, 0,
                                                                  0);
    } else {
      UNIMPLEMENTED();
    }
  }
}

template<typename T>
class LogicalSliceKernel final : public user_op::OpKernel {
 public:
  LogicalSliceKernel() = default;
  ~LogicalSliceKernel() = default;

  std::shared_ptr<user_op::OpKernelCache> InitOpKernelCache(
      user_op::KernelCacheContext* ctx) const override {
    const SbpParallel& x_sbp = ctx->SbpParallel4ArgNameAndIndex("x", 0);
    const SbpParallel& y_sbp = ctx->SbpParallel4ArgNameAndIndex("y", 0);
    if (ctx->parallel_ctx().parallel_num() > 1) {
      if (x_sbp.has_split_parallel()) {
        CHECK(y_sbp.has_partial_sum_parallel());
      } else if (x_sbp.has_broadcast_parallel()) {
        CHECK(y_sbp.has_broadcast_parallel());
      } else {
        CHECK(x_sbp.has_partial_sum_parallel());
        CHECK(y_sbp.has_partial_sum_parallel());
      }
    }
    return CreateSliceCache(ctx, "x");
  }

 private:
  void Compute(user_op::KernelComputeContext* ctx, user_op::OpKernelState*,
               const user_op::OpKernelCache* cache) const override {
    user_op::Tensor* y_tensor = ctx->Tensor4ArgNameAndIndex("y", 0);
    const user_op::Tensor* x_tensor = ctx->Tensor4ArgNameAndIndex("x", 0);
    const SliceContext& slice_ctx =
        dynamic_cast<const OpKernelCacheWrapper<SliceContext>*>(cache)->Get();
    AutoMemset(ctx->stream(), y_tensor->mut_dptr(), 0,
               y_tensor->shape().elem_cnt() * GetSizeOfDataType(y_tensor->data_type()),
               y_tensor->mem_case());
    SwitchWriteSlice(SwitchCase(y_tensor->shape().NumAxes(), y_tensor->data_type()), ctx, x_tensor,
                     y_tensor, slice_ctx, true);
  }
  bool AlwaysComputeWhenAllOutputsEmpty() const override { return false; }
};

template<typename T>
class LogicalSliceAssignKernel final : public user_op::OpKernel {
 public:
  LogicalSliceAssignKernel() = default;
  ~LogicalSliceAssignKernel() = default;

  std::shared_ptr<user_op::OpKernelCache> InitOpKernelCache(
      user_op::KernelCacheContext* ctx) const override {
    if (ctx->parallel_ctx().parallel_num() > 1) {
      const NdSbp& value_nd_sbp = ctx->NdSbp4ArgNameAndIndex("value", 0);
      CHECK(std::all_of(value_nd_sbp.sbp_parallel().begin(), value_nd_sbp.sbp_parallel().end(),
                        [](const SbpParallel& sbp) {
                          return sbp.has_partial_sum_parallel() || sbp.has_broadcast_parallel();
                        }))
          << "value's sbp must be broadcast or partial_sum";
    }
    return CreateSliceCache(ctx, "ref");
  }

 private:
  void Compute(user_op::KernelComputeContext* ctx, user_op::OpKernelState*,
               const user_op::OpKernelCache* cache) const override {
    const user_op::Tensor* value_tensor = ctx->Tensor4ArgNameAndIndex("value", 0);
    user_op::Tensor* ref_tensor = ctx->Tensor4ArgNameAndIndex("ref", 0);
    user_op::Tensor* y_tensor = ctx->Tensor4ArgNameAndIndex("y", 0);
    // When eager executing, y_tensor shared the same memory with ref_tensor
    if (ref_tensor->dptr<T>() != y_tensor->dptr<T>()) {
      // lazy run
      AutoMemcpy(ctx->stream(), y_tensor->mut_dptr<T>(), ref_tensor->dptr<T>(),
                 y_tensor->shape().elem_cnt() * sizeof(T), ref_tensor->mem_case(),
                 y_tensor->mem_case());
    }
    const SliceContext& slice_ctx =
        dynamic_cast<const OpKernelCacheWrapper<SliceContext>*>(cache)->Get();
    SwitchWriteSlice(SwitchCase(value_tensor->shape().NumAxes(), value_tensor->data_type()), ctx,
                     value_tensor, y_tensor, slice_ctx, false);
  }
  bool AlwaysComputeWhenAllOutputsEmpty() const override { return true; }
};

template<DeviceType device_type, typename T>
class SliceUpdateKernel final : public user_op::OpKernel {
 public:
  SliceUpdateKernel() = default;
  ~SliceUpdateKernel() = default;

 private:
  void Compute(user_op::KernelComputeContext* ctx) const override {
    const user_op::Tensor* x_tensor = ctx->Tensor4ArgNameAndIndex("x", 0);
    const user_op::Tensor* update_tensor = ctx->Tensor4ArgNameAndIndex("update", 0);
    user_op::Tensor* y_tensor = ctx->Tensor4ArgNameAndIndex("y", 0);
    Memcpy<device_type>(ctx->stream(), y_tensor->mut_dptr<T>(), x_tensor->dptr<T>(),
                        y_tensor->shape().elem_cnt() * sizeof(T));
    SliceParams params = ConstructSliceParams(ctx, y_tensor, update_tensor);
    SliceKernelUtil<device_type, T>::Backward(ctx->stream(), params, update_tensor->dptr<T>(),
                                              y_tensor->mut_dptr<T>());
  }
  bool AlwaysComputeWhenAllOutputsEmpty() const override { return false; }
};

#define REGISTER_SLICE_KERNELS(device, dtype)                                                   \
  REGISTER_USER_KERNEL("slice").SetCreateFn<SliceKernel<device, dtype>>().SetIsMatchedHob(      \
      (user_op::HobDeviceType() == device)                                                      \
      && (user_op::HobDataType("y", 0) == GetDataType<dtype>::value));                          \
  REGISTER_USER_KERNEL("slice_grad")                                                            \
      .SetCreateFn<SliceGradKernel<device, dtype>>()                                            \
      .SetIsMatchedHob((user_op::HobDeviceType() == device)                                     \
                       && (user_op::HobDataType("dx", 0) == GetDataType<dtype>::value));        \
  REGISTER_USER_KERNEL("slice_update")                                                          \
      .SetCreateFn<SliceUpdateKernel<device, dtype>>()                                          \
      .SetIsMatchedHob((user_op::HobDeviceType() == device)                                     \
                       && (user_op::HobDataType("x", 0) == GetDataType<dtype>::value)           \
                       && (user_op::HobDataType("update", 0) == GetDataType<dtype>::value))     \
      .SetInplaceProposalFn([](const user_op::InferContext&,                                    \
                               user_op::AddInplaceArgPair AddInplaceArgPairFn) -> Maybe<void> { \
        OF_RETURN_IF_ERROR(AddInplaceArgPairFn("y", 0, "x", 0, true));                          \
        return Maybe<void>::Ok();                                                               \
      });

#define REGISTER_SLICE_KERNELS_WITH_DEVICE(device) \
  REGISTER_SLICE_KERNELS(device, bool)             \
  REGISTER_SLICE_KERNELS(device, float)            \
  REGISTER_SLICE_KERNELS(device, double)           \
  REGISTER_SLICE_KERNELS(device, int32_t)          \
  REGISTER_SLICE_KERNELS(device, int64_t)          \
  REGISTER_SLICE_KERNELS(device, int8_t)           \
  REGISTER_SLICE_KERNELS(device, uint8_t)

REGISTER_SLICE_KERNELS_WITH_DEVICE(DeviceType::kCPU)
#ifdef WITH_CUDA
REGISTER_SLICE_KERNELS_WITH_DEVICE(DeviceType::kCUDA)
REGISTER_SLICE_KERNELS(DeviceType::kCUDA, float16)
#endif

#define REGISTER_LOGICAL_SLICE_ASSIGN_AND_LOGICAL_SLICE_KERNELS(dtype)               \
  REGISTER_USER_KERNEL("logical_slice_assign")                                       \
      .SetCreateFn<LogicalSliceAssignKernel<dtype>>()                                \
      .SetIsMatchedHob(user_op::HobDataType("ref", 0) == GetDataType<dtype>::value); \
  REGISTER_USER_KERNEL("logical_slice")                                              \
      .SetCreateFn<LogicalSliceKernel<dtype>>()                                      \
      .SetIsMatchedHob(user_op::HobDataType("x", 0) == GetDataType<dtype>::value);

REGISTER_LOGICAL_SLICE_ASSIGN_AND_LOGICAL_SLICE_KERNELS(float)
REGISTER_LOGICAL_SLICE_ASSIGN_AND_LOGICAL_SLICE_KERNELS(double)
REGISTER_LOGICAL_SLICE_ASSIGN_AND_LOGICAL_SLICE_KERNELS(int32_t)
REGISTER_LOGICAL_SLICE_ASSIGN_AND_LOGICAL_SLICE_KERNELS(int64_t)
REGISTER_LOGICAL_SLICE_ASSIGN_AND_LOGICAL_SLICE_KERNELS(int8_t)
REGISTER_LOGICAL_SLICE_ASSIGN_AND_LOGICAL_SLICE_KERNELS(uint8_t)
REGISTER_LOGICAL_SLICE_ASSIGN_AND_LOGICAL_SLICE_KERNELS(bool)
#ifdef WITH_CUDA
REGISTER_LOGICAL_SLICE_ASSIGN_AND_LOGICAL_SLICE_KERNELS(float16)
#endif

}  // namespace oneflow
