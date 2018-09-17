// Copyright 2018 Xiaomi, Inc.  All rights reserved.
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

#include "mace/kernels/space_to_depth.h"
#include "mace/core/runtime/opencl/cl2_header.h"
#include "mace/core/runtime/opencl/opencl_runtime.h"
#include "mace/kernels/opencl/helper.h"
#include "mace/utils/tuner.h"
#include "mace/utils/utils.h"

namespace mace {
namespace kernels {

template <typename T>
MaceStatus SpaceToDepthOpFunctor<DeviceType::GPU, T>::operator()(
    const Tensor *input, Tensor *output, StatsFuture *future) {
  const index_t batch = input->dim(0);
  const index_t input_height = input->dim(1);
  const index_t input_width = input->dim(2);
  const index_t input_depth = input->dim(3);

  MACE_CHECK((input_depth % 4) == 0,
             "input channel should be dividable by 4");
  MACE_CHECK(
      (input_width % block_size_ == 0) && (input_height % block_size_ == 0),
      "input width and height should be dividable by block_size");

  const index_t output_height = input_height / block_size_;
  const index_t output_width = input_width / block_size_;
  const index_t output_depth = input_depth * block_size_ * block_size_;

  const index_t input_depth_blocks = RoundUpDiv4(input_depth);
  const index_t output_depth_blocks = RoundUpDiv4(output_depth);

  std::vector<index_t> output_shape = {batch, output_height, output_width,
                                       output_depth};

  std::vector<size_t> image_shape;
  CalImage2DShape(output_shape, BufferType::IN_OUT_CHANNEL, &image_shape);
  MACE_RETURN_IF_ERROR(output->ResizeImage(output_shape, image_shape));

  auto runtime = context_->device()->opencl_runtime();
  if (kernel_.get() == nullptr) {
    std::set<std::string> built_options;
    OUT_OF_RANGE_CONFIG(kernel_error_, context_);
    NON_UNIFORM_WG_CONFIG;
    const char *kernel_name = "space_to_depth";
    std::string obfuscated_kernel_name = MACE_OBFUSCATE_SYMBOL(kernel_name);
    std::stringstream kernel_name_ss;
    kernel_name_ss << "-D" << kernel_name << "=" << obfuscated_kernel_name;
    built_options.emplace(kernel_name_ss.str());
    auto dt = DataTypeToEnum<T>::value;
    built_options.emplace("-DDATA_TYPE=" + DtToCLDt(dt));
    built_options.emplace("-DCMD_DATA_TYPE=" + DtToCLCMDDt(dt));
    MACE_RETURN_IF_ERROR(runtime->BuildKernel("space_to_depth",
                                              obfuscated_kernel_name,
                                              built_options,
                                              &kernel_));
    kwg_size_ =
        static_cast<uint32_t>(runtime->GetKernelMaxWorkGroupSize(kernel_));
  }

  const uint32_t gws[3] = {static_cast<uint32_t>(input_depth_blocks),
                           static_cast<uint32_t>(input_width),
                           static_cast<uint32_t>(input_height * batch)};
  if (!IsVecEqual(input_shape_, input->shape())) {
    uint32_t idx = 0;
    OUT_OF_RANGE_SET_ARG;
    SET_3D_GWS_ARGS(kernel_);
    kernel_.setArg(idx++, *(input->opencl_image()));
    kernel_.setArg(idx++, static_cast<int32_t>(block_size_));
    kernel_.setArg(idx++, static_cast<int32_t>(input_width));
    kernel_.setArg(idx++, static_cast<int32_t>(input_depth_blocks));
    kernel_.setArg(idx++, static_cast<int32_t>(output_height * batch));
    kernel_.setArg(idx++, static_cast<int32_t>(output_width));
    kernel_.setArg(idx++, static_cast<int32_t>(output_depth_blocks));
    kernel_.setArg(idx++, *(output->opencl_image()));

    input_shape_ = input->shape();
  }

  const std::vector<uint32_t> lws = Default3DLocalWS(runtime, gws, kwg_size_);
  std::string tuning_key = Concat("space_to_depth_opencl_kernel", input->dim(0),
                                  input->dim(1), input->dim(2), input->dim(3));
  MACE_RETURN_IF_ERROR(TuningOrRun3DKernel(runtime, kernel_, tuning_key,
                                           gws, lws, future));

  OUT_OF_RANGE_VALIDATION(kernel_error_);
  return MACE_SUCCESS;
}

template struct SpaceToDepthOpFunctor<DeviceType::GPU, float>;
template struct SpaceToDepthOpFunctor<DeviceType::GPU, half>;

}  // namespace kernels
}  // namespace mace