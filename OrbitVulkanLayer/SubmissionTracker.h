// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ORBIT_VULKAN_LAYER_COMMAND_BUFFER_MANAGER_H_
#define ORBIT_VULKAN_LAYER_COMMAND_BUFFER_MANAGER_H_

#include <stack>

#include "OrbitBase/Logging.h"
#include "OrbitBase/Profiling.h"
#include "VulkanLayerProducer.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/synchronization/mutex.h"
#include "vulkan/vulkan.h"

namespace orbit_vulkan_layer {

namespace internal {

struct SubmissionMetaInformation {
  uint64_t pre_submission_cpu_timestamp;
  uint64_t post_submission_cpu_timestamp;
  int32_t thread_id;
};

struct SubmittedCommandBuffer {
  std::optional<uint32_t> command_buffer_begin_slot_index;
  uint32_t command_buffer_end_slot_index;
};

struct SubmittedMarker {
  SubmissionMetaInformation meta_information;
  uint32_t slot_index;
};

struct Color {
  // Values are all in range [0.f, 1.f.]
  float red;
  float green;
  float blue;
  float alpha;
};

struct MarkerState {
  std::optional<SubmittedMarker> begin_info;
  std::optional<SubmittedMarker> end_info;
  std::string text;
  Color color;
  size_t depth;
  bool cut_off;
};

struct SubmitInfo {
  std::vector<SubmittedCommandBuffer> command_buffers;
};

struct QueueSubmission {
  SubmissionMetaInformation meta_information;
  std::vector<SubmitInfo> submit_infos;
  std::vector<MarkerState> completed_markers;
  uint32_t num_begin_markers = 0;
};

}  // namespace internal

/*
 * This class ultimately is responsible to track command buffer and debug marker timings.
 * To do so, it keeps tracks of command-buffer allocations, destruction, begins, ends as well as
 * submissions.
 * On `VkBeginCommandBuffer` and `VkEndCommandBuffer` it can (if capturing) insert write timestamp
 * commands (`VkCmdWriteTimestamp`). The same is done for debug marker begins and ends. All that
 * data will be gathered together at a queue submission (`VkQueueSubmit`).
 *
 * Upon every `VkQueuePresentKHR` it will check if the timestamps of a certain submission are
 * already available, and if so, it will send the results over to the `VulkanLayerProducer`.
 *
 * See also `DispatchTable` (for vulkan dispatch), `TimerQueryPool` (to manage the timestamp slots),
 * and `DeviceManager` (to retrieve device properties).
 *
 * Thread-Safety: This class is internally synchronized (using read/write locks), and can be
 * safely accessed from different threads.
 */
template <class DispatchTable, class DeviceManager, class TimerQueryPool>
class SubmissionTracker : public VulkanLayerProducer::CaptureStatusListener {
 public:
  explicit SubmissionTracker(uint32_t max_local_marker_depth_per_command_buffer,
                             DispatchTable* dispatch_table, TimerQueryPool* timer_query_pool,
                             DeviceManager* device_manager,
                             std::unique_ptr<VulkanLayerProducer>* vulkan_layer_producer)
      : max_local_marker_depth_per_command_buffer_(max_local_marker_depth_per_command_buffer),
        dispatch_table_(dispatch_table),
        timer_query_pool_(timer_query_pool),
        device_manager_(device_manager),
        vulkan_layer_producer_{vulkan_layer_producer} {
    CHECK(dispatch_table_ != nullptr);
    CHECK(timer_query_pool_ != nullptr);
    CHECK(device_manager_ != nullptr);
    CHECK(vulkan_layer_producer_ != nullptr);
  }

  void TrackCommandBuffers(VkDevice device, VkCommandPool pool,
                           const VkCommandBuffer* command_buffers, uint32_t count) {
    absl::WriterMutexLock lock(&mutex_);
    if (!pool_to_command_buffers_.contains(pool)) {
      pool_to_command_buffers_[pool] = {};
    }
    absl::flat_hash_set<VkCommandBuffer>& associated_cbs = pool_to_command_buffers_.at(pool);
    for (uint32_t i = 0; i < count; ++i) {
      VkCommandBuffer cb = command_buffers[i];
      associated_cbs.insert(cb);
      command_buffer_to_device_[cb] = device;
    }
  }

