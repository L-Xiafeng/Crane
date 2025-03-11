/**
 * Copyright (c) 2024 Peking University and Peking University
 * Changsha Institute for Computing and Digital Economy
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "CtldClient.h"

#include "CranedServer.h"
#include "JobManager.h"

namespace Craned {

CtldClient::~CtldClient() {
  m_thread_stop_ = true;

  CRANE_TRACE("CtldClient is ending. Waiting for the thread to finish.");
  if (m_async_send_thread_.joinable()) m_async_send_thread_.join();
}

void CtldClient::InitChannelAndStub(const std::string& server_address) {
  grpc::ChannelArguments channel_args;

  if (g_config.CompressedRpc)
    channel_args.SetCompressionAlgorithm(GRPC_COMPRESS_GZIP);

  if (g_config.ListenConf.UseTls)
    m_ctld_channel_ = CreateTcpTlsCustomChannelByHostname(
        server_address, g_config.CraneCtldListenPort,
        g_config.ListenConf.TlsCerts, channel_args);
  else
    m_ctld_channel_ = CreateTcpInsecureCustomChannel(
        server_address, g_config.CraneCtldListenPort, channel_args);

  // std::unique_ptr will automatically release the dangling stub.
  m_stub_ = CraneCtld::NewStub(m_ctld_channel_);

  m_async_send_thread_ = std::thread([this] { AsyncSendThread_(); });
}

void CtldClient::CranedConnected() {
  CRANE_DEBUG("Notify Ctld CranedConnected.");
  crane::grpc::CranedConnectedCtldNotify req;
  req.set_craned_id(g_config.CranedIdOfThisNode);
  grpc::ClientContext context;
  google::protobuf::Empty reply;
  grpc::Status status = m_stub_->CranedConnectedCtld(&context, req, &reply);
  if (!status.ok()) {
    CRANE_ERROR("Notify CranedConnected failed: {}", status.error_message());
    return;
  }
}

void CtldClient::CranedReady(const std::vector<task_id_t>& nonexistent_jobs) {
  crane::grpc::CranedReadyRequest ready_request;
  ready_request.set_craned_id(g_config.CranedIdOfThisNode);
  auto* grpc_meta = ready_request.mutable_remote_meta();
  auto& dres = g_config.CranedRes[g_config.CranedIdOfThisNode]->dedicated_res;

  grpc_meta->mutable_dres_in_node()->CopyFrom(
      static_cast<crane::grpc::DedicatedResourceInNode>(dres));

  grpc_meta->set_craned_version(CRANE_VERSION_STRING);

  const SystemRelInfo& sys_info = g_config.CranedMeta.SysInfo;
  auto* grpc_sys_rel_info = grpc_meta->mutable_sys_rel_info();
  grpc_sys_rel_info->set_name(sys_info.name);
  grpc_sys_rel_info->set_release(sys_info.release);
  grpc_sys_rel_info->set_version(sys_info.version);

  grpc_meta->mutable_craned_start_time()->set_seconds(
      ToUnixSeconds(g_config.CranedMeta.CranedStartTime));
  grpc_meta->mutable_system_boot_time()->set_seconds(
      ToUnixSeconds(g_config.CranedMeta.SystemBootTime));

  grpc_meta->mutable_nonexistent_jobs()->Assign(nonexistent_jobs.begin(),
                                                nonexistent_jobs.end());

  g_thread_pool->detach_task([req = std::move(ready_request), this] {
    int retry_count = 5;
    while (retry_count-- > 0) {
      crane::grpc::CranedReadyReply ready_reply;
      grpc::ClientContext context;
      context.set_deadline(std::chrono::system_clock::now() +
                           std::chrono::seconds(1));
      auto status = m_stub_->CranedReady(&context, req, &ready_reply);
      if (!status.ok()) {
        CRANE_DEBUG("CranedReady failed: {}, retry.", status.error_message());
        std::this_thread::sleep_for(std::chrono::seconds(2));
        continue;
      }
      if (ready_reply.ok()) {
        g_server->SetReady(true);
        CRANE_INFO("Craned successfully Up.");
        return;
      } else {
        CRANE_WARN("Craned ready get reply of false.");
        return;
      }
    }
  });
}

void CtldClient::TaskStatusChangeAsync(
    TaskStatusChangeQueueElem&& task_status_change) {
  absl::MutexLock lock(&m_task_status_change_mtx_);
  m_task_status_change_list_.emplace_back(std::move(task_status_change));
}

bool CtldClient::CancelTaskStatusChangeByTaskId(
    task_id_t task_id, crane::grpc::TaskStatus* new_status) {
  absl::MutexLock lock(&m_task_status_change_mtx_);

  size_t num_removed{0};

  for (auto it = m_task_status_change_list_.begin();
       it != m_task_status_change_list_.end();)
    if (it->task_id == task_id) {
      num_removed++;
      *new_status = it->new_status;
      it = m_task_status_change_list_.erase(it);
    } else
      ++it;

  CRANE_ASSERT_MSG(num_removed <= 1,
                   "TaskStatusChange should happen at most once "
                   "for a single running task!");

  return num_removed >= 1;
}

void CtldClient::AsyncSendThread_() {
  m_start_connecting_notification_.WaitForNotification();

  std::this_thread::sleep_for(std::chrono::seconds(1));

  absl::Condition cond(
      +[](decltype(m_task_status_change_list_)* queue) {
        return !queue->empty();
      },
      &m_task_status_change_list_);

  bool prev_conn_state = false;
  while (true) {
    if (m_thread_stop_) break;

    bool connected = m_ctld_channel_->WaitForConnected(
        std::chrono::system_clock::now() + std::chrono::seconds(3));

    if (!prev_conn_state && connected) {
      CRANE_TRACE("Channel to CraneCtlD is connected.");
      absl::MutexLock lk(&m_cb_mutex_);
      if (m_on_ctld_connected_cb_) m_on_ctld_connected_cb_();
    } else if (!connected && prev_conn_state) {
      CRANE_TRACE("Channel to CraneCtlD is disconnected.");
      absl::MutexLock lk(&m_cb_mutex_);
      if (m_on_ctld_disconnected_cb_) m_on_ctld_disconnected_cb_();
    }
    prev_conn_state = connected;

    if (!connected) {
      CRANE_INFO("Channel to CraneCtlD is not connected. Reconnecting...");
      std::this_thread::sleep_for(std::chrono::seconds(10));
      continue;
    }

    bool has_msg = m_task_status_change_mtx_.LockWhenWithTimeout(
        cond, absl::Milliseconds(50));
    if (!has_msg) {
      m_task_status_change_mtx_.Unlock();
      continue;
    }

    std::list<TaskStatusChangeQueueElem> changes;
    changes.splice(changes.begin(), std::move(m_task_status_change_list_));
    m_task_status_change_mtx_.Unlock();

    while (!changes.empty()) {
      grpc::ClientContext context;
      crane::grpc::TaskStatusChangeRequest request;
      crane::grpc::TaskStatusChangeReply reply;
      grpc::Status status;

      auto status_change = changes.front();

      CRANE_TRACE("Sending TaskStatusChange for task #{}",
                  status_change.task_id);

      request.set_craned_id(m_craned_id_);
      request.set_task_id(status_change.task_id);
      request.set_new_status(status_change.new_status);
      request.set_exit_code(status_change.exit_code);
      if (status_change.reason.has_value())
        request.set_reason(status_change.reason.value());

      status = m_stub_->TaskStatusChange(&context, request, &reply);
      if (!status.ok()) {
        CRANE_ERROR(
            "Failed to send TaskStatusChange: "
            "{{TaskId: {}, NewStatus: {}}}, reason: {} | {}, code: {}",
            status_change.task_id, int(status_change.new_status),
            status.error_message(), context.debug_error_string(),
            int(status.error_code()));

        if (status.error_code() == grpc::UNAVAILABLE) {
          // If some messages are not sent due to channel failure,
          // put them back into m_task_status_change_list_
          if (!changes.empty()) {
            m_task_status_change_mtx_.Lock();
            m_task_status_change_list_.splice(
                m_task_status_change_list_.begin(), std::move(changes));
            m_task_status_change_mtx_.Unlock();
          }
          break;
        } else
          changes.pop_front();
      } else {
        CRANE_TRACE("TaskStatusChange for task #{} sent. reply.ok={}",
                    status_change.task_id, reply.ok());
        changes.pop_front();
      }
    }
  }
}

}  // namespace Craned
