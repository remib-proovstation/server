// Copyright (c) 2018-2020, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

#ifdef TRITON_ENABLE_METRICS

#include "src/core/metrics.h"

#include <thread>
#include "src/core/constants.h"
#include "src/core/logging.h"

#ifdef TRITON_ENABLE_METRICS_GPU
#include <cuda_runtime_api.h>
#include <nvml.h>
#endif  // TRITON_ENABLE_METRICS_GPU

namespace nvidia { namespace inferenceserver {

Metrics::Metrics()
    : registry_(std::make_shared<prometheus::Registry>()),
      serializer_(new prometheus::TextSerializer()),
      inf_success_family_(
          prometheus::BuildCounter()
              .Name("nv_inference_request_success")
              .Help("Number of successful inference requests, all batch sizes")
              .Register(*registry_)),
      inf_failure_family_(
          prometheus::BuildCounter()
              .Name("nv_inference_request_failure")
              .Help("Number of failed inference requests, all batch sizes")
              .Register(*registry_)),
      inf_count_family_(prometheus::BuildCounter()
                            .Name("nv_inference_count")
                            .Help("Number of inferences performed")
                            .Register(*registry_)),
      inf_count_exec_family_(prometheus::BuildCounter()
                                 .Name("nv_inference_exec_count")
                                 .Help("Number of model executions performed")
                                 .Register(*registry_)),
      inf_request_duration_us_family_(
          prometheus::BuildCounter()
              .Name("nv_inference_request_duration_us")
              .Help("Cummulative inference request duration in microseconds")
              .Register(*registry_)),
      inf_queue_duration_us_family_(
          prometheus::BuildCounter()
              .Name("nv_inference_queue_duration_us")
              .Help("Cummulative inference queuing duration in microseconds")
              .Register(*registry_)),
      inf_compute_input_duration_us_family_(
          prometheus::BuildCounter()
              .Name("nv_inference_compute_input_duration_us")
              .Help("Cummulative compute input duration in microseconds")
              .Register(*registry_)),
      inf_compute_infer_duration_us_family_(
          prometheus::BuildCounter()
              .Name("nv_inference_compute_infer_duration_us")
              .Help("Cummulative compute inference duration in microseconds")
              .Register(*registry_)),
      inf_compute_output_duration_us_family_(
          prometheus::BuildCounter()
              .Name("nv_inference_compute_output_duration_us")
              .Help("Cummulative inference compute output duration in "
                    "microseconds")
              .Register(*registry_)),
#ifdef TRITON_ENABLE_METRICS_GPU
      gpu_utilization_family_(prometheus::BuildGauge()
                                  .Name("nv_gpu_utilization")
                                  .Help("GPU utilization rate [0.0 - 1.0)")
                                  .Register(*registry_)),
      gpu_memory_total_family_(prometheus::BuildGauge()
                                   .Name("nv_gpu_memory_total_bytes")
                                   .Help("GPU total memory, in bytes")
                                   .Register(*registry_)),
      gpu_memory_used_family_(prometheus::BuildGauge()
                                  .Name("nv_gpu_memory_used_bytes")
                                  .Help("GPU used memory, in bytes")
                                  .Register(*registry_)),
      gpu_power_usage_family_(prometheus::BuildGauge()
                                  .Name("nv_gpu_power_usage")
                                  .Help("GPU power usage in watts")
                                  .Register(*registry_)),
      gpu_power_limit_family_(prometheus::BuildGauge()
                                  .Name("nv_gpu_power_limit")
                                  .Help("GPU power management limit in watts")
                                  .Register(*registry_)),
      gpu_energy_consumption_family_(
          prometheus::BuildCounter()
              .Name("nv_energy_consumption")
              .Help("GPU energy consumption in joules since the Triton Server "
                    "started")
              .Register(*registry_)),
#endif  // TRITON_ENABLE_METRICS_GPU
      metrics_enabled_(false), gpu_metrics_enabled_(false)
{
}

Metrics::~Metrics()
{
#ifdef TRITON_ENABLE_METRICS_GPU
  // Signal the nvml thread to exit and then wait for it...
  if (nvml_thread_ != nullptr) {
    nvml_thread_exit_.store(true);
    nvml_thread_->join();
  }
#endif  // TRITON_ENABLE_METRICS_GPU
}

bool
Metrics::Enabled()
{
  auto singleton = GetSingleton();
  return singleton->metrics_enabled_;
}

void
Metrics::EnableMetrics()
{
  auto singleton = GetSingleton();
  singleton->metrics_enabled_ = true;
}

void
Metrics::EnableGPUMetrics()
{
  auto singleton = GetSingleton();

  // Ensure thread-safe enabling of GPU Metrics
  std::lock_guard<std::mutex> lock(singleton->gpu_metrics_enabling_);
  if (singleton->gpu_metrics_enabled_) {
    return;
  }

  if (std::getenv("TRITON_SERVER_CPU_ONLY") == nullptr) {
    singleton->InitializeNvmlMetrics();
  }

  singleton->gpu_metrics_enabled_ = true;
}

bool
Metrics::InitializeNvmlMetrics()
{
#ifndef TRITON_ENABLE_METRICS_GPU
  return false;
#else
  nvmlReturn_t nvmlerr = nvmlInit();
  if (nvmlerr != NVML_SUCCESS) {
    LOG_WARNING << "failed to initialize, GPU metrics will not be available: "
                << nvmlErrorString(nvmlerr);
    return false;
  }

  int dcnt;
  cudaError_t cudaerr = cudaGetDeviceCount(&dcnt);
  if (cudaerr != cudaSuccess) {
    LOG_WARNING
        << "failed to get device count, GPU metrics will not be available: "
        << cudaGetErrorString(cudaerr);
    return false;
  }

  // Create NVML metrics for each GPU
  for (int didx = 0; didx < dcnt; ++didx) {
    // Get handle for the GPU
    cudaDeviceProp gpu_properties;
    cudaerr = cudaGetDeviceProperties(&gpu_properties, didx);
    if (cudaerr != cudaSuccess) {
      LOG_WARNING << "failed to get device properties for device " << didx
                  << ", GPU metrics will not be available for this device: "
                  << cudaGetErrorString(cudaerr);
      continue;
    }

    char pcibusid_str[64];
    cudaerr =
        cudaDeviceGetPCIBusId(pcibusid_str, sizeof(pcibusid_str) - 1, didx);
    if (cudaerr != cudaSuccess) {
      LOG_WARNING << "failed to get Bus ID for device " << didx
                  << ", GPU metrics will not be available for this device: "
                  << cudaGetErrorString(cudaerr);
      continue;
    }

    nvmlDevice_t gpu;
    nvmlReturn_t nvmlerr = nvmlDeviceGetHandleByPciBusId_v2(pcibusid_str, &gpu);
    if (nvmlerr != NVML_SUCCESS) {
      LOG_WARNING << "failed to get device from Bus ID "
                  << ", GPU metrics will not be available for this device: "
                  << nvmlErrorString(nvmlerr);
      continue;
    }

    char gpu_name[NVML_DEVICE_NAME_BUFFER_SIZE + 1];
    nvmlerr = nvmlDeviceGetName(gpu, gpu_name, NVML_DEVICE_NAME_BUFFER_SIZE);
    if (nvmlerr == NVML_SUCCESS) {
      LOG_INFO << "Collecting metrics for GPU " << didx << ": " << gpu_name;
    } else {
      LOG_INFO << "Collecting metrics for GPU " << didx;
    }

    std::string uuid;
    char uuid_str[NVML_DEVICE_UUID_BUFFER_SIZE + 1];
    nvmlerr = nvmlDeviceGetUUID(gpu, uuid_str, NVML_DEVICE_UUID_BUFFER_SIZE);
    if (nvmlerr == NVML_SUCCESS) {
      uuid = uuid_str;
    } else {
      uuid = "unknown";
    }

    std::map<std::string, std::string> gpu_labels;
    gpu_labels.insert(std::map<std::string, std::string>::value_type(
        kMetricsLabelGpuUuid, uuid));

    gpu_utilization_.push_back(&gpu_utilization_family_.Add(gpu_labels));
    gpu_memory_total_.push_back(&gpu_memory_total_family_.Add(gpu_labels));
    gpu_memory_used_.push_back(&gpu_memory_used_family_.Add(gpu_labels));
    gpu_power_usage_.push_back(&gpu_power_usage_family_.Add(gpu_labels));
    gpu_power_limit_.push_back(&gpu_power_limit_family_.Add(gpu_labels));
    gpu_energy_consumption_.push_back(
        &gpu_energy_consumption_family_.Add(gpu_labels));
    nvml_device_.emplace_back(gpu);
  }

  // Update the device count. Some devices may have problems using NVML/CUDA API
  // and thus device count needs to be updated.
  dcnt = nvml_device_.size();

  // Periodically send the NVML metrics...
  if (dcnt > 0) {
    nvml_thread_exit_.store(false);
    nvml_thread_.reset(new std::thread([this, dcnt] {
      // Stop attempting any metric the fails multiple consecutive
      // times for a device.
      constexpr int fail_threshold = 3;
      std::vector<int> power_limit_fail_cnt(dcnt);
      std::vector<int> power_usage_fail_cnt(dcnt);
      std::vector<int> energy_fail_cnt(dcnt);
      std::vector<int> util_fail_cnt(dcnt);
      std::vector<int> mem_fail_cnt(dcnt);

      unsigned long long last_energy[dcnt];
      for (int didx = 0; didx < dcnt; ++didx) {
        last_energy[didx] = 0;
      }

      while (!nvml_thread_exit_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));

        for (int didx = 0; didx < dcnt; ++didx) {
          nvmlDevice_t gpu = nvml_device_[didx];

          // Power limit
          if (power_limit_fail_cnt[didx] < fail_threshold) {
            unsigned int power_limit;
            nvmlReturn_t nvmlerr =
                nvmlDeviceGetPowerManagementLimit(gpu, &power_limit);
            if (nvmlerr == NVML_SUCCESS) {
              power_limit_fail_cnt[didx] = 0;
            } else {
              LOG_WARNING << "failed to get power limit for GPU " << didx
                          << ": " << nvmlErrorString(nvmlerr);
              power_limit = 0;
              power_limit_fail_cnt[didx]++;
            }
            gpu_power_limit_[didx]->Set((double)power_limit * 0.001);
          }

          // Power usage
          if (power_usage_fail_cnt[didx] < fail_threshold) {
            unsigned int power_usage;
            nvmlReturn_t nvmlerr = nvmlDeviceGetPowerUsage(gpu, &power_usage);
            if (nvmlerr == NVML_SUCCESS) {
              power_usage_fail_cnt[didx] = 0;
            } else {
              LOG_WARNING << "failed to get power usage for GPU " << didx
                          << ": " << nvmlErrorString(nvmlerr);
              power_usage = 0;
              power_usage_fail_cnt[didx]++;
            }
            gpu_power_usage_[didx]->Set((double)power_usage * 0.001);
          }

          // Energy Consumption
          if (energy_fail_cnt[didx] < fail_threshold) {
            unsigned long long energy;
            nvmlReturn_t nvmlerr =
                nvmlDeviceGetTotalEnergyConsumption(gpu, &energy);
            if (nvmlerr == NVML_SUCCESS) {
              energy_fail_cnt[didx] = 0;
              if (last_energy[didx] == 0) {
                last_energy[didx] = energy;
              }
              gpu_energy_consumption_[didx]->Increment(
                  (double)(energy - last_energy[didx]) * 0.001);
              last_energy[didx] = energy;
            } else {
              LOG_WARNING << "failed to get energy consumption for GPU " << didx
                          << ": " << nvmlErrorString(nvmlerr);
              energy_fail_cnt[didx]++;
            }
          }

          // Utilization
          if (util_fail_cnt[didx] < fail_threshold) {
            nvmlUtilization_t util;
            nvmlReturn_t nvmlerr = nvmlDeviceGetUtilizationRates(gpu, &util);
            if (nvmlerr == NVML_SUCCESS) {
              util_fail_cnt[didx] = 0;
            } else {
              LOG_WARNING << "failed to get utilization for GPU " << didx
                          << ": " << nvmlErrorString(nvmlerr);
              util.gpu = 0;
              util_fail_cnt[didx]++;
            }
            gpu_utilization_[didx]->Set((double)util.gpu * 0.01);
          }

          // Memory
          if (mem_fail_cnt[didx] < fail_threshold) {
            nvmlMemory_t mem;
            nvmlReturn_t nvmlerr = nvmlDeviceGetMemoryInfo(gpu, &mem);
            if (nvmlerr == NVML_SUCCESS) {
              mem_fail_cnt[didx] = 0;
            } else {
              LOG_WARNING << "failed to get memory for GPU " << didx << ": "
                          << nvmlErrorString(nvmlerr);
              mem.total = 0;
              mem.used = 0;
              mem_fail_cnt[didx]++;
            }
            gpu_memory_total_[didx]->Set(mem.total);
            gpu_memory_used_[didx]->Set(mem.used);
          }
        }
      }
    }));
  }

