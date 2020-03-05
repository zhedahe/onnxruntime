// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "orttraining/training_ops/cpu/nn/layer_norm.h"
#include "core/framework/tensor.h"
#include "core/util/math_cpuonly.h"
#include "core/providers/common.h"

namespace onnxruntime {
namespace contrib{

// LayerNormGrad

#define REGISTER_KERNEL_TYPED(T)                                  \
  ONNX_OPERATOR_TYPED_KERNEL_EX(                                  \
      LayerNormalizationGrad,                                     \
      kOnnxDomain,                                                \
      9,                                                          \
      T,                                                          \
      kCpuExecutionProvider,                                      \
      KernelDefBuilder()                                          \
          .TypeConstraint("T", DataTypeImpl::GetTensorType<T>()), \
      LayerNormGrad<T>);

REGISTER_KERNEL_TYPED(float)
REGISTER_KERNEL_TYPED(double)

#undef REGISTER_KERNEL_TYPED

template <typename T>
LayerNormGrad<T>::LayerNormGrad(const OpKernelInfo& op_kernel_info)
    : OpKernel{op_kernel_info} {
  ORT_ENFORCE(op_kernel_info.GetAttr("axis", &axis_).IsOK());
}

template <typename T>
Status LayerNormGrad<T>::Compute(OpKernelContext* op_kernel_context) const {
  const Tensor* Y_grad = op_kernel_context->Input<Tensor>(0);
  const Tensor* X = op_kernel_context->Input<Tensor>(1);
  const Tensor* scale = op_kernel_context->Input<Tensor>(2);
  const Tensor* mean = op_kernel_context->Input<Tensor>(3);
  const Tensor* inv_std_var = op_kernel_context->Input<Tensor>(4);

  const auto& X_shape = X->Shape();
  const auto axis = HandleNegativeAxis(axis_, X_shape.NumDimensions());
  const auto N = X_shape.SizeToDimension(axis);
  const auto M = X_shape.SizeFromDimension(axis);
  ORT_ENFORCE(M != 1);
  const auto& scale_shape = scale->Shape();

  Tensor* X_grad = op_kernel_context->Output(0, X_shape);
  Tensor* scale_grad = op_kernel_context->Output(1, scale_shape);
  Tensor* bias_grad = op_kernel_context->Output(2, scale_shape);

  // Note: Eigen has column-major storage order by default
  ConstEigenArrayMap<T> Y_grad_arr{Y_grad->Data<T>(), M, N};
  ConstEigenArrayMap<T> X_arr{X->Data<T>(), M, N};
  ConstEigenVectorArrayMap<T> scale_vec{scale->Data<T>(), M};
  ConstEigenVectorArrayMap<float> mean_vec{mean->Data<float>(), N};
  ConstEigenVectorArrayMap<float> inv_std_var_vec{inv_std_var->Data<float>(), N};

  EigenArrayMap<T> X_grad_arr{X_grad->MutableData<T>(), M, N};
  EigenVectorArrayMap<T> scale_grad_vec{scale_grad->MutableData<T>(), M};
  EigenVectorArrayMap<T> bias_grad_vec{bias_grad->MutableData<T>(), M};

  using Array = Eigen::ArrayXX<T>;
  using RowVector = Eigen::Array<T, 1, Eigen::Dynamic>;

  // A, B, C are calculated as below:
  // A = Y_grad * (X - mean(X)) * inv_std_var
  // B = Y_grad * scale * inv_std_var
  // C = Y_grad * scale * inv_std_var * (X - mean(X)) * inv_std_var

  // A, B, and C are M x N

  Array X_mean_difference_over_std_var =
      (X_arr.rowwise() - mean_vec.cast<T>().transpose()).rowwise() * inv_std_var_vec.cast<T>().transpose();
  Array A = Y_grad_arr * X_mean_difference_over_std_var;
  Array B = (Y_grad_arr.colwise() * scale_vec).rowwise() * inv_std_var_vec.cast<T>().transpose();
  Array C = B * X_mean_difference_over_std_var;

  // mean_B = mean(Y_grad * scale * inv_std_var)
  RowVector mean_B = B.colwise().mean();  // 1 x N

  // mean_C = mean(Y_grad * scale * inv_std_var * (X - mean(X)) * inv_std_var)
  RowVector mean_C = C.colwise().mean();  // 1 x N

  // X_grad = Y_grad * scale * inv_std_var - mean_B - (X - mean(X)) * inv_std_var * mean_C
  //        = B - mean_B - (X - mean(X)) * inv_std_var * mean_c
  X_grad_arr = B.rowwise() - mean_B - X_mean_difference_over_std_var.rowwise() * mean_C;

  // bias_grad = sum(Y_grad)
  bias_grad_vec = Y_grad_arr.rowwise().sum();

  // scale_grad = sum(Y_grad * (X - mean(X)) * inv_std_var)
  //            = sum(A)
  scale_grad_vec = A.rowwise().sum();

  return Status::OK();
}

}  // namespace contrib
}  // namespace onnxruntime
