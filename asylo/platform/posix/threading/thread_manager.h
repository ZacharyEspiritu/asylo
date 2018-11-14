/*
 *
 * Copyright 2017 Asylo authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#ifndef ASYLO_PLATFORM_POSIX_THREADING_THREAD_MANAGER_H_
#define ASYLO_PLATFORM_POSIX_THREADING_THREAD_MANAGER_H_

#include <pthread.h>
#include <functional>
#include <memory>
#include <queue>
#include <utility>

#include "absl/container/flat_hash_map.h"

namespace asylo {

// ThreadManager class is a singleton responsible for:
// - Maintaining a queue of thread start_routine functions.
class ThreadManager {
 public:
  static ThreadManager *GetInstance();

  // Adds the given |function| to a start_routine queue of functions waiting to
  // be run by the pthreads implementation. |thread_id_out| will be updated to
  // the pthread_t of the created thread.
  int CreateThread(const std::function<void *()> &start_routine,
                   pthread_t *thread_id_out);

  // Removes a function from the start_routine queue and runs it. If no
  // start_routine is present this function will abort().
  int StartThread();

  // Waits till given |thread_id| has returned and assigns its returned void* to
  // |return_value|.
  int JoinThread(pthread_t thread_id, void **return_value_out);

 private:
  ThreadManager() = default;
  ThreadManager(ThreadManager const &) = delete;
  void operator=(ThreadManager const &) = delete;

  // Represents a thread inside of the enclave.
  class Thread {
   public:
    enum class ThreadState { QUEUED, RUNNING, DONE, JOINED };

    // Creates a thread in the QUEUED state with the specified |start_routine|.
    Thread(std::function<void *()> start_routine);

    ~Thread() = default;

    // Moves the thread into the RUNNING state, runs the thread's start_routine,
    // and then sets the state to DONE.
    void Run();

    // Returns the return value of the thread's start routine.
    void *GetReturnValue();

    // Updates the thread ID; used to bind an Asylo thread struct to the ID of
    // the donated Enclave thread running this Asylo thread.
    void UpdateThreadId(pthread_t thread_id);

    // Accessor for thread id.
    pthread_t GetThreadId();

    // Updates the thread state, potentially unblocking any thread waiting for
    // this thread to enter or exit that state.
    void UpdateThreadState(const ThreadState &state);

    // Blocks until this thread enters |state|.
    void WaitForThreadToEnterState(const ThreadState &state);

    // Blocks until this thread is not in |state|.
    void WaitForThreadToExitState(const ThreadState &state);

   private:
    // Function passed to pthread_create() bound to its argument.
    const std::function<void *()> start_routine_;

    // Return value of start_routine, set when Run() is complete.
    void *ret_;

    // Current thread_id if the thread has been allocated.
    pthread_t thread_id_;

    // Guards internal state of a Thread object.
    pthread_mutex_t lock_ = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t state_change_cond_ = PTHREAD_COND_INITIALIZER;
    ThreadState state_ = ThreadState::QUEUED;
  };

  // Returns a Thread pointer for a given |thread_id|.
  std::shared_ptr<Thread> GetThread(pthread_t thread_id);

  // Guards queued_threads_.
  pthread_mutex_t queued_threads_lock_ = PTHREAD_MUTEX_INITIALIZER;

  // Queue of start_routines waiting to be run.
  // std::shared_ptr is documented to use atomic increments/decrements to manage
  // a refcount instead of using a mutex.
  std::queue<std::shared_ptr<Thread>> queued_threads_;

  // Guards threads_.
  pthread_mutex_t threads_lock_ = PTHREAD_MUTEX_INITIALIZER;

  // List of currently running threads or threads waiting to be joined.
  absl::flat_hash_map<pthread_t, std::shared_ptr<Thread>> threads_;
};

}  // namespace asylo

#endif  // ASYLO_PLATFORM_POSIX_THREADING_THREAD_MANAGER_H_
