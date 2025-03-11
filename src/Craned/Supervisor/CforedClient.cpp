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

#include "CforedClient.h"

#include <cerrno>

#include "TaskManager.h"
#include "crane/String.h"
namespace Supervisor {

using crane::grpc::StreamCforedTaskIOReply;
using crane::grpc::StreamCforedTaskIORequest;

CforedClient::CforedClient() : m_stopped_(false) {
  m_loop_ = uvw::loop::create();

  std::shared_ptr<uvw::idle_handle> idle_handle =
      m_loop_->resource<uvw::idle_handle>();

  idle_handle->on<uvw::idle_event>(
      [this](const uvw::idle_event&, uvw::idle_handle& h) {
        if (m_stopped_) {
          h.parent().walk([](auto&& h) { h.close(); });
          h.parent().stop();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
      });
  if (idle_handle->start() != 0) {
    CRANE_ERROR("Failed to start the idle event in CforedManager EvLoop.");
  }
  m_ev_thread_ = std::thread([this] {
    util::SetCurrentThreadName("CforedClient");
    m_loop_->run();
  });
};

CforedClient::~CforedClient() {
  CRANE_TRACE("CforedClient to {} is being destructed.", m_cfored_name_);
  m_stopped_ = true;
  if (m_fwd_thread_.joinable()) m_fwd_thread_.join();
  if (m_ev_thread_.joinable()) m_ev_thread_.join();
  m_cq_.Shutdown();
  CRANE_TRACE("CforedClient to {} was destructed.", m_cfored_name_);
}

void CforedClient::SetUpTaskFwd(pid_t pid, int task_input_fd,
                                int task_output_fd, bool pty) {
  CRANE_DEBUG("Setting up task fwd for pid:{} input_fd:{} output_fd:{} pty:{}",
              pid, task_input_fd, task_output_fd, pty);
  TaskFwdMeta meta = {task_input_fd, task_output_fd, pid, pty};

  auto poll_handle = m_loop_->resource<uvw::poll_handle>(task_output_fd);
  poll_handle->on<uvw::poll_event>(
      [this, meta](const uvw::poll_event&, uvw::poll_handle& h) {
        CRANE_TRACE("Detect task #{} output.", g_config.JobId);

        constexpr int MAX_BUF_SIZE = 4096;
        char buf[MAX_BUF_SIZE];

        auto ret = read(meta.output_fd, buf, MAX_BUF_SIZE);
        bool read_finished{false};

        if (ret == 0) {
          if (!meta.pty) {
            read_finished = true;
          } else {
            // For pty,do nothing, process exit on return -1 and error set to
            // EIO
            CRANE_TRACE("Read EOF from pty task #{} on cfored {}",
                        g_config.JobId, m_cfored_name_);
          }
        }

        if (ret == -1) {
          if (!meta.pty) {
            CRANE_ERROR("Error when reading task #{} output, error {}",
                        g_config.JobId, std::strerror(errno));
            return;
          }

          if (errno == EIO) {
            // For pty output, the read() will return -1 with errno set to EIO
            // when process exit.
            // ref: https://unix.stackexchange.com/questions/538198
            read_finished = true;
          } else if (errno == EAGAIN) {
            // Read before the process begin.
            return;
          } else {
            CRANE_ERROR("Error when reading task #{} output, error {}",
                        g_config.JobId, std::strerror(errno));
            return;
          }
        }

        if (read_finished) {
          CRANE_TRACE("Task #{} to cfored {} finished its output.",
                      g_config.JobId, m_cfored_name_);
          h.close();
          close(meta.output_fd);

          bool ok_to_free = this->TaskOutputFinish(meta.pid);
          if (ok_to_free) {
            CRANE_TRACE("It's ok to unregister task #{} on {}", g_config.JobId,
                        m_cfored_name_);
            TaskEnd(meta.pid);
          }
          return;
        }

        std::string output(buf, ret);
        CRANE_TRACE("Fwd to task #{}: {}", g_config.JobId, output);
        this->TaskOutPutForward(output);
      });

  int ret = poll_handle->start(uvw::poll_handle::poll_event_flags::READABLE);
  if (ret < 0) {
    CRANE_ERROR("poll_handle->start() error: {}", uv_strerror(ret));
  }
  absl::MutexLock lock(&m_mtx_);
  m_fwd_meta_map[pid] = meta;
}

bool CforedClient::TaskInputNoLock_(const std::string& msg, int fd) {
  ssize_t sz_sent = 0, sz_written;
  while (sz_sent != msg.size()) {
    sz_written = write(fd, msg.c_str() + sz_sent, msg.size() - sz_sent);
    if (sz_written < 0) {
      CRANE_ERROR("Pipe to Crun task was broken.");
      return false;
    }

    sz_sent += sz_written;
  }
  return true;
}

void CforedClient::InitChannelAndStub(const std::string& cfored_name) {
  m_cfored_name_ = cfored_name;
  grpc::ChannelArguments channel_args;
  if (g_config.CompressedRpc)
    channel_args.SetCompressionAlgorithm(GRPC_COMPRESS_GZIP);

  if (g_config.CforedListenConf.UseTls) {
    m_cfored_channel_ = CreateTcpTlsChannelByHostname(
        cfored_name, kCforedDefaultPort, g_config.CforedListenConf.TlsCerts);
  } else {
    m_cfored_channel_ =
        CreateTcpInsecureChannel(cfored_name, kCforedDefaultPort);
  }

  // std::unique_ptr will automatically release the dangling stub.
  m_stub_ = crane::grpc::CraneForeD::NewStub(m_cfored_channel_);

  m_fwd_thread_ = std::thread([this] { AsyncSendRecvThread_(); });
}

void CforedClient::CleanOutputQueueAndWriteToStreamThread_(
    grpc::ClientAsyncReaderWriter<StreamCforedTaskIORequest,
                                  StreamCforedTaskIOReply>* stream,
    std::atomic<bool>* write_pending) {
  CRANE_TRACE("CleanOutputQueueThread started.");
  std::string output;
  bool ok = m_output_queue_.try_dequeue(output);

  // Make sure before exit all output has been drained.
  while (!m_stopped_ || ok) {
    if (!ok) {
      std::this_thread::sleep_for(std::chrono::milliseconds(75));
      ok = m_output_queue_.try_dequeue(output);
      continue;
    }

    StreamCforedTaskIORequest request;
    request.set_type(StreamCforedTaskIORequest::TASK_OUTPUT);

    auto* payload = request.mutable_payload_task_output_req();
    payload->set_msg(output);

    while (write_pending->load(std::memory_order::acquire))
      std::this_thread::sleep_for(std::chrono::milliseconds(25));

    CRANE_TRACE("Writing output...");
    write_pending->store(true, std::memory_order::release);
    stream->Write(request, (void*)Tag::Write);

    ok = m_output_queue_.try_dequeue(output);
  }

  CRANE_TRACE("CleanOutputQueueThread exited.");
}

void CforedClient::AsyncSendRecvThread_() {
  enum class State : int {
    Registering = 0,
    WaitRegisterAck = 1,
    Forwarding = 2,
    Unregistering = 3,
    End = 4,
  };

  std::thread output_clean_thread;
  std::atomic<bool> write_pending;

  bool ok;
  Tag tag;
  grpc::ClientContext context;
  StreamCforedTaskIORequest request;
  StreamCforedTaskIOReply reply;
  grpc::CompletionQueue::NextStatus next_status;

  auto stream =
      m_stub_->AsyncTaskIOStream(&context, &m_cq_, (void*)Tag::Prepare);

  CRANE_TRACE("Preparing TaskIOStream...");

  State state = State::Registering;
  while (true) {
    auto ddl = std::chrono::system_clock::now() + std::chrono::milliseconds(50);
    next_status = m_cq_.AsyncNext((void**)&tag, &ok, ddl);
    // CRANE_TRACE("NextStatus: {}, ok: {}, Tag received: {}, state: {}",
    //             int(next_status), ok, intptr_t(tag), int(state));

    if (next_status == grpc::CompletionQueue::SHUTDOWN) break;

    // TIMEOUT is like the Idle event in libuv and
    // thus a context switch of state machine.
    if (next_status == grpc::CompletionQueue::TIMEOUT) {
      if (m_stopped_) {
        CRANE_TRACE("TIMEOUT with m_stopped_=true.");

        // No need to switch to Unregistering state if already switched.
        if (state == State::Unregistering) continue;
        // Wait for forwarding thread to drain output queue and stop.
        if (output_clean_thread.joinable()) output_clean_thread.join();
        // If some writes are pending, let state machine clean them up.
        if (write_pending.load(std::memory_order::acquire)) continue;

        // Cfored received stopping signal. Unregistering...
        CRANE_TRACE("Unregistering on cfored {}.", m_cfored_name_);

        request.Clear();
        request.set_type(StreamCforedTaskIORequest::SUPERVISOR_UNREGISTER);
        request.mutable_payload_unregister_req()->set_craned_id(
            g_config.CranedIdOfThisNode);
        request.mutable_payload_unregister_req()->set_task_id(g_config.JobId);
        request.mutable_payload_unregister_req()->set_step_id(g_config.StepId);

        stream->WriteLast(request, grpc::WriteOptions(), (void*)Tag::Write);

        // There's no need to issue a read request here,
        // since every state ends with a read request issuing.

        state = State::Unregistering;
      }

      continue;
    }

    CRANE_ASSERT(next_status == grpc::CompletionQueue::GOT_EVENT);

    // All failures of Write or Read cause the end of state machine.
    // But, for Prepare tag which indicates the stream is ready,
    // ok is false, since there's no message to read.
    if (!ok && tag != Tag::Prepare) {
      CRANE_ERROR("Cfored connection failed.");
      state = State::End;
    }

    switch (state) {
    case State::Registering:
      // Stream is ready. Start registering.
      CRANE_TRACE("Registering new stream on cfored {}", m_cfored_name_);

      CRANE_ASSERT_MSG_VA(tag == Tag::Prepare, "Tag: {}", int(tag));

      request.set_type(StreamCforedTaskIORequest::SUPERVISOR_REGISTER);
      request.mutable_payload_register_req()->set_craned_id(
          g_config.CranedIdOfThisNode);
      request.mutable_payload_register_req()->set_task_id(g_config.JobId);
      request.mutable_payload_register_req()->set_step_id(g_config.StepId);

      write_pending.store(true, std::memory_order::release);
      stream->Write(request, (void*)Tag::Write);

      state = State::WaitRegisterAck;
      break;

    case State::WaitRegisterAck: {
      CRANE_TRACE("WaitRegisterAck");

      if (tag == Tag::Write) {
        write_pending.store(false, std::memory_order::release);
        CRANE_TRACE("Cfored Registration was sent. Reading Ack...");

        reply.Clear();
        stream->Read(&reply, (void*)Tag::Read);
      } else if (tag == Tag::Read) {
        CRANE_TRACE("Cfored RegisterAck Read. Start Forwarding..");
        state = State::Forwarding;

        // Issue initial read request
        reply.Clear();
        stream->Read(&reply, (void*)Tag::Read);

        // Start output forwarding thread
        output_clean_thread =
            std::thread(&CforedClient::CleanOutputQueueAndWriteToStreamThread_,
                        this, stream.get(), &write_pending);
      }
    } break;

    case State::Forwarding: {
      CRANE_TRACE("Forwarding State");
      // Do nothing for acknowledgements of successful writes in Forward State.
      if (tag == Tag::Write) {
        write_pending.store(false, std::memory_order::release);
        break;
      }

      CRANE_ASSERT(tag == Tag::Read);
      if (reply.type() != StreamCforedTaskIOReply::SUPERVISOR_TASK_INPUT) {
        CRANE_ERROR("Expect TASK_INPUT, but got {}", (int)reply.type());
        break;
      }

      const std::string& msg = reply.payload_task_input_req().msg();

      m_mtx_.Lock();

      for (auto& fwd_meta : m_fwd_meta_map | std::ranges::views::values) {
        if (!fwd_meta.input_stopped)
          fwd_meta.input_stopped = !TaskInputNoLock_(msg, fwd_meta.input_fd);
      }

      m_mtx_.Unlock();

      reply.Clear();
      stream->Read(&reply, (void*)Tag::Read);
    } break;

    case State::Unregistering:
      if (tag == Tag::Write) {
        CRANE_TRACE("UNREGISTER msg was sent. waiting for reply...");
        break;
      }
      CRANE_ASSERT(tag == Tag::Read);
      CRANE_TRACE("UNREGISTER_REPLY msg received.");

      if (reply.type() !=
          StreamCforedTaskIOReply::SUPERVISOR_UNREGISTER_REPLY) {
        CRANE_TRACE("Expect UNREGISTER_REPLY, but got {}. Ignoring it.",
                    (int)reply.type());
        reply.Clear();
        stream->Read(&reply, (void*)Tag::Read);
        break;
      }

      state = State::End;
      [[fallthrough]];

    case State::End:
      m_stopped_ = true;
      if (output_clean_thread.joinable()) output_clean_thread.join();
      break;
    }

    CRANE_TRACE("Next state: {}", int(state));
    if (state == State::End) break;
  }
}

bool CforedClient::TaskOutputFinish(pid_t pid) {
  absl::MutexLock lock(&m_mtx_);
  m_fwd_meta_map[pid].output_stopped = true;
  return m_fwd_meta_map[pid].proc_stopped;
};

bool CforedClient::TaskProcessStop(pid_t pid) {
  absl::MutexLock lock(&m_mtx_);
  m_fwd_meta_map[pid].proc_stopped = true;
  return m_fwd_meta_map[pid].output_stopped;
}

void CforedClient::TaskEnd(pid_t pid) {
  g_task_mgr->TaskStopAndDoStatusChange();
};

void CforedClient::TaskOutPutForward(const std::string& msg) {
  CRANE_TRACE("Receive TaskOutputForward for task #{}: {}", g_config.JobId,
              msg);
  m_output_queue_.enqueue(msg);
}

}  // namespace Supervisor
