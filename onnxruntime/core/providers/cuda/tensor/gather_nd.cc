// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/providers/cuda/tensor/gather_nd.h"
#include "core/providers/cuda/tensor/gather_nd_impl.h"
#include "core/providers/cuda/shared_inc/cuda_utils.h"

namespace onnxruntime {
namespace cuda {

Status CheckBatchDimensionsMatch(
    size_t num_batch_dimensions,
    const std::vector<std::reference_wrapper<TensorShape>>& tensor_shapes) {
  for (size_t tensor_shape_idx = 0; tensor_shape_idx < tensor_shapes.size(); ++tensor_shape_idx) {
    const TensorShape& tensor_shape = tensor_shapes[tensor_shape_idx];
    ORT_RETURN_IF_NOT(
        num_batch_dimensions <= tensor_shape.NumDimensions(),
        "Number of batch dimensions exceeds tensor rank. ",
        "Batch dimension count: ", num_batch_dimensions,
        ", tensor rank: ", tensor_shape.NumDimensions(),
        ", tensor index: ", tensor_shape_idx);
  }

  if (tensor_shapes.empty()) return Status::OK();

  const TensorShape& first_tensor_shape = tensor_shapes.front();
  for (size_t batch_dimension_idx = 0; batch_dimension_idx < num_batch_dimensions; ++batch_dimension_idx) {
    for (size_t tensor_shape_idx = 1; tensor_shape_idx < tensor_shapes.size(); ++tensor_shape_idx) {
      const TensorShape& other_tensor_shape = tensor_shapes[tensor_shape_idx];
      ORT_RETURN_IF_NOT(
          first_tensor_shape[batch_dimension_idx] == other_tensor_shape[batch_dimension_idx],
          "Batch dimensions differ at index ", batch_dimension_idx, ": ",
          first_tensor_shape[batch_dimension_idx], " != ", other_tensor_shape[batch_dimension_idx],
          ", tensor indices: 0, ", tensor_shape_idx);
    }
  }

  return Status::OK();
}

template <typename T>
struct ComputeImpl {
  void operator()(const int64_t num_slices,
                  const int64_t slice_size,
                  const void* const kernel_input_data,
                  const bool fwd,
                  void* const kernel_output_data,
                  int64_t* const input_slice_offsets_data) const {
    typedef typename ToCudaType<T>::MappedType CudaT;
    if (fwd) {
      GatherNDImpl<CudaT>(num_slices, kernel_input_data,
                          kernel_output_data, slice_size,
                          input_slice_offsets_data);
    } else {
#ifdef ENABLE_TRAINING
      GatherNDGradImpl<CudaT>(num_slices, kernel_input_data,
                              kernel_output_data, slice_size,
                              input_slice_offsets_data);
#else
      return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                             "Gradient computation is only supported in the training mode.");
#endif
    }
  }
};

template <typename TIndex>
Status GatherNDBase::CommonComputeKernel(
    const int64_t batch_dims,
    const TensorShape& input_shape,
    const Tensor* kernel_input_tensor,
    Tensor* kernel_output_tensor,
    const TensorShape& indices_shape,
    const Tensor* indices_tensor,
    const bool fwd) const {
  // Note on naming:
  // `input` refers to the GatherND `data` input, while `kernel_input` refers to
  // what the GatherND[Grad] CUDA kernel accepts as input.

  const auto num_slice_dims = indices_shape[indices_shape.NumDimensions() - 1];
  const auto num_slices = indices_shape.SizeToDimension(indices_shape.NumDimensions() - 1);
  const auto slice_size = input_shape.SizeFromDimension(batch_dims + num_slice_dims);
  const auto num_batches = input_shape.SizeToDimension(batch_dims);
  const auto input_batch_stride = input_shape.SizeFromDimension(batch_dims);
  const auto num_slices_per_batch = num_slices / num_batches;

  const TIndex* const indices_data = indices_tensor->Data<TIndex>();
  const void* const kernel_input_data = kernel_input_tensor->DataRaw();
  void* const kernel_output_data = kernel_output_tensor->MutableDataRaw();

  std::vector<int64_t> sizes_from_slice_dims(num_slice_dims);
  {
    auto running_product = slice_size;
    for (int64_t i = 0; i < num_slice_dims; ++i) {
      sizes_from_slice_dims[num_slice_dims - 1 - i] = running_product;
      running_product *= input_shape[batch_dims + num_slice_dims - 1 - i];
    }
  }

  auto sizes_from_slice_dims_buffer = GetScratchBuffer<int64_t>(sizes_from_slice_dims.size());
  CUDA_RETURN_IF_ERROR(cudaMemcpyAsync(
      sizes_from_slice_dims_buffer.get(),
      sizes_from_slice_dims.data(),
      sizes_from_slice_dims.size() * sizeof(int64_t),
      cudaMemcpyHostToDevice));

  auto input_slice_offsets_buffer = GetScratchBuffer<int64_t>(num_slices);

  TArray<int64_t> input_dims(input_shape.GetDims());
  // TODO reuse computed slice offsets from GatherND in GatherNDGrad
  ComputeSliceOffsetsImpl(
      batch_dims,
      input_dims,
      num_slices,
      num_slices_per_batch,
      input_batch_stride,
      num_slice_dims,
      sizes_from_slice_dims_buffer.get(),
      indices_data,
      input_slice_offsets_buffer.get());

  utils::MLTypeCallDispatcher<ComputeImpl, float, MLFloat16, double>
      t_disp(kernel_input_tensor->GetElementType());
  t_disp.Invoke(num_slices, slice_size, kernel_input_data, fwd, kernel_output_data, input_slice_offsets_buffer.get());
  return Status::OK();
}

#define REGISTER_KERNEL_TYPED_GATHER_ND(TIndex, ver)                      \
  ONNX_OPERATOR_TYPED_KERNEL_EX(                                          \
      GatherND,                                                           \
      kOnnxDomain,                                                        \
      ver,                                                                \
      TIndex,                                                             \
      kCudaExecutionProvider,                                             \
      KernelDefBuilder()                                                  \
          .TypeConstraint("T", DataTypeImpl::AllIEEEFloatTensorTypes())   \
          .TypeConstraint("Tind", DataTypeImpl::GetTensorType<TIndex>()), \
      GatherND<TIndex>);

// TODO: decprecate GatherND-1 after updating training models to opset-12
#ifdef ENABLE_TRAINING
REGISTER_KERNEL_TYPED_GATHER_ND(int64_t, 1)
#endif
REGISTER_KERNEL_TYPED_GATHER_ND(int64_t, 12)

template <typename TIndex>
Status GatherND<TIndex>::ComputeInternal(OpKernelContext* context) const {
  auto input_tensor = context->Input<Tensor>(0);
  auto indices_tensor = context->Input<Tensor>(1);
  ORT_RETURN_IF_NOT(input_tensor != nullptr);
  ORT_RETURN_IF_NOT(indices_tensor != nullptr);

  auto input_shape = input_tensor->Shape();
  auto indices_shape = indices_tensor->Shape();

  if (indices_shape.NumDimensions() == 0) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                           "indices tensor must has rank larger than 0");
  }

  auto last_indices_dimension = batch_dims_ + indices_shape[indices_shape.NumDimensions() - 1];
  if (last_indices_dimension > static_cast<int64_t>(input_shape.NumDimensions())) {
    return ORT_MAKE_STATUS(ONNXRUNTIME, INVALID_ARGUMENT,
                           "last dimension of indices must not be larger than rank of input tensor");
  }

  ORT_RETURN_IF_ERROR(CheckBatchDimensionsMatch(
      static_cast<size_t>(batch_dims_), {input_shape, indices_shape}));

  // Output shape
  std::vector<int64_t> shape(indices_shape.GetDims().begin(), indices_shape.GetDims().end() - 1);
  shape.insert(shape.end(), input_shape.GetDims().begin() + last_indices_dimension, input_shape.GetDims().end());

  auto output_tensor = context->Output(0, TensorShape(shape));

  // Compute
  auto status = CommonComputeKernel<TIndex>(batch_dims_, input_shape, input_tensor, output_tensor, indices_shape, indices_tensor, true);

  return status;
}

}  // namespace cuda
}  // namespace onnxruntime
