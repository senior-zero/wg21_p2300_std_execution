/*
 * Copyright (c) NVIDIA
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

#include <execution.hpp>
#include <type_traits>

#include <thrust/device_vector.h>

#include "detail/common.cuh"
#include "detail/then.cuh"
#include "detail/bulk.cuh"
#include "detail/when_all.cuh"

namespace example::cuda::stream {

  struct scheduler_t {
    template <class R_>
      struct operation_state_t {
        using R = std::__t<R_>;
        [[no_unique_address]] R rec_;
        friend void tag_invoke(std::execution::start_t, operation_state_t& op) noexcept try {
          std::execution::set_value((R&&) op.rec_);
        } catch(...) {
          std::execution::set_error((R&&) op.rec_, std::current_exception());
        }
      };

    struct sender_t {
      using completion_signatures =
        std::execution::completion_signatures<
          std::execution::set_value_t(),
          std::execution::set_error_t(std::exception_ptr)>;

      template <class R>
        friend auto tag_invoke(std::execution::connect_t, sender_t, R&& rec)
          noexcept(std::is_nothrow_constructible_v<std::remove_cvref_t<R>, R>)
          -> operation_state_t<std::__x<std::remove_cvref_t<R>>> {
          return {(R&&) rec};
        }

      friend scheduler_t
      tag_invoke(std::execution::get_completion_scheduler_t<std::execution::set_value_t>, sender_t) noexcept {
        return {};
      }
    };

    template <std::execution::sender Sender, std::integral Shape, class Fun>
      using bulk_sender_t = bulk::sender_t<std::__x<std::remove_cvref_t<Sender>>, Shape, std::__x<std::remove_cvref_t<Fun>>>;

    template <std::execution::sender Sender, class Fun>
      using then_sender_t = then::sender_t<std::__x<std::remove_cvref_t<Sender>>, std::__x<std::remove_cvref_t<Fun>>>;

    template <std::execution::sender... Senders>
      using when_all_sender_t = example::cuda::stream::when_all::sender_t<std::__x<std::decay_t<Senders>>...>;

    template <std::execution::sender S, std::integral Shape, class Fn>
    friend bulk_sender_t<S, Shape, Fn>
    tag_invoke(std::execution::bulk_t, const scheduler_t& sch, S&& sndr, Shape shape, Fn fun) noexcept {
      return bulk_sender_t<S, Shape, Fn>{(S&&) sndr, shape, (Fn&&)fun};
    }

    template <std::execution::sender S, class Fn>
    friend then_sender_t<S, Fn>
    tag_invoke(std::execution::then_t, const scheduler_t& sch, S&& sndr, Fn fun) noexcept {
      return then_sender_t<S, Fn>{(S&&) sndr, (Fn&&)fun};
    }

    template <std::execution::sender... Senders>
    friend auto 
    tag_invoke(std::execution::transfer_when_all_t, const scheduler_t& sch, Senders&&... sndrs) noexcept {
      return std::execution::transfer(when_all_sender_t<Senders...>{(Senders&&)sndrs...}, sch);
    }

    friend sender_t tag_invoke(std::execution::schedule_t, const scheduler_t&) noexcept {
      return {};
    }

    friend std::execution::forward_progress_guarantee tag_invoke(
        std::execution::get_forward_progress_guarantee_t,
        const scheduler_t&) noexcept {
      return std::execution::forward_progress_guarantee::weakly_parallel;
    }

    bool operator==(const scheduler_t&) const noexcept = default;
  };
}

