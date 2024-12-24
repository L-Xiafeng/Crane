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

#pragma once

#include "CranedPublicDefs.h"
// Precompiled header comes first.

#include <grp.h>

#include "CgroupManager.h"
#include "crane/PasswordEntry.h"
#include "protos/Crane.grpc.pb.h"

namespace Craned {

// Task execution info
struct ProcessInstance {
  crane::grpc::TaskToD task;
  pid_t pid;
};

// Job related info
// Todo: Task may consists of multiple subtasks
struct TaskInstance {
  ~TaskInstance() = default;

  crane::grpc::TaskToD task;

  std::string cgroup_path;
  CgroupInterface* cgroup;

  // Task execution results
  bool orphaned{false};
  CraneErr err_before_exec{CraneErr::kOk};

  absl::flat_hash_map<pid_t, std::unique_ptr<ProcessInstance>> processes;
};

/**
 * The class that manages all tasks and handles interrupts.
 * SIGINT and SIGCHLD are processed in JobManager.
 * Especially, outside caller can use SetSigintCallback() to
 * set the callback when SIGINT is triggered.
 */
class TaskManager {
 public:
  TaskManager();

  ~TaskManager();

  CraneErr ExecuteTaskAsync(crane::grpc::TaskToD const& task);

  CraneExpected<task_id_t> QueryTaskIdFromPidAsync(pid_t pid);

  CraneExpected<EnvMap> QueryTaskEnvMapAsync(task_id_t task_id);

  void TerminateTaskAsync(uint32_t task_id);

  void MarkTaskAsOrphanedAndTerminateAsync(task_id_t task_id);

  bool CheckTaskStatusAsync(task_id_t task_id, crane::grpc::TaskStatus* status);

  bool ChangeTaskTimeLimitAsync(task_id_t task_id, absl::Duration time_limit);

  // Wait internal libevent base loop to exit...
  void Wait();

  /***
   * Set the callback function will be called when SIGINT is triggered.
   * This function is not thread-safe.
   * @param cb the callback function.
   */
  void SetSigintCallback(std::function<void()> cb);

  // Called from SupervisorKeeper which guarantee no data race.
  void AddRecoveredTask_(crane::grpc::TaskToD task);

 private:
  template <class T>
  using ConcurrentQueue = moodycamel::ConcurrentQueue<T>;

  struct EvQueueQueryTaskIdFromPid {
    std::promise<CraneExpected<task_id_t>> task_id_prom;
    pid_t pid;
  };

  struct EvQueueQueryTaskEnvMap {
    std::promise<CraneExpected<EnvMap>> env_prom;
    task_id_t task_id;
  };

  struct ChangeTaskTimeLimitQueueElem {
    uint32_t task_id;
    absl::Duration time_limit;
    std::promise<bool> ok_prom;
  };

  struct TaskTerminateQueueElem {
    uint32_t task_id{0};
    bool terminated_by_user{false};     // If the task is canceled by user,
                                        // task->status=Cancelled
    bool mark_as_orphaned{false};
  };

  struct CheckTaskStatusQueueElem {
    task_id_t task_id;
    std::promise<std::pair<bool, crane::grpc::TaskStatus>> status_prom;
  };

  void LaunchTaskInstanceMt_(TaskInstance* instance);

  CraneErr SpawnSupervisor_(TaskInstance* instance, ProcessInstance* process);

  const TaskInstance* FindInstanceByTaskId_(uint32_t task_id);

  // todo: use grpc struct for params
  /**
   * Inform CraneCtld of the status change of a task.
   * This method is called when the status of a task is changed:
   * 1. A task is completed successfully. It means that this task returns
   *  normally with 0 or a non-zero code. (EvSigchldCb_)
   * 2. A task is killed by a signal. In this case, the task is considered
   *  failed. (EvSigchldCb_)
   * 3. A task cannot be created because of various reasons.
   *  (EvGrpcSpawnInteractiveTaskCb_ and EvGrpcExecuteTaskCb_)
   * @param release_resource If set to true, CraneCtld will release the
   *  resource (mark the task status as REQUEUE) and requeue the task.
   */
  void ActivateTaskStatusChangeAsync_(uint32_t task_id,
                                      crane::grpc::TaskStatus new_status,
                                      uint32_t exit_code,
                                      std::optional<std::string> reason);