  void UntrackCommandBuffers(VkDevice device, VkCommandPool pool,
                             const VkCommandBuffer* command_buffers, uint32_t count) {
    absl::WriterMutexLock lock(&mutex_);
    CHECK(pool_to_command_buffers_.contains(pool));
    absl::flat_hash_set<VkCommandBuffer>& associated_command_buffers =
        pool_to_command_buffers_.at(pool);
    for (uint32_t i = 0; i < count; ++i) {
      VkCommandBuffer command_buffer = command_buffers[i];
      associated_command_buffers.erase(command_buffer);
      CHECK(command_buffer_to_device_.contains(command_buffer));
      CHECK(command_buffer_to_device_.at(command_buffer) == device);
      command_buffer_to_device_.erase(command_buffer);
    }
    if (associated_command_buffers.empty()) {
      pool_to_command_buffers_.erase(pool);
    }
  }

  void MarkCommandBufferBegin(VkCommandBuffer command_buffer) {
    // Even when we are not capturing we create state for this command buffer to allow the
    // debug marker tracking.
    {
      absl::WriterMutexLock lock(&mutex_);
      CHECK(!command_buffer_to_state_.contains(command_buffer));
      command_buffer_to_state_[command_buffer] = {};
    }
    if (!IsCapturing()) {
      return;
    }

    uint32_t slot_index = RecordTimestamp(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    {
      absl::WriterMutexLock lock(&mutex_);
      CHECK(command_buffer_to_state_.contains(command_buffer));
      command_buffer_to_state_.at(command_buffer).command_buffer_begin_slot_index =
          std::make_optional(slot_index);
    }
  }

  void MarkCommandBufferEnd(VkCommandBuffer command_buffer) {
    if (!IsCapturing()) {
      return;
    }

    uint32_t slot_index = RecordTimestamp(command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    {
      absl::ReaderMutexLock lock(&mutex_);
      CHECK(command_buffer_to_state_.contains(command_buffer));
      CommandBufferState& command_buffer_state = command_buffer_to_state_.at(command_buffer);
      // Writing to this field is safe, as there can't be any operation on this command buffer
      // in parallel.
      command_buffer_state.command_buffer_end_slot_index = std::make_optional(slot_index);
    }
  }

  void MarkDebugMarkerBegin(VkCommandBuffer command_buffer, const char* text,
                            internal::Color color) {
    CHECK(text != nullptr);
    bool too_many_markers;
    {
      absl::WriterMutexLock lock(&mutex_);
      CHECK(command_buffer_to_state_.contains(command_buffer));
      CommandBufferState& state = command_buffer_to_state_.at(command_buffer);
      ++state.local_marker_stack_size;
      too_many_markers = max_local_marker_depth_per_command_buffer_ > 0 &&
                         state.local_marker_stack_size > max_local_marker_depth_per_command_buffer_;
      Marker marker{.type = MarkerType::kDebugMarkerBegin,
                    .text = std::string(text),
                    .color = color,
                    .cut_off = too_many_markers};
      state.markers.emplace_back(std::move(marker));
    }

    if (!IsCapturing() || too_many_markers) {
      return;
    }

    uint32_t slot_index = RecordTimestamp(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

    {
      absl::WriterMutexLock lock(&mutex_);
      CHECK(command_buffer_to_state_.contains(command_buffer));
      CommandBufferState& state = command_buffer_to_state_.at(command_buffer);
      state.markers.back().slot_index = std::make_optional(slot_index);
    }
  }
  void MarkDebugMarkerEnd(VkCommandBuffer command_buffer) {
    bool too_many_markers;
    {
      absl::WriterMutexLock lock(&mutex_);
      CHECK(command_buffer_to_state_.contains(command_buffer));
      CommandBufferState& state = command_buffer_to_state_.at(command_buffer);
      too_many_markers = max_local_marker_depth_per_command_buffer_ > 0 &&
                         state.local_marker_stack_size > max_local_marker_depth_per_command_buffer_;
      Marker marker{.type = MarkerType::kDebugMarkerEnd, .cut_off = too_many_markers};
      state.markers.emplace_back(std::move(marker));
      // We might see more "ends" then "begins", as the "begins" can be on a different command
      // buffer
      if (state.local_marker_stack_size != 0) {
        --state.local_marker_stack_size;
      }
    }

    if (!IsCapturing() || too_many_markers) {
      return;
    }

    uint32_t slot_index = RecordTimestamp(command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    {
      absl::WriterMutexLock lock(&mutex_);
      CHECK(command_buffer_to_state_.contains(command_buffer));
      CommandBufferState& state = command_buffer_to_state_.at(command_buffer);
      state.markers.back().slot_index = std::make_optional(slot_index);
    }
  }
  // After command buffers are submitted into a queue, they can be reused for further operations.
  // Thus, our identification via the pointers become invalid. We will use the vkQueueSubmit
  // to make our data persistent until we have processed the results of the execution of these
  // command buffers (which will be done in the vkQueuePresentKHR).
  // If we are not capturing, that method will do nothing and return `nullopt`.
  // Otherwise, it will create and return an internal::QueueSubmission object, holding the
  // information about all command buffers recorded in this submission.
  // It also takes a timestamp before the execution of the driver code for the submission.
  // This allows us to map submissions from the Vulkan layer to the driver submissions.
  [[nodiscard]] std::optional<internal::QueueSubmission> PersistCommandBuffersOnSubmit(
      uint32_t submit_count, const VkSubmitInfo* submits) {
    if (!IsCapturing()) {
      // `PersistDebugMarkersOnSubmit` and `OnCaptureFinished` will take care of clean up and
      // slot resetting.
      return std::nullopt;
    }

    internal::QueueSubmission queue_submission;
    queue_submission.meta_information.pre_submission_cpu_timestamp = MonotonicTimestampNs();
    queue_submission.meta_information.thread_id = GetCurrentThreadId();

    for (uint32_t submit_index = 0; submit_index < submit_count; ++submit_index) {
      VkSubmitInfo submit_info = submits[submit_index];
      queue_submission.submit_infos.emplace_back();
      internal::SubmitInfo& submitted_submit_info = queue_submission.submit_infos.back();
      for (uint32_t command_buffer_index = 0; command_buffer_index < submit_info.commandBufferCount;
           ++command_buffer_index) {
        VkCommandBuffer command_buffer = submit_info.pCommandBuffers[command_buffer_index];
        CHECK(command_buffer_to_state_.contains(command_buffer));
        CommandBufferState& state = command_buffer_to_state_.at(command_buffer);

        // If we neither recorded the end nor the begin of a command buffer, we have no information
        // to send.
        if (!state.command_buffer_end_slot_index.has_value()) {
          CHECK(!state.command_buffer_end_slot_index.has_value());
          continue;
        }

        internal::SubmittedCommandBuffer submitted_command_buffer{
            .command_buffer_begin_slot_index = state.command_buffer_begin_slot_index,
            .command_buffer_end_slot_index = state.command_buffer_end_slot_index.value()};
        submitted_submit_info.command_buffers.emplace_back(submitted_command_buffer);

        // Remove the slots from the state now, such that `OnCaptureFinished` wont reset the slots
        // twice. Note, that they will be reset in `CompleteSubmits`.
        state.command_buffer_begin_slot_index.reset();
        state.command_buffer_end_slot_index.reset();
      }
    }

    return queue_submission;
  }

  // We assume to be capturing if `queue_submission_optional` has content, i.e. we were capturing
  // on this VkQueueSubmit before calling into the driver.
  // It also takes a timestamp after the execution of the driver code for the submission.
  // This allows us to map submissions from the Vulkan layer to the driver submissions.
  void PersistDebugMarkersOnSubmit(
      VkQueue queue, uint32_t submit_count, const VkSubmitInfo* submits,
      std::optional<internal::QueueSubmission> queue_submission_optional) {
    absl::WriterMutexLock lock(&mutex_);
    if (!queue_to_markers_.contains(queue)) {
      queue_to_markers_[queue] = {};
    }
    CHECK(queue_to_markers_.contains(queue));
    QueueMarkerState& markers = queue_to_markers_.at(queue);

    // If we consider this to still capturing, take a cpu timestamp as "post submission" such that
    // the submission "meta information" is complete. We can then attach that also to each debug
    // marker, as they may be spanned across different submissions.
    if (queue_submission_optional.has_value()) {
      queue_submission_optional->meta_information.post_submission_cpu_timestamp =
          MonotonicTimestampNs();
    }

    std::vector<uint32_t> begin_markers_from_prev_submit_to_reset;
    VkDevice device = VK_NULL_HANDLE;
    for (uint32_t submit_index = 0; submit_index < submit_count; ++submit_index) {
      VkSubmitInfo submit_info = submits[submit_index];
      for (uint32_t command_buffer_index = 0; command_buffer_index < submit_info.commandBufferCount;
           ++command_buffer_index) {
        VkCommandBuffer command_buffer = submit_info.pCommandBuffers[command_buffer_index];
        if (device == VK_NULL_HANDLE) {
          CHECK(command_buffer_to_device_.contains(command_buffer));
          device = command_buffer_to_device_.at(command_buffer);
        }
        CHECK(command_buffer_to_state_.contains(command_buffer));
        CommandBufferState& state = command_buffer_to_state_.at(command_buffer);

        // Debug markers:
        for (const Marker& marker : state.markers) {
          std::optional<internal::SubmittedMarker> submitted_marker = std::nullopt;
          if (marker.slot_index.has_value() && queue_submission_optional.has_value()) {
            submitted_marker = {.meta_information = queue_submission_optional->meta_information,
                                .slot_index = marker.slot_index.value()};
          }

          switch (marker.type) {
            case MarkerType::kDebugMarkerBegin: {
              if (queue_submission_optional.has_value() && marker.slot_index.has_value()) {
                ++queue_submission_optional->num_begin_markers;
              }
              CHECK(marker.text.has_value());
              CHECK(marker.color.has_value());
              internal::MarkerState marker_state{.text = marker.text.value(),
                                                 .color = marker.color.value(),
                                                 .begin_info = submitted_marker,
                                                 .depth = markers.marker_stack.size(),
                                                 .cut_off = marker.cut_off};
              markers.marker_stack.push(std::move(marker_state));
              break;
            }

            case MarkerType::kDebugMarkerEnd: {
              internal::MarkerState marker_state = markers.marker_stack.top();
              markers.marker_stack.pop();

              // If there is a begin marker slot from a previous submission or one that was cut off,
              // this is our chance to reset the slot.
              if (marker_state.begin_info.has_value() &&
                  (!queue_submission_optional.has_value() || marker_state.cut_off)) {
                begin_markers_from_prev_submit_to_reset.push_back(
                    marker_state.begin_info.value().slot_index);
              }

              if (queue_submission_optional.has_value() && marker.slot_index.has_value() &&
                  !marker_state.cut_off) {
                marker_state.end_info = submitted_marker;
                queue_submission_optional->completed_markers.emplace_back(std::move(marker_state));
              }

              break;
            }
          }
        }
        command_buffer_to_state_.erase(command_buffer);
      }
    }

    if (!queue_submission_optional.has_value()) {
      if (!begin_markers_from_prev_submit_to_reset.empty()) {
        timer_query_pool_->ResetQuerySlots(device, begin_markers_from_prev_submit_to_reset);
      }
      return;
    }

    if (!queue_to_submissions_.contains(queue)) {
      queue_to_submissions_[queue] = {};
    }
    CHECK(queue_to_submissions_.contains(queue));
    queue_to_submissions_.at(queue).emplace_back(std::move(queue_submission_optional.value()));
  }

  void CompleteSubmits(VkDevice device) {
    VkQueryPool query_pool = timer_query_pool_->GetQueryPool(device);
    std::vector<internal::QueueSubmission> completed_submissions =
        PullCompletedSubmissions(device, query_pool);

    if (completed_submissions.empty()) {
      return;
    }

    VkPhysicalDevice physical_device = device_manager_->GetPhysicalDeviceOfLogicalDevice(device);
    const float timestamp_period =
        device_manager_->GetPhysicalDeviceProperties(physical_device).limits.timestampPeriod;

    std::vector<uint32_t> query_slots_to_reset = {};
    for (const auto& completed_submission : completed_submissions) {
      orbit_grpc_protos::CaptureEvent capture_event;
      orbit_grpc_protos::GpuQueueSubmission* submission_proto =
          capture_event.mutable_gpu_queue_submission();
      WriteMetaInfo(completed_submission.meta_information, submission_proto->mutable_meta_info());

      WriteCommandBufferTimings(completed_submission, submission_proto, query_slots_to_reset,
                                device, query_pool, timestamp_period);

      // Now for the debug markers:
      WriteDebugMarkers(completed_submission, submission_proto, query_slots_to_reset, device,
                        query_pool, timestamp_period);

      (*vulkan_layer_producer_)->EnqueueCaptureEvent(std::move(capture_event));
    }

    timer_query_pool_->ResetQuerySlots(device, query_slots_to_reset);
  }

  void ResetCommandBuffer(VkCommandBuffer command_buffer) {
    absl::WriterMutexLock lock(&mutex_);
    if (!command_buffer_to_state_.contains(command_buffer)) {
      return;
    }
    CHECK(command_buffer_to_state_.contains(command_buffer));
    CommandBufferState& state = command_buffer_to_state_.at(command_buffer);
    CHECK(command_buffer_to_device_.contains(command_buffer));
    VkDevice device = command_buffer_to_device_.at(command_buffer);
    std::vector<uint32_t> marker_slots_to_rollback = {};
    if (state.command_buffer_begin_slot_index.has_value()) {
      marker_slots_to_rollback.push_back(state.command_buffer_begin_slot_index.value());
    }
    if (state.command_buffer_end_slot_index.has_value()) {
      marker_slots_to_rollback.push_back(state.command_buffer_end_slot_index.value());
    }
    for (const Marker& marker : state.markers) {
      if (marker.slot_index.has_value()) {
        marker_slots_to_rollback.push_back(marker.slot_index.value());
      }
    }
    timer_query_pool_->RollbackPendingQuerySlots(device, marker_slots_to_rollback);

    command_buffer_to_state_.erase(command_buffer);
  }

  void ResetCommandPool(VkCommandPool command_pool) {
    absl::flat_hash_set<VkCommandBuffer> command_buffers;
    {
      absl::ReaderMutexLock lock(&mutex_);
      if (!pool_to_command_buffers_.contains(command_pool)) {
        return;
      }
      CHECK(pool_to_command_buffers_.contains(command_pool));
      command_buffers = pool_to_command_buffers_.at(command_pool);
    }
    for (const auto& command_buffer : command_buffers) {
      ResetCommandBuffer(command_buffer);
    }
  }

  void OnCaptureStart() override {}

  void OnCaptureStop() override {}

  void OnCaptureFinished() override {
    absl::WriterMutexLock lock(&mutex_);
    std::vector<uint32_t> slots_to_reset;

    VkDevice device = VK_NULL_HANDLE;

    for (auto& [command_buffer, command_buffer_state] : command_buffer_to_state_) {
      if (device == VK_NULL_HANDLE) {
        CHECK(command_buffer_to_device_.contains(command_buffer));
        device = command_buffer_to_device_.at(command_buffer);
      }
      if (command_buffer_state.command_buffer_begin_slot_index.has_value()) {
        slots_to_reset.push_back(command_buffer_state.command_buffer_begin_slot_index.value());
        command_buffer_state.command_buffer_begin_slot_index.reset();
      }

      if (command_buffer_state.command_buffer_end_slot_index.has_value()) {
        slots_to_reset.push_back(command_buffer_state.command_buffer_end_slot_index.value());
        command_buffer_state.command_buffer_end_slot_index.reset();
      }

      for (Marker& marker : command_buffer_state.markers) {
        if (marker.slot_index.has_value()) {
          slots_to_reset.push_back(marker.slot_index.value());
          marker.slot_index.reset();
        }
      }
    }
    if (!slots_to_reset.empty()) {
      timer_query_pool_->ResetQuerySlots(device, slots_to_reset);
    }
  }

 private:
  enum class MarkerType { kDebugMarkerBegin = 0, kDebugMarkerEnd };

  struct Marker {
    MarkerType type;
    std::optional<uint32_t> slot_index;
    std::optional<std::string> text;
    std::optional<internal::Color> color;
    bool cut_off;
  };

  struct QueueMarkerState {
    std::stack<internal::MarkerState> marker_stack;
  };

  struct CommandBufferState {
    std::optional<uint32_t> command_buffer_begin_slot_index;
    std::optional<uint32_t> command_buffer_end_slot_index;
    std::vector<Marker> markers;
    uint32_t local_marker_stack_size;
  };

  uint32_t RecordTimestamp(VkCommandBuffer command_buffer,
                           VkPipelineStageFlagBits pipeline_stage_flags) {
    VkDevice device;
    {
      absl::ReaderMutexLock lock(&mutex_);
      CHECK(command_buffer_to_device_.contains(command_buffer));
      device = command_buffer_to_device_.at(command_buffer);
    }

    VkQueryPool query_pool = timer_query_pool_->GetQueryPool(device);

    uint32_t slot_index;
    bool found_slot = timer_query_pool_->NextReadyQuerySlot(device, &slot_index);
    CHECK(found_slot);
    dispatch_table_->CmdWriteTimestamp(command_buffer)(command_buffer, pipeline_stage_flags,
                                                       query_pool, slot_index);

    return slot_index;
  }

  std::vector<internal::QueueSubmission> PullCompletedSubmissions(VkDevice device,
                                                                  VkQueryPool query_pool) {
    std::vector<internal::QueueSubmission> completed_submissions = {};

    absl::WriterMutexLock lock(&mutex_);
    for (auto& [unused_queue, queue_submissions] : queue_to_submissions_) {
      auto submission_it = queue_submissions.begin();
      while (submission_it != queue_submissions.end()) {
        const internal::QueueSubmission& submission = *submission_it;
        if (submission.submit_infos.empty()) {
          submission_it = queue_submissions.erase(submission_it);
          continue;
        }

        bool erase_submission = true;
        // Let's find the last command buffer in this submission, so first find the last
        // submit info that has at least one command buffer.
        // We test if for this command buffer, we already have a query result for its last slot
        // and if so (or if the submission does not contain any command buffer) erase this
        // submission.
        auto submit_info_reverse_it = submission.submit_infos.rbegin();
        while (submit_info_reverse_it != submission.submit_infos.rend()) {
          const internal::SubmitInfo& submit_info = submission.submit_infos.back();
          if (submit_info.command_buffers.empty()) {
            ++submit_info_reverse_it;
            continue;
          }
          // We found our last command buffer, so lets check if its result is there:
          const internal::SubmittedCommandBuffer& last_command_buffer =
              submit_info.command_buffers.back();
          uint32_t check_slot_index_end = last_command_buffer.command_buffer_end_slot_index;

          static constexpr VkDeviceSize kResultStride = sizeof(uint64_t);
          uint64_t test_query_result = 0;
          VkResult query_worked = dispatch_table_->GetQueryPoolResults(device)(
              device, query_pool, check_slot_index_end, 1, sizeof(test_query_result),
              &test_query_result, kResultStride, VK_QUERY_RESULT_64_BIT);

          // Only erase the submission if we query its timers now.
          if (query_worked == VK_SUCCESS) {
            erase_submission = true;
            completed_submissions.push_back(submission);
          } else {
            erase_submission = false;
          }
          break;
        }

        if (erase_submission) {
          submission_it = queue_submissions.erase(submission_it);
        } else {
          ++submission_it;
        }
      }
    }

    return completed_submissions;
  }

  uint64_t QueryGpuTimestampNs(VkDevice device, VkQueryPool query_pool, uint32_t slot_index,
                               float timestamp_period) {
    static constexpr VkDeviceSize kResultStride = sizeof(uint64_t);

    uint64_t timestamp = 0;
    VkResult result_status = dispatch_table_->GetQueryPoolResults(device)(
        device, query_pool, slot_index, 1, sizeof(timestamp), &timestamp, kResultStride,
        VK_QUERY_RESULT_64_BIT);
    // TODO: That should be rather an if and if it is false, we need to push back the submission for
    // later
    CHECK(result_status == VK_SUCCESS);

    return static_cast<uint64_t>(static_cast<double>(timestamp) * timestamp_period);
  }

  static void WriteMetaInfo(const internal::SubmissionMetaInformation& meta_info,
                            orbit_grpc_protos::GpuQueueSubmissionMetaInfo* target_proto) {
    target_proto->set_tid(meta_info.thread_id);
    target_proto->set_pre_submission_cpu_timestamp(meta_info.pre_submission_cpu_timestamp);
    target_proto->set_post_submission_cpu_timestamp(meta_info.post_submission_cpu_timestamp);
  }

  void WriteCommandBufferTimings(const internal::QueueSubmission& completed_submission,
                                 orbit_grpc_protos::GpuQueueSubmission* submission_proto,
                                 std::vector<uint32_t>& query_slots_to_reset, VkDevice device,
                                 VkQueryPool query_pool, float timestamp_period) {
    for (const auto& completed_submit : completed_submission.submit_infos) {
      orbit_grpc_protos::GpuSubmitInfo* submit_info_proto = submission_proto->add_submit_infos();
      for (const auto& completed_command_buffer : completed_submit.command_buffers) {
        orbit_grpc_protos::GpuCommandBuffer* command_buffer_proto =
            submit_info_proto->add_command_buffers();

        if (completed_command_buffer.command_buffer_begin_slot_index.has_value()) {
          uint32_t slot_index = completed_command_buffer.command_buffer_begin_slot_index.value();
          uint64_t begin_timestamp =
              QueryGpuTimestampNs(device, query_pool, slot_index, timestamp_period);
          command_buffer_proto->set_begin_gpu_timestamp_ns(begin_timestamp);

          query_slots_to_reset.push_back(slot_index);
        }

        uint32_t slot_index = completed_command_buffer.command_buffer_end_slot_index;
        uint64_t end_timestamp =
            QueryGpuTimestampNs(device, query_pool, slot_index, timestamp_period);

        command_buffer_proto->set_end_gpu_timestamp_ns(end_timestamp);
        query_slots_to_reset.push_back(slot_index);
      }
    }
  }

  void WriteDebugMarkers(const internal::QueueSubmission& completed_submission,
                         orbit_grpc_protos::GpuQueueSubmission* submission_proto,
                         std::vector<uint32_t>& query_slots_to_reset, VkDevice device,
                         VkQueryPool query_pool, const float timestamp_period) {
    submission_proto->set_num_begin_markers(completed_submission.num_begin_markers);
    for (const auto& marker_state : completed_submission.completed_markers) {
      uint64_t end_timestamp = QueryGpuTimestampNs(
          device, query_pool, marker_state.end_info->slot_index, timestamp_period);
      query_slots_to_reset.push_back(marker_state.end_info->slot_index);

      orbit_grpc_protos::GpuDebugMarker* marker_proto = submission_proto->add_completed_markers();
      marker_proto->set_text_key(
          (*vulkan_layer_producer_)->InternStringIfNecessaryAndGetKey(marker_state.text));
      if (marker_state.color.red != 0.0f || marker_state.color.green != 0.0f ||
          marker_state.color.blue != 0.0f || marker_state.color.alpha != 0.0f) {
        auto color = marker_proto->mutable_color();
        color->set_red(marker_state.color.red);
        color->set_green(marker_state.color.green);
        color->set_blue(marker_state.color.blue);
        color->set_alpha(marker_state.color.alpha);
      }
      marker_proto->set_depth(marker_state.depth);
      marker_proto->set_end_gpu_timestamp_ns(end_timestamp);

      // If we haven't captured the begin marker, we'll leave the optional begin_marker empty.
      if (!marker_state.begin_info.has_value()) {
        continue;
      }
      orbit_grpc_protos::GpuDebugMarkerBeginInfo* begin_debug_marker_proto =
          marker_proto->mutable_begin_marker();
      WriteMetaInfo(marker_state.begin_info->meta_information,
                    begin_debug_marker_proto->mutable_meta_info());

      uint64_t begin_timestamp = QueryGpuTimestampNs(
          device, query_pool, marker_state.begin_info->slot_index, timestamp_period);
      query_slots_to_reset.push_back(marker_state.begin_info->slot_index);

      begin_debug_marker_proto->set_gpu_timestamp_ns(begin_timestamp);
    }
  }

  // We use 0 to disable filtering of markers.
  uint32_t max_local_marker_depth_per_command_buffer_ = 0;

  absl::Mutex mutex_;
  absl::flat_hash_map<VkCommandPool, absl::flat_hash_set<VkCommandBuffer>> pool_to_command_buffers_;
  absl::flat_hash_map<VkCommandBuffer, VkDevice> command_buffer_to_device_;

  absl::flat_hash_map<VkCommandBuffer, CommandBufferState> command_buffer_to_state_;
  absl::flat_hash_map<VkQueue, std::vector<internal::QueueSubmission>> queue_to_submissions_;
  absl::flat_hash_map<VkQueue, QueueMarkerState> queue_to_markers_;

  DispatchTable* dispatch_table_;
  TimerQueryPool* timer_query_pool_;
  DeviceManager* device_manager_;

  [[nodiscard]] bool IsCapturing() {
    return *vulkan_layer_producer_ != nullptr && (*vulkan_layer_producer_)->IsCapturing();
  }
  std::unique_ptr<VulkanLayerProducer>* vulkan_layer_producer_;
};

}  // namespace orbit_vulkan_layer

#endif  // ORBIT_VULKAN_LAYER_COMMAND_BUFFER_MANAGER_H_