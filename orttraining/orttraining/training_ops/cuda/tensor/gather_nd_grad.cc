// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "orttraining/training_ops/cuda/tensor/gather_nd_grad.h"
#include "core/providers/cuda/shared_inc/cuda_utils.h"

namespace onnxruntime {
namespace cuda {

#define REGISTER_KERNEL_TYPED_GATHER_ND_GRAD(TIndex)                                                                        \
  ONNX_OPERATOR_TYPED_KERNEL_EX(                                                                                            \
      GatherNDGrad,                                                                                                         \
      kMSDomain,                                                                                                            \
      1,                                                                                                                    \
      TIndex,                                                                                                               \
      kCudaExecutionProvider,                                                                                               \
      KernelDefBuilder().TypeConstraint("T", {DataTypeImpl::GetTensorType<MLFloat16>(),                                     \
                                              DataTypeImpl::GetTensorType<float>(), DataTypeImpl::GetTensorType<double>()}) \
          .TypeConstraint("Tind", DataTypeImpl::GetTensorType<TIndex>())                                                    \
          .TypeConstraint("T1", DataTypeImpl::GetTensorType<int64_t>())                                                     \
          .InputMemoryType<OrtMemTypeCPUInput>(0),                                                                          \
      GatherNDGrad<TIndex>);

REGISTER_KERNEL_TYPED_GATHER_ND_GRAD(int64_t)

template <typename TIndex>
Status GatherNDGrad<TIndex>::ComputeInternal(OpKernelContext* context) const {
  auto shape_tensor = context->Input<Tensor>(0);
  auto indices_tensor = context->Input<Tensor>(1);
  auto update_tensor = context->Input<Tensor>(2);
  ORT_RETURN_IF_NOT(shape_tensor != nullptr);
  ORT_RETURN_IF_NOT(indices_tensor != nullptr);
  ORT_RETURN_IF_NOT(update_tensor != nullptr);

  auto indices_shape = indices_tensor->Shape();
  auto update_shape = update_tensor->Shape();

  if (indices_shape.NumDimensions() == 0) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                           "indices tensor must has rank larger than 0");
  }

  auto last_indices_dimension = batch_dims_ + indices_shape[indices_shape.NumDimensions() - 1];

  //Output
  auto shape_data = shape_tensor->Data<int64_t>();
  auto input_shape = TensorShape(shape_data, shape_tensor->SizeInBytes() / sizeof(shape_tensor->DataType()));

  if (last_indices_dimension > static_cast<int64_t>(input_shape.NumDimensions())) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                           "last dimension of indices must not be larger than rank of input tensor");
  }

  ORT_RETURN_IF_ERROR(CheckBatchDimensionsMatch(
      static_cast<size_t>(batch_dims_), {input_shape, indices_shape, update_shape}));

  auto output_tensor = context->Output(0, input_shape);

  // TODO this memset can be expensive, a sparse tensor representation would help here
  CUDA_RETURN_IF_ERROR(cudaMemsetAsync(output_tensor->MutableDataRaw(), 0, output_tensor->SizeInBytes()));

  auto status = CommonComputeKernel<TIndex>(batch_dims_, input_shape, update_tensor, output_tensor, indices_shape, indices_tensor, false);
  return status;
}

}  // namespace cuda
}  // namespace onnxruntime
