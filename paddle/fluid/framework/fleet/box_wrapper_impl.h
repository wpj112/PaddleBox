/* Copyright (c) 2020 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */
#pragma once
#ifdef PADDLE_WITH_BOX_PS
#include <glog/logging.h>
#include <vector>

DECLARE_bool(enable_pullpush_dedup_keys);

namespace paddle {
namespace framework {

template <typename FEATURE_VALUE_GPU_TYPE>
void BoxWrapper::PullSparseCase(const paddle::platform::Place& place,
                                const std::vector<const uint64_t*>& keys,
                                const std::vector<float*>& values,
                                const std::vector<int64_t>& slot_lengths,
                                const int hidden_size,
                                const int expand_embed_dim) {
  if (!platform::is_gpu_place(place)) {
    PADDLE_THROW(platform::errors::Unimplemented(
        "Warning:: CPUPlace is not supported in PaddleBox now."));
    return;
  }
  //  VLOG(3) << "Begin PullSparse";
  int device_id = BOOST_GET_CONST(platform::CUDAPlace, place).GetDeviceId();
  DeviceBoxData& dev = device_caches_[device_id];
  platform::Timer& all_timer = dev.all_pull_timer;
  platform::Timer& pull_boxps_timer = dev.boxps_pull_timer;
  all_timer.Resume();

  // construct slot_level lod info
  std::vector<int64_t> slot_lengths_lod;
  slot_lengths_lod.push_back(0);

  int64_t total_length = 0;
  int slot_num = static_cast<int>(slot_lengths.size());
  for (int i = 0; i < slot_num; i++) {
    total_length += slot_lengths[i];
    slot_lengths_lod.push_back(total_length);
  }
  dev.total_key_length = total_length;

  auto ctx = platform::DeviceContextPool::Instance().Get(
      BOOST_GET_CONST(platform::CUDAPlace, place));
  auto stream = dynamic_cast<platform::CUDADeviceContext*>(ctx)->stream();

  uint64_t* total_keys = nullptr;
  int* key2slot = nullptr;
  if (FLAGS_enable_pullpush_dedup_keys) {
    total_keys =
        reinterpret_cast<uint64_t*>(dev.keys_tensor.mutable_data<int64_t>(
            {static_cast<int64_t>(total_length * 2), 1}, place));
    key2slot = reinterpret_cast<int*>(dev.keys2slot.mutable_data<int>(
        {static_cast<int64_t>(total_length * 5), 1}, place));
  } else {
    total_keys = reinterpret_cast<uint64_t*>(
        dev.keys_tensor.mutable_data<int64_t>({total_length, 1}, place));
    key2slot = reinterpret_cast<int*>(
        dev.keys2slot.mutable_data<int>({total_length, 1}, place));
  }

  int* total_dims = reinterpret_cast<int*>(
      dev.dims_tensor.mutable_data<int>({total_length, 1}, place));

  uint64_t** gpu_keys = dev.keys_ptr_tensor.mutable_data<uint64_t*>(
      static_cast<int>(keys.size() * sizeof(uint64_t*)), place);

  int64_t* slot_lens = reinterpret_cast<int64_t*>(
      dev.slot_lens.mutable_data<int64_t>({(slot_num + 1), 1}, place));
  cudaMemcpyAsync(gpu_keys, keys.data(), keys.size() * sizeof(uint64_t*),
                  cudaMemcpyHostToDevice, stream);
  cudaMemcpyAsync(slot_lens, slot_lengths_lod.data(),
                  slot_lengths_lod.size() * sizeof(int64_t),
                  cudaMemcpyHostToDevice, stream);
  this->CopyKeys(place, gpu_keys, total_keys, slot_lens, slot_num,
                 static_cast<int>(total_length), key2slot);

  // dedup keys pull
  if (FLAGS_enable_pullpush_dedup_keys) {
    uint32_t* d_restore_idx =
        reinterpret_cast<uint32_t*>(&key2slot[total_length]);
    uint32_t* d_sorted_idx =
        reinterpret_cast<uint32_t*>(&d_restore_idx[total_length]);
    uint32_t* d_offset =
        reinterpret_cast<uint32_t*>(&d_sorted_idx[total_length]);
    uint32_t* d_merged_cnts =
        reinterpret_cast<uint32_t*>(&d_offset[total_length]);
    uint64_t* d_merged_keys =
        reinterpret_cast<uint64_t*>(&total_keys[total_length]);
    int dedup_size =
        boxps_ptr_->DedupKeysAndFillIdx(device_id, total_length,
                                        total_keys,     // input
                                        d_merged_keys,  // output
                                        d_restore_idx,  // pull fill idx
                                        d_sorted_idx,   // sort old idx
                                        d_offset,       // offset
                                        d_merged_cnts);
    PADDLE_ENFORCE_GT(dedup_size, 0,
                      platform::errors::PreconditionNotMet(
                          "dedup keys need more than zero failed in BoxPS."));
    dev.dedup_key_length = dedup_size;

    int64_t total_bytes = dedup_size * sizeof(FEATURE_VALUE_GPU_TYPE);
    FEATURE_VALUE_GPU_TYPE* total_values_gpu =
        dev.pull_push_tensor.mutable_data<FEATURE_VALUE_GPU_TYPE>(total_bytes,
                                                                  place);

    pull_boxps_timer.Resume();

    int ret = boxps_ptr_->PullSparseGPU(
        d_merged_keys, reinterpret_cast<void*>(total_values_gpu),
        static_cast<int>(dedup_size), device_id);
    PADDLE_ENFORCE_EQ(ret, 0, platform::errors::PreconditionNotMet(
                                  "PullSparseGPU failed in BoxPS."));
    pull_boxps_timer.Pause();
    // values.size() not sure equal slot_num
    float** gpu_values = dev.values_ptr_tensor.mutable_data<float*>(
        static_cast<int>(values.size() * sizeof(float*)), place);
    cudaMemcpyAsync(gpu_values, values.data(), values.size() * sizeof(float*),
                    cudaMemcpyHostToDevice, stream);

    this->CopyForPull(place, gpu_keys, gpu_values, total_values_gpu, slot_lens,
                      slot_num, key2slot, hidden_size, expand_embed_dim,
                      total_length, total_dims, d_restore_idx);
  } else {
    int64_t total_bytes = total_length * sizeof(FEATURE_VALUE_GPU_TYPE);
    FEATURE_VALUE_GPU_TYPE* total_values_gpu =
        dev.pull_push_tensor.mutable_data<FEATURE_VALUE_GPU_TYPE>(total_bytes,
                                                                  place);

    pull_boxps_timer.Resume();

    int ret = boxps_ptr_->PullSparseGPU(
        total_keys, reinterpret_cast<void*>(total_values_gpu),
        static_cast<int>(total_length), device_id);
    PADDLE_ENFORCE_EQ(ret, 0, platform::errors::PreconditionNotMet(
                                  "PullSparseGPU failed in BoxPS."));
    pull_boxps_timer.Pause();
    // values.size() not sure equal slot_num
    float** gpu_values = dev.values_ptr_tensor.mutable_data<float*>(
        static_cast<int>(values.size() * sizeof(float*)), place);
    cudaMemcpyAsync(gpu_values, values.data(), values.size() * sizeof(float*),
                    cudaMemcpyHostToDevice, stream);

    this->CopyForPull(place, gpu_keys, gpu_values, total_values_gpu, slot_lens,
                      slot_num, key2slot, hidden_size, expand_embed_dim,
                      total_length, total_dims);
  }
  all_timer.Pause();
}

template <typename FeaturePushValueGpuType>
void BoxWrapper::PushSparseGradCase(
    const paddle::platform::Place& place,
    const std::vector<const uint64_t*>& keys,
    const std::vector<const float*>& grad_values,
    const std::vector<int64_t>& slot_lengths, const int hidden_size,
    const int expand_embed_dim, const int batch_size) {
  if (!platform::is_gpu_place(place)) {
    PADDLE_THROW(platform::errors::Unimplemented(
        "Warning:: CPUPlace is not supported in PaddleBox now."));
    return;
  }
  int device_id = BOOST_GET_CONST(platform::CUDAPlace, place).GetDeviceId();
  DeviceBoxData& dev = device_caches_[device_id];
  platform::Timer& all_timer = dev.all_push_timer;
  platform::Timer& push_boxps_timer = dev.boxps_push_timer;

  all_timer.Resume();

  auto stream = dynamic_cast<platform::CUDADeviceContext*>(
                    platform::DeviceContextPool::Instance().Get(
                        BOOST_GET_CONST(platform::CUDAPlace, place)))
                    ->stream();
  uint64_t* total_keys =
      reinterpret_cast<uint64_t*>(dev.keys_tensor.data<int64_t>());
  int* total_dims = reinterpret_cast<int*>(dev.dims_tensor.data<int>());
  int slot_num = static_cast<int>(slot_lengths.size());
  if (!dev.d_slot_vector.IsInitialized()) {
    int* buf_slot_vector = reinterpret_cast<int*>(
        dev.d_slot_vector.mutable_data<int>({slot_num, 1}, place));
    cudaMemcpyAsync(buf_slot_vector, slot_vector_.data(),
                    slot_num * sizeof(int), cudaMemcpyHostToDevice, stream);
  }

  const int64_t* slot_lens =
      reinterpret_cast<int64_t*>(dev.slot_lens.data<int64_t>());
  const int* d_slot_vector = dev.d_slot_vector.data<int>();
  const int* key2slot = reinterpret_cast<int*>(dev.keys2slot.data<int>());
  float** gpu_values = dev.values_ptr_tensor.data<float*>();
  cudaMemcpyAsync(gpu_values, grad_values.data(),
                  grad_values.size() * sizeof(float*), cudaMemcpyHostToDevice,
                  stream);

  int64_t total_length = dev.total_key_length;
  // dedup keys pull
  if (FLAGS_enable_pullpush_dedup_keys) {
    const uint32_t* d_sorted_idx =
        reinterpret_cast<const uint32_t*>(&key2slot[total_length * 2]);
    const uint32_t* d_offset =
        reinterpret_cast<const uint32_t*>(&d_sorted_idx[total_length]);
    const uint32_t* d_merged_cnts =
        reinterpret_cast<const uint32_t*>(&d_offset[total_length]);
    uint64_t* d_merged_keys = &total_keys[total_length];

    int64_t dedup_size = dev.dedup_key_length;
    int64_t total_bytes = dedup_size * sizeof(FeaturePushValueGpuType);
    FeaturePushValueGpuType* total_grad_values_gpu =
        dev.pull_push_tensor.mutable_data<FeaturePushValueGpuType>(total_bytes,
                                                                   place);
    this->CopyForPush(place, gpu_values, total_grad_values_gpu, d_slot_vector,
                      slot_lens, slot_num, hidden_size, expand_embed_dim,
                      dedup_size, batch_size, total_dims, key2slot,
                      d_sorted_idx, d_offset, d_merged_cnts);

    push_boxps_timer.Resume();
    int ret = boxps_ptr_->PushSparseGPU(
        d_merged_keys, reinterpret_cast<void*>(total_grad_values_gpu),
        static_cast<int>(dedup_size), device_id);
    PADDLE_ENFORCE_EQ(ret, 0, platform::errors::PreconditionNotMet(
                                  "PushSparseGPU failed in BoxPS."));
    push_boxps_timer.Pause();
  } else {
    int64_t total_bytes = total_length * sizeof(FeaturePushValueGpuType);
    FeaturePushValueGpuType* total_grad_values_gpu =
        dev.pull_push_tensor.mutable_data<FeaturePushValueGpuType>(total_bytes,
                                                                   place);
    this->CopyForPush(place, gpu_values, total_grad_values_gpu, d_slot_vector,
                      slot_lens, slot_num, hidden_size, expand_embed_dim,
                      total_length, batch_size, total_dims, key2slot);

    push_boxps_timer.Resume();
    int ret = boxps_ptr_->PushSparseGPU(
        total_keys, reinterpret_cast<void*>(total_grad_values_gpu),
        static_cast<int>(total_length), device_id);
    PADDLE_ENFORCE_EQ(ret, 0, platform::errors::PreconditionNotMet(
                                  "PushSparseGPU failed in BoxPS."));
    push_boxps_timer.Pause();
  }
  all_timer.Pause();
}

}  // namespace framework
}  // namespace paddle
#endif
