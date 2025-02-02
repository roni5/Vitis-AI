/*
 * Copyright 2019 Xilinx Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sys/time.h>

#include <chrono>
#include <iostream>
#include <numeric>
#include <vector>
#include <opencv2/opencv.hpp>

#include "xrt/xrt_bo.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_kernel.h"
using namespace std;

typedef struct AcceleratorHandle_t {
  xrt::kernel kernel;
  xrt::device device;
  xrt::run runner;
  xrt::bo input;
  xrt::bo dummy1;
  xrt::bo dummy2;
  xrt::bo output;
  xrt::bo params;
  void *input_m;
  void *dummy1_m;
  void *dummy2_m;
  void *output_m;
  void *params_m;
} AcceleratorHandle;

int preprocess(AcceleratorHandle *accelerator, unsigned char *in_image_data,
               int img_ht, int img_wt, int out_ht, int out_wt,
               uint64_t dpu_input_buf_addr, int no_zcpy) {
  // Input size to transfer
  const int imageToDevice_size = img_wt * img_ht * 3 * sizeof(char);
  // Copy to input BO

  std::memcpy(accelerator->input_m, in_image_data, imageToDevice_size);

  // Send the input imageToDevice data to the device memory
  accelerator->input.sync(xclBOSyncDirection::XCL_BO_SYNC_BO_TO_DEVICE,
                          imageToDevice_size, 0);
  // Invoke accelerator
  if (!no_zcpy)
    accelerator->runner(accelerator->input, accelerator->dummy1,
                        accelerator->dummy2, dpu_input_buf_addr,
                        accelerator->params, img_wt, img_ht, img_wt, out_wt,
                        out_ht, out_wt, out_ht, out_wt, 0, 0);
  else
  {
    accelerator->runner(accelerator->input, accelerator->dummy1,
                        accelerator->dummy2, accelerator->output,
                        accelerator->params, img_wt, img_ht, img_wt, out_wt,
                        out_ht, out_wt, out_ht, out_wt, 0, 0);
  }

  //// Wait for execution to finish
  accelerator->runner.wait();

  if (no_zcpy) {
    // Copy the output data from device to host memory
    const int output_size = out_wt * out_wt* 3 * sizeof(char);
    int8_t *out_data = (int8_t *)dpu_input_buf_addr;
    accelerator->output.sync(xclBOSyncDirection::XCL_BO_SYNC_BO_FROM_DEVICE);
    // Copy to output buffer
    std::memcpy(out_data, accelerator->output_m, output_size);
  }

  return 0;
}

AcceleratorHandle *pp_kernel_init(char *xclbin, vector<float> mean,
                                  float input_scale, int out_ht, int out_wt,
                                  int no_zcpy)

{
  // Device/Card Index on system
  unsigned device_index = 0;

  // Check for devices on the system
  if (device_index >= xclProbe()) {
    throw std::runtime_error("Cannot find device index specified");
    return nullptr;
  }

  // Acquire Device by index
  auto device = xrt::device(device_index);
  // Load XCLBIN
  auto uuid = device.load_xclbin(xclbin);
  // Get Kernel/Pre-Proc CU
  auto preproc_accelerator =
      xrt::kernel(device, uuid.get(), "blobfromimage_accel");

  // Get runner instance from xrt
  auto runner = xrt::run(preproc_accelerator);
  // Create BO for input/output/params

  auto input_mem_grp = preproc_accelerator.group_id(0);
  auto output_mem_grp = preproc_accelerator.group_id(3);
  auto params_mem_grp = preproc_accelerator.group_id(4);
  auto dummy1_grp = preproc_accelerator.group_id(1); 
  auto dummy2_grp = preproc_accelerator.group_id(2);


  // Creating memory for 4K image
  const int in_image_size = 3840 * 2160 * 3 * sizeof(char);

  auto input = xrt::bo(device, in_image_size, input_mem_grp);

  void *input_m = input.map();
  if (input_m == nullptr)
    throw std::runtime_error("[ERRR] Input pointer is invalid\n");

  auto dummy1 = xrt::bo(device, sizeof(float), dummy1_grp);

  void *dummy1_m = dummy1.map();
  if (dummy1_m == nullptr)
    throw std::runtime_error("[ERRR] Input pointer is invalid\n");

  auto dummy2 = xrt::bo(device, sizeof(float), dummy2_grp);

  void *dummy2_m = dummy2.map();
  if (dummy2_m == nullptr)
    throw std::runtime_error("[ERRR] Input pointer is invalid\n");

  // Create memory for params
  const int params_size = 6 * sizeof(float);

  auto params = xrt::bo(device, params_size, params_mem_grp);

  void *params_m = params.map();
  if (params_m == nullptr)
    throw std::runtime_error("[ERRR] Params pointer is invalid\n");

  float params_local[6];
  //# Mean params
  params_local[0] = mean[0];
  params_local[1] = mean[1];
  params_local[2] = mean[2];
  //# Input scale
  params_local[3] = params_local[4] = params_local[5] = input_scale;
  //# Set to default zero

  // Copy to params BO
  std::memcpy(params_m, params_local, params_size);
  // Send the params data to device memory
  params.sync(xclBOSyncDirection::XCL_BO_SYNC_BO_TO_DEVICE);

  float xx = 1;
  float *yy = &xx;

  std::memcpy(dummy1_m, yy, sizeof(float));
  std::memcpy(dummy2_m, yy, sizeof(float));

  dummy1.sync(xclBOSyncDirection::XCL_BO_SYNC_BO_TO_DEVICE, sizeof(float), 0);
  dummy2.sync(xclBOSyncDirection::XCL_BO_SYNC_BO_TO_DEVICE, sizeof(float), 0);

  // Return accelerator handle
  auto accel_handle = new AcceleratorHandle();

  if (no_zcpy) {
    // Create memory for output image
    const int out_image_size = out_ht * out_wt * 3 * sizeof(char);
    auto output = xrt::bo(device, out_image_size, output_mem_grp);
    void *output_m = output.map();
    if (output_m == nullptr)
      throw std::runtime_error("[ERRR] Output pointer is invalid\n");
    accel_handle->output = std::move(output);
    accel_handle->output_m = output_m;
  }

  accel_handle->kernel = std::move(preproc_accelerator);
  accel_handle->device = std::move(device);
  accel_handle->runner = std::move(runner);
  accel_handle->input = std::move(input);
  accel_handle->dummy1 = std::move(dummy1);
  accel_handle->dummy2 = std::move(dummy2);
  accel_handle->params = std::move(params);
  accel_handle->input_m = input_m;
  accel_handle->dummy1_m = dummy1_m;
  accel_handle->dummy2_m = dummy2_m;
  accel_handle->params_m = params_m;

  // Return accelerator handle
  return accel_handle;
}