  return true;
#endif  // TRITON_ENABLE_METRICS_GPU
}

bool
Metrics::UUIDForCudaDevice(int cuda_device, std::string* uuid)
{
  // If metrics were not initialized then just silently fail since
  // with NVML we can't get the CUDA device (and not worth doing
  // anyway since metrics aren't being reported).
  auto singleton = GetSingleton();
  if (!singleton->gpu_metrics_enabled_) {
    return false;
  }

  // If GPU metrics is not enabled just silently fail.
#ifndef TRITON_ENABLE_METRICS_GPU
  return false;
#else
  char pcibusid_str[64];
  cudaError_t cuerr = cudaDeviceGetPCIBusId(
      pcibusid_str, sizeof(pcibusid_str) - 1, cuda_device);
  if (cuerr != cudaSuccess) {
    LOG_ERROR << "failed to get PCI Bus ID for CUDA device " << cuda_device
              << ": " << cudaGetErrorString(cuerr);
    return false;
  }

  nvmlDevice_t device;
  nvmlReturn_t nvmlerr =
      nvmlDeviceGetHandleByPciBusId_v2(pcibusid_str, &device);
  if (nvmlerr != NVML_SUCCESS) {
    LOG_ERROR << "failed to get device from PCI Bus ID: NVML_ERROR "
              << nvmlErrorString(nvmlerr);
    return false;
  }

  char uuid_str[NVML_DEVICE_UUID_BUFFER_SIZE + 1];
  nvmlerr = nvmlDeviceGetUUID(device, uuid_str, NVML_DEVICE_UUID_BUFFER_SIZE);
  if (nvmlerr != NVML_SUCCESS) {
    LOG_ERROR << "failed to get device UUID: NVML_ERROR "
              << nvmlErrorString(nvmlerr);
    return false;
  }

  *uuid = uuid_str;
  return true;
#endif  // TRITON_ENABLE_METRICS_GPU
}

std::shared_ptr<prometheus::Registry>
Metrics::GetRegistry()
{
  auto singleton = Metrics::GetSingleton();
  return singleton->registry_;
}

const std::string
Metrics::SerializedMetrics()
{
  auto singleton = Metrics::GetSingleton();
  return singleton->serializer_->Serialize(
      singleton->registry_.get()->Collect());
}

Metrics*
Metrics::GetSingleton()
{
  static Metrics singleton;
  return &singleton;
}

}}  // namespace nvidia::inferenceserver

#endif  // TRITON_ENABLE_METRICS