  // todo: Refactor this, send rpc to supervisor
  /**
   * Send a signal to the process group to which the processes in
   *  ProcessInstance belongs.
   * This function ASSUMES that ALL processes belongs to the process group with
   *  the PGID set to the PID of the first process in this ProcessInstance.
   * @param signum the value of signal.
   * @return if the signal is sent successfully, kOk is returned.
   * if the task name doesn't exist, kNonExistent is returned.
   * if the signal is invalid, kInvalidParam is returned.
   * otherwise, kGenericFailure is returned.
   */
  static CraneErr KillPid_(pid_t pid, int signum);

  // Note: the three maps below are NOT protected by any mutex.
  //  They should be modified in libev callbacks to avoid races.

  // Contains all the task that is running on this Craned node.
  absl::flat_hash_map<task_id_t, std::unique_ptr<TaskInstance>> m_task_map_;

  //  ==================================================================
  // Critical data region starts
  //
  // To improve performance, the cgroup creation and task creation
  // are parallelized,
  // which breaks the serializability guaranteed by the single event loop.
  // The data structures in this region are accessed by multiple threads.
  // The atomicity of these data structure is guaranteed by either mutex or
  // AtomicHashMap.

  // The two following maps are used as indexes
  // and doesn't have the ownership of underlying objects.
  // A TaskInstance may contain more than one ProcessInstance.
  absl::flat_hash_map<pid_t /*pid*/, TaskInstance*> m_pid_task_map_
      ABSL_GUARDED_BY(m_mtx_);
  absl::flat_hash_map<pid_t /*pid*/, ProcessInstance*> m_pid_proc_map_
      ABSL_GUARDED_BY(m_mtx_);

  absl::Mutex m_mtx_;

  // Critical data region ends
  // ========================================================================

  void EvSigchldCb_();

  // Callback function to handle SIGINT sent by Ctrl+C
  void EvSigintCb_();

  void EvCleanGrpcExecuteTaskQueueCb_();

  void EvCleanGrpcQueryTaskIdFromPidQueueCb_();

  void EvCleanGrpcQueryTaskEnvQueueCb_();

  void EvCleanTaskStatusChangeQueueCb_();

  void EvCleanTerminateTaskQueueCb_();

  void EvCleanCheckTaskStatusQueueCb_();

  void EvCleanChangeTaskTimeLimitQueueCb_();


  std::shared_ptr<uvw::loop> m_uvw_loop_;

  std::shared_ptr<uvw::signal_handle> m_sigchld_handle_;

  // When this event is triggered, the JobManager will not accept
  // any more new tasks and quit as soon as all existing task end.
  std::shared_ptr<uvw::signal_handle> m_sigint_handle_;

  std::shared_ptr<uvw::async_handle> m_query_task_id_from_pid_async_handle_;
  ConcurrentQueue<EvQueueQueryTaskIdFromPid> m_query_task_id_from_pid_queue_;

  std::shared_ptr<uvw::async_handle>
      m_query_task_environment_variables_async_handle_;
  ConcurrentQueue<EvQueueQueryTaskEnvMap>
      m_query_task_environment_variables_queue;

  std::shared_ptr<uvw::async_handle> m_grpc_execute_task_async_handle_;
  // A custom event that handles the ExecuteTask RPC.
  ConcurrentQueue<std::unique_ptr<TaskInstance>> m_grpc_execute_task_queue_;

  std::shared_ptr<uvw::async_handle> m_task_status_change_async_handle_;
  ConcurrentQueue<TaskStatusChangeQueueElem> m_task_status_change_queue_;

  std::shared_ptr<uvw::async_handle> m_change_task_time_limit_async_handle_;
  ConcurrentQueue<ChangeTaskTimeLimitQueueElem> m_task_time_limit_change_queue_;

  std::shared_ptr<uvw::async_handle> m_terminate_task_async_handle_;
  ConcurrentQueue<TaskTerminateQueueElem> m_task_terminate_queue_;

  std::shared_ptr<uvw::async_handle> m_check_task_status_async_handle_;
  ConcurrentQueue<CheckTaskStatusQueueElem> m_check_task_status_queue_;

  // The function which will be called when SIGINT is triggered.
  std::function<void()> m_sigint_cb_;

  // When SIGINT is triggered or Shutdown() gets called, this variable is set to
  // true. Then, AddTaskAsyncMethod will not accept any more new tasks and
  // ev_sigchld_cb_ will stop the event loop when there is no task running.
  std::atomic_bool m_is_ending_now_{false};

  // After m_is_ending_now_ set to true, when all task are cleared, we can exit.
  std::atomic_bool m_task_cleared_{false};

  std::thread m_uvw_thread_;

  static inline TaskManager* m_instance_ptr_;
};
}  // namespace Craned

inline std::unique_ptr<Craned::TaskManager> g_task_mgr;