/*
 * Copyright (c) 2022 Lucian Radu Teodorescu
 *
 * Licensed under the Apache License Version 2.0 with LLVM Exceptions
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   https://llvm.org/LICENSE.txt
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdexec/execution.hpp>
#include <test_common/type_helpers.hpp>

#include <functional>
#include <vector>

namespace ex = stdexec;

// Put all the test utilities in an anonymous namespace to avoid ODR violations
namespace {
  template <class S>
  struct scheduler_env {
    template <stdexec::__one_of<ex::set_value_t, ex::set_error_t, ex::set_stopped_t> CPO>
    friend S tag_invoke(ex::get_completion_scheduler_t<CPO>, const scheduler_env&) noexcept {
      return {};
    }
  };

  //! Scheduler that will send impulses on user's request.
  //! One can obtain senders from this, connect them to receivers and start the operation states.
  //! Until the scheduler is told to start the next operation, the actions in the operation states are
  //! not executed. This is similar to a task scheduler, but it's single threaded. It has basic
  //! thread-safety to allow it to be run with `sync_wait` (which makes us not control when the
  //! operation_state object is created and started).
  struct impulse_scheduler {
   private:
    //! Command type that can store the action of firing up a sender
    using oper_command_t = std::function<void()>;
    using cmd_vec_t = std::vector<oper_command_t>;

    struct data {
      cmd_vec_t all_commands_;
      std::mutex mutex_;
      std::condition_variable cv_;
    };

    //! That data shared between the operation state and the actual scheduler
    //! Shared pointer to allow the scheduler to be copied (not the best semantics, but it will do)
    std::shared_ptr<data> shared_data_{};

    template <typename R>
    struct oper {
      data* data_;
      R receiver_;

      oper(data* shared_data, R&& recv)
        : data_(shared_data)
        , receiver_((R&&) recv) {
      }

      oper(oper&&) = delete;

      friend void tag_invoke(ex::start_t, oper& self) noexcept {
        // Enqueue another command to the list of all commands
        // The scheduler will start this, whenever start_next() is called
        std::unique_lock lock{self.data_->mutex_};
        self.data_->all_commands_.emplace_back([&self]() {
          if (ex::get_stop_token(ex::get_env(self.receiver_)).stop_requested()) {
            ex::set_stopped((R&&) self.receiver_);
          } else {
            ex::set_value((R&&) self.receiver_);
          }
        });
        self.data_->cv_.notify_all();
      }
    };

    struct my_sender {
      using __id = my_sender;
      using __t = my_sender;

      using is_sender = void;
      using completion_signatures = ex::completion_signatures< //
        ex::set_value_t(),                                     //
        ex::set_stopped_t()>;
      data* shared_data_;

      template <class R>
      friend oper<std::decay_t<R>> tag_invoke(ex::connect_t, my_sender self, R&& r) {
        return {self.shared_data_, (R&&) r};
      }

      friend scheduler_env<impulse_scheduler> tag_invoke(ex::get_env_t, const my_sender&) noexcept {
        return {};
      }
    };

   public:
    using __id = impulse_scheduler;
    using __t = impulse_scheduler;

    impulse_scheduler()
      : shared_data_(std::make_shared<data>()) {
    }

    ~impulse_scheduler() = default;

    //! Actually start the command from the last started operation_state
    //! Blocks if no command registered (i.e., no operation state started)
    void start_next() {
      // Wait for a command that we can execute
      std::unique_lock lock{shared_data_->mutex_};
      while (shared_data_->all_commands_.empty()) {
        shared_data_->cv_.wait(lock);
      }

      // Pop one command from the queue
      auto cmd = std::move(shared_data_->all_commands_.front());
      shared_data_->all_commands_.erase(shared_data_->all_commands_.begin());
      // Exit the lock before executing the command
      lock.unlock();
      // Execute the command, i.e., send an impulse to the connected sender
      cmd();
    }

    friend my_sender tag_invoke(ex::schedule_t, const impulse_scheduler& self) {
      return my_sender{self.shared_data_.get()};
    }

    friend bool operator==(impulse_scheduler, impulse_scheduler) noexcept {
      return true;
    }

    friend bool operator!=(impulse_scheduler, impulse_scheduler) noexcept {
      return false;
    }
  };

  //! Scheduler that executes everything inline, i.e., on the same thread
  template <class Domain = void>
  struct basic_inline_scheduler {
    using __t = basic_inline_scheduler;
    using __id = basic_inline_scheduler;

    template <typename R>
    struct oper : immovable {
      R recv_;

      friend void tag_invoke(ex::start_t, oper& self) noexcept {
        ex::set_value((R&&) self.recv_);
      }
    };

    struct my_sender {
      using __t = my_sender;
      using __id = my_sender;
      using is_sender = void;
      using completion_signatures = ex::completion_signatures<ex::set_value_t()>;

      template <typename R>
      friend oper<R> tag_invoke(ex::connect_t, my_sender self, R r) {
        return {{}, (R&&) r};
      }

      friend scheduler_env<basic_inline_scheduler>
        tag_invoke(ex::get_env_t, const my_sender&) noexcept {
        return {};
      }
    };

    friend my_sender tag_invoke(ex::schedule_t, const basic_inline_scheduler&) {
      return {};
    }

    friend bool operator==(const basic_inline_scheduler&, const basic_inline_scheduler&) noexcept {
      return true;
    }

    friend bool operator!=(const basic_inline_scheduler&, const basic_inline_scheduler&) noexcept {
      return false;
    }

    friend Domain tag_invoke(ex::get_domain_t, const basic_inline_scheduler&) noexcept
      requires(!ex::same_as<Domain, void>)
    {
      return Domain();
    }
  };

  using inline_scheduler = basic_inline_scheduler<>;

  //! Scheduler that returns a sender that always completes with error.
  template <typename E = std::exception_ptr>
  struct error_scheduler {
    using __id = error_scheduler;
    using __t = error_scheduler;

    error_scheduler() = default;

    error_scheduler(E err)
      : err_((E&&) err) {
    }

    error_scheduler(error_scheduler&&) noexcept = default;

    error_scheduler(const error_scheduler&) noexcept = default;

   private:
    template <typename R>
    struct oper : immovable {
      R recv_;
      E err_;

      friend void tag_invoke(ex::start_t, oper& self) noexcept {
        ex::set_error((R&&) self.recv_, (E&&) self.err_);
      }
    };

    struct my_sender {
      using __id = my_sender;
      using __t = my_sender;

      using is_sender = void;
      using completion_signatures = ex::completion_signatures< //
        ex::set_value_t(),                                     //
        ex::set_error_t(E),
        ex::set_stopped_t()>;

      E err_;

      template <typename R>
      friend oper<R> tag_invoke(ex::connect_t, my_sender self, R&& r) {
        return {{}, (R&&) r, (E&&) self.err_};
      }

      friend scheduler_env<error_scheduler> tag_invoke(ex::get_env_t, const my_sender&) noexcept {
        return {};
      }
    };

    E err_{};

    friend my_sender tag_invoke(ex::schedule_t, error_scheduler self) {
      return {(E&&) self.err_};
    }

    friend bool operator==(error_scheduler, error_scheduler) noexcept {
      return true;
    }

    friend bool operator!=(error_scheduler, error_scheduler) noexcept {
      return false;
    }
  };

  //! Scheduler that returns a sender that always completes with cancellation.
  struct stopped_scheduler {
    using __id = stopped_scheduler;
    using __t = stopped_scheduler;

    template <typename R>
    struct oper : immovable {
      R recv_;

      friend void tag_invoke(ex::start_t, oper& self) noexcept {
        ex::set_stopped((R&&) self.recv_);
      }
    };

    struct my_sender {
      using __id = my_sender;
      using __t = my_sender;

      using is_sender = void;
      using completion_signatures = ex::completion_signatures< //
        ex::set_value_t(),                                     //
        ex::set_stopped_t()>;

      template <typename R>
      friend oper<R> tag_invoke(ex::connect_t, my_sender self, R&& r) {
        return {{}, (R&&) r};
      }

      friend scheduler_env<stopped_scheduler> tag_invoke(ex::get_env_t, const my_sender&) noexcept {
        return {};
      }
    };

    friend my_sender tag_invoke(ex::schedule_t, stopped_scheduler) {
      return {};
    }

    friend bool operator==(stopped_scheduler, stopped_scheduler) noexcept {
      return true;
    }

    friend bool operator!=(stopped_scheduler, stopped_scheduler) noexcept {
      return false;
    }
  };
} // anonymous namespace