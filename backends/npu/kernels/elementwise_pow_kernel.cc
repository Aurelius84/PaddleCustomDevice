// Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "kernels/funcs/npu_funcs.h"
#include "kernels/funcs/npu_op_runner.h"

namespace custom_kernel {

template <typename T, typename Context>
void SumKernel(const Context& dev_ctx,
               const phi::DenseTensor& x,
               const phi::IntArray& axes,
               phi::DataType out_dtype,
               bool keep_dim,
               phi::DenseTensor* out);

bool DetermineDirectCompute(const phi::DenseTensor& x,
                            const phi::DenseTensor& y,
                            int axis) {
  bool direct_compute = false;
  auto x_dims = x.dims();
  auto y_dims = y.dims();
  axis = (axis < 0 ? std::abs(x_dims.size() - y_dims.size()) + axis + 1 : axis);
  if (x_dims.size() >= y_dims.size()) {
    direct_compute = y_dims == phi::slice_ddim(x_dims, axis, x_dims.size());
  } else {
    direct_compute = x_dims == phi::slice_ddim(y_dims, axis, y_dims.size());
  }
  return direct_compute;
}

template <typename T, typename Context>
void AclopElementwisePowRawKernel(const Context& dev_ctx,
                                  const phi::DenseTensor& x,
                                  const phi::DenseTensor& y,
                                  int axis,
                                  phi::DenseTensor* out) {
  dev_ctx.template Alloc<T>(out);
  auto stream = dev_ctx.stream();

  bool direct_compute = DetermineDirectCompute(x, y, axis);
  if (direct_compute) {
    const auto& runner = NpuOpRunner("Pow", {x, y}, {*out}, {});
    runner.Run(stream);
  } else {
    phi::DenseTensor transformed_x, transformed_y;
    custom_kernel::NpuElementWiseOpBroadcast<T>(
        dev_ctx, &x, &y, axis, &transformed_x, &transformed_y);
    const auto& runner =
        NpuOpRunner("Pow", {transformed_x, transformed_y}, {*out}, {});
    runner.Run(stream);
  }
}

template <typename T, typename Context>
void ElementwisePowRawKernel(const Context& dev_ctx,
                             const phi::DenseTensor& x,
                             const phi::DenseTensor& y,
                             int axis,
                             phi::DenseTensor* out) {
  DO_COMPATIBILITY(aclnnPowTensorTensor,
                   (custom_kernel::AclopElementwisePowRawKernel<T, Context>(
                       dev_ctx, x, y, axis, out)));
  dev_ctx.template Alloc<T>(out);

  bool direct_compute = DetermineDirectCompute(x, y, axis);
  if (direct_compute) {
    EXEC_NPU_CMD(aclnnPowTensorTensor, dev_ctx, x, y, *out);
  } else {
    phi::DenseTensor transformed_x, transformed_y;
    custom_kernel::NpuElementWiseOpBroadcast<T>(
        dev_ctx, &x, &y, axis, &transformed_x, &transformed_y);
    EXEC_NPU_CMD(
        aclnnPowTensorTensor, dev_ctx, transformed_x, transformed_y, *out);
  }
}

template <typename T, typename Context>
void ElementwisePowKernel(const Context& dev_ctx,
                          const phi::DenseTensor& x,
                          const phi::DenseTensor& y,
                          phi::DenseTensor* out) {
  int axis = -1;
  custom_kernel::ElementwisePowRawKernel<T>(dev_ctx, x, y, axis, out);
}

template <typename T, typename Context>
void ElementwisePowGradAclop(const Context& dev_ctx,
                             const phi::DenseTensor& x,
                             const phi::DenseTensor& y,
                             const phi::DenseTensor& dout,
                             phi::DenseTensor* dx,
                             phi::DenseTensor* dy) {
  int axis = -1;
  auto x_dims = x.dims();
  auto y_dims = y.dims();
  axis = (axis < 0 ? std::abs(x_dims.size() - y_dims.size()) + axis + 1 : axis);
  phi::DenseTensor transformed_x, transformed_y;
  custom_kernel::NpuElementWiseOpBroadcast<T>(
      dev_ctx, &x, &y, axis, &transformed_x, &transformed_y);

  auto dout_dims = dout.dims();
  auto stream = dev_ctx.stream();
  // Reshape info vector.
  std::vector<int> reduce_axes;
  if (dx) {
    phi::DenseTensor zero_tensor;
    phi::DenseTensorMeta dout_meta = {dout.dtype(), dout_dims};
    zero_tensor.set_meta(dout_meta);
    dev_ctx.template Alloc<T>(&zero_tensor);
    custom_kernel::FillNpuTensorWithConstant<T>(
        &zero_tensor, dev_ctx, static_cast<T>(0));

    dev_ctx.template Alloc<T>(dx);
    phi::DenseTensor tmp_dx;
    tmp_dx.Resize(dout_dims);
    dev_ctx.template Alloc<T>(&tmp_dx);

    // dx = dout * y * pow(x, y - 1);
    phi::DenseTensor PowGrad_dx_temp1;
    PowGrad_dx_temp1.set_meta(dout_meta);
    dev_ctx.template Alloc<T>(&PowGrad_dx_temp1);
    const auto& runner_PowGrad_dx_temp1 =
        NpuOpRunner("Mul", {dout, transformed_y}, {PowGrad_dx_temp1}, {});
    runner_PowGrad_dx_temp1.Run(stream);

    phi::DenseTensor one_dx;
    phi::DenseTensorMeta y_meta = {transformed_y.dtype(), transformed_y.dims()};
    one_dx.set_meta(y_meta);
    dev_ctx.template Alloc<T>(&one_dx);
    const auto& runner_one_dx =
        NpuOpRunner("OnesLike", {transformed_y}, {one_dx}, {});
    runner_one_dx.Run(stream);

    phi::DenseTensor sub_dx;
    sub_dx.set_meta(y_meta);
    dev_ctx.template Alloc<T>(&sub_dx);
    const auto& runner_sub_dx =
        NpuOpRunner("Sub", {transformed_y, one_dx}, {sub_dx}, {});
    runner_sub_dx.Run(stream);

    phi::DenseTensor PowGrad_dx_temp2;
    phi::DenseTensorMeta x_meta = {transformed_x.dtype(), transformed_x.dims()};
    PowGrad_dx_temp2.set_meta(x_meta);
    dev_ctx.template Alloc<T>(&PowGrad_dx_temp2);
    const auto& runner_PowGrad_dx_temp2 =
        NpuOpRunner("Pow", {transformed_x, sub_dx}, {PowGrad_dx_temp2}, {});
    runner_PowGrad_dx_temp2.Run(stream);

    const auto& runner_dx =
        NpuOpRunner("Mul", {PowGrad_dx_temp1, PowGrad_dx_temp2}, {tmp_dx}, {});
    runner_dx.Run(stream);

    if (x_dims != dout_dims) {
      reduce_axes.clear();

      int src_axis = (x_dims.size() < dout_dims.size() ? axis : 0);
      for (int ax = 0; ax < dout_dims.size(); ++ax) {
        if ((ax < src_axis || ax >= src_axis + x_dims.size()) ||
            (dout_dims[ax] > 1 && x_dims[ax - src_axis] == 1)) {
          reduce_axes.push_back(ax);
        }
      }
      if (!reduce_axes.empty()) {
        custom_kernel::SumKernel<T, Context>(dev_ctx,
                                             tmp_dx,
                                             phi::IntArray(reduce_axes),
                                             dx->dtype(),
                                             false,
                                             dx);
      }
    } else {
      TensorCopy(dev_ctx, tmp_dx, false, dx);
    }
  }
  if (dy) {
    phi::DenseTensor zero_tensor;
    phi::DenseTensorMeta dout_meta = {dout.dtype(), dout_dims};
    zero_tensor.set_meta(dout_meta);
    dev_ctx.template Alloc<T>(&zero_tensor);
    custom_kernel::FillNpuTensorWithConstant<T>(
        &zero_tensor, dev_ctx, static_cast<T>(0));

    dev_ctx.template Alloc<T>(dy);
    phi::DenseTensor tmp_dy;
    tmp_dy.Resize(dout_dims);
    dev_ctx.template Alloc<T>(&tmp_dy);

    phi::DenseTensorMeta x_meta = {transformed_x.dtype(), transformed_x.dims()};

    // dy = dout * log(x) * pow(x, y)
    phi::DenseTensor PowGrad_dy_temp1;
    PowGrad_dy_temp1.set_meta(x_meta);
    dev_ctx.template Alloc<T>(&PowGrad_dy_temp1);

    const auto& runner_PowGrad_dy_temp1 = NpuOpRunner(
        "Pow", {transformed_x, transformed_y}, {PowGrad_dy_temp1}, {});
    runner_PowGrad_dy_temp1.Run(stream);

    phi::DenseTensor one_dy;
    one_dy.set_meta(x_meta);
    dev_ctx.template Alloc<T>(&one_dy);
    const auto& runner_one_dy =
        NpuOpRunner("OnesLike", {transformed_x}, {one_dy}, {});
    runner_one_dy.Run(stream);

    phi::DenseTensor sub_dy;
    sub_dy.set_meta(x_meta);
    dev_ctx.template Alloc<T>(&sub_dy);
    const auto& runner_sub_dy =
        NpuOpRunner("Sub", {transformed_x, one_dy}, {sub_dy}, {});
    runner_sub_dy.Run(stream);

    phi::DenseTensor log_dy;
    log_dy.set_meta(x_meta);
    dev_ctx.template Alloc<T>(&log_dy);
    const auto& runner_log_dy = NpuOpRunner("Log1p", {sub_dy}, {log_dy}, {});
    runner_log_dy.Run(stream);

    phi::DenseTensor PowGrad_dy_temp2;
    PowGrad_dy_temp2.set_meta(x_meta);
    dev_ctx.template Alloc<T>(&PowGrad_dy_temp2);
    const auto& runner_PowGrad_dy_temp2 =
        NpuOpRunner("Mul", {log_dy, PowGrad_dy_temp1}, {PowGrad_dy_temp2}, {});
    runner_PowGrad_dy_temp2.Run(stream);

    const auto& runner_dy =
        NpuOpRunner("Mul", {dout, PowGrad_dy_temp2}, {tmp_dy}, {});
    runner_dy.Run(stream);

    if (y_dims != dout_dims) {
      reduce_axes.clear();

      int src_axis = (y_dims.size() < dout_dims.size() ? axis : 0);
      for (int ax = 0; ax < dout_dims.size(); ++ax) {
        if ((ax < src_axis || ax >= src_axis + y_dims.size()) ||
            (dout_dims[ax] > 1 && y_dims[ax - src_axis] == 1)) {
          reduce_axes.push_back(ax);
        }
      }
      if (!reduce_axes.empty()) {
        custom_kernel::SumKernel<T, Context>(dev_ctx,
                                             tmp_dy,
                                             phi::IntArray(reduce_axes),
                                             dy->dtype(),
                                             false,
                                             dy);
      }
    } else {
      TensorCopy(dev_ctx, tmp_dy, false, dy);
    }
  }
  if (!dx && !dy) {
    PADDLE_THROW(
        phi::errors::Unavailable("Not support all outputs to be empty."));
  }
}

template <typename T, typename Context>
void ElementwisePowGradKernel(const Context& dev_ctx,
                              const phi::DenseTensor& x,
                              const phi::DenseTensor& y,
                              const phi::DenseTensor& dout,
                              phi::DenseTensor* dx,
                              phi::DenseTensor* dy) {
  DO_COMPATIBILITY(aclnnPowTensorTensor,
                   (custom_kernel::ElementwisePowGradAclop<T, Context>(
                       dev_ctx, x, y, dout, dx, dy)));
  int axis = -1;

  auto x_dims = x.dims();
  auto y_dims = y.dims();
  auto dout_dims = dout.dims();
  axis = (axis < 0 ? std::abs(x_dims.size() - y_dims.size()) + axis + 1 : axis);
  // const phi::DenseTensor& print_tensor = dout;
  // paddle::funcs::TensorFormatter formatter;
  // formatter.Print(print_tensor);
  std::vector<int64_t> reduce_axes;
  std::vector<int64_t> dst_dims_vec;
  if (dx) {
    dev_ctx.template Alloc<T>(dx);
    if (x_dims != dout_dims) {
      reduce_axes.clear();
      int src_axis = (x_dims.size() < dout_dims.size() ? axis : 0);
      for (int ax = 0; ax < dout_dims.size(); ++ax) {
        if ((ax < src_axis || ax >= src_axis + x_dims.size()) ||
            (dout_dims[ax] > 1 && x_dims[ax - src_axis] == 1)) {
          reduce_axes.push_back(ax);
        } else {
          dst_dims_vec.push_back(dout_dims[ax]);
        }
      }
    }
    phi::DenseTensor y_sub_1;
    phi::DenseTensorMeta y_meta = {y.dtype(), y.dims()};
    y_sub_1.set_meta(y_meta);
    dev_ctx.template Alloc<T>(&y_sub_1);
    phi::DenseTensor x_temp;
    if (reduce_axes.empty()) {
      x_temp = *dx;
    } else {
      x_temp.Resize(dout_dims);
      dev_ctx.template Alloc<T>(&x_temp);
    }
    phi::Scalar acl_scalar_one = phi::Scalar(1.0);
    EXEC_NPU_CMD(
        aclnnSubs, dev_ctx, y, acl_scalar_one, acl_scalar_one, y_sub_1);
    EXEC_NPU_CMD(aclnnPowTensorTensor, dev_ctx, x, y_sub_1, x_temp);
    EXEC_NPU_CMD(aclnnInplaceMul, dev_ctx, x_temp, y);
    EXEC_NPU_CMD(aclnnInplaceMul, dev_ctx, x_temp, dout);
    if (!reduce_axes.empty()) {
      phi::DenseTensor tmp(*dx);
      tmp.Resize(phi::make_ddim(dst_dims_vec));
      bool keep_dims = false;
      auto dtype = ConvertToNpuDtype(x.dtype());
      EXEC_NPU_CMD(
          aclnnReduceSum, dev_ctx, x_temp, reduce_axes, keep_dims, dtype, tmp);
    }
  }
  if (dy) {
    dev_ctx.template Alloc<T>(dy);
    if (y_dims != dout_dims) {
      reduce_axes.clear();
      int src_axis = (y_dims.size() < dout_dims.size() ? axis : 0);
      for (int ax = 0; ax < dout_dims.size(); ++ax) {
        if ((ax < src_axis || ax >= src_axis + y_dims.size()) ||
            (dout_dims[ax] > 1 && y_dims[ax - src_axis] == 1)) {
          reduce_axes.push_back(ax);
        } else {
          dst_dims_vec.push_back(dout_dims[ax]);
        }
      }
    }
    phi::DenseTensor y_temp;
    if (reduce_axes.empty()) {
      y_temp = *dy;
    } else {
      y_temp.Resize(dout_dims);
      dev_ctx.template Alloc<T>(&y_temp);
    }
    phi::DenseTensor x_log;
    phi::DenseTensorMeta x_log_meta = {x.dtype(), x.dims()};
    x_log.set_meta(x_log_meta);
    dev_ctx.template Alloc<T>(&x_log);
    EXEC_NPU_CMD(aclnnPowTensorTensor, dev_ctx, x, y, y_temp);
    EXEC_NPU_CMD(aclnnLog, dev_ctx, x, x_log);
    EXEC_NPU_CMD(aclnnInplaceMul, dev_ctx, y_temp, x_log);
    EXEC_NPU_CMD(aclnnMul, dev_ctx, y_temp, dout, y_temp);
    if (!reduce_axes.empty()) {
      phi::DenseTensor tmp(*dy);
      tmp.Resize(phi::make_ddim(dst_dims_vec));
      bool keep_dims = false;
      auto dtype = ConvertToNpuDtype(y.dtype());
      EXEC_NPU_CMD(
          aclnnReduceSum, dev_ctx, y_temp, reduce_axes, keep_dims, dtype, tmp);
    }
  }
}

}  // namespace custom_kernel

PD_REGISTER_PLUGIN_KERNEL(elementwise_pow,
                          npu,
                          ALL_LAYOUT,
                          custom_kernel::ElementwisePowKernel,
                          int,
                          int64_t,
                          float,
                          phi::dtype::float16,
                          phi::dtype::bfloat16,
                          double) {}

PD_REGISTER_PLUGIN_KERNEL(elementwise_pow_raw,
                          npu,
                          ALL_LAYOUT,
                          custom_kernel::ElementwisePowRawKernel,
                          int,
                          int64_t,
                          float,
                          phi::dtype::float16,
                          double) {}

PD_REGISTER_PLUGIN_KERNEL(elementwise_pow_grad,
                          npu,
                          ALL_LAYOUT,
                          custom_kernel::ElementwisePowGradKernel,
                          int,
                          int64_t,
                          float,
                          phi::dtype::float16,
                          phi::dtype::bfloat16,
                          double) {}
