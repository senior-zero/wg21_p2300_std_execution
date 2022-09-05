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
#include <exception>
#include <iostream>

namespace example::cuda::stream {
  struct receiver_base_t {};

  namespace bulk {

    template <class Fun>
    struct ignore_index_t {
      Fun fn_;

      template <class... As>
      __host__ __device__ void operator()(int, As&&... as) {
        fn_((As&&)as...);
      }
    };

    template <int BlockThreads, std::integral Shape, class Fun, class... As>
      __launch_bounds__(BlockThreads) 
      __global__ void kernel(Shape shape, Fun fn, As... as) {
        const int tid = static_cast<int>(threadIdx.x + blockIdx.x * blockDim.x);

        if (tid < static_cast<int>(shape)) {
          fn(tid, as...);
        }
      }

    template <class ReceiverId, std::integral Shape, class FunId>
      class receiver_t
        : std::execution::receiver_adaptor<receiver_t<ReceiverId, Shape, FunId>, std::__t<ReceiverId>>
        , receiver_base_t {
        using Receiver = std::__t<ReceiverId>;
        using Fun = std::__t<FunId>;
        friend std::execution::receiver_adaptor<receiver_t, Receiver>;

        Shape shape_;
        Fun f_;

        template <class... As>
        void set_value(As&&... as) && noexcept 
          requires std::__callable<Fun, Shape, std::decay_t<As>...> {

          // TODO GPU storage
          // TODO Return value for `then_t`
          constexpr int block_threads = 256;
          const int grid_blocks = (static_cast<int>(shape_) + block_threads - 1) / block_threads;
          kernel<block_threads, Shape, Fun, std::decay_t<As>...><<<grid_blocks, block_threads>>>(shape_, f_, as...);

          if constexpr (!std::is_base_of_v<receiver_base_t, Receiver>) {
            cudaStreamSynchronize(0);
          }

          std::execution::set_value(std::move(this->base()), (As&&)as...);
        }

       public:
        explicit receiver_t(Receiver rcvr, Shape shape, Fun fun)
          : std::execution::receiver_adaptor<receiver_t, Receiver>((Receiver&&) rcvr)
          , shape_(shape)
          , f_((Fun&&) fun)
        {}
      };

    template <class SenderId, std::integral Shape, class FunId>
      struct sender_t {
        using Sender = std::__t<SenderId>;
        using Fun = std::__t<FunId>;

        Sender sndr_;
        Shape shape_;
        Fun fun_;

        using set_error_t = 
          std::execution::completion_signatures<
            std::execution::set_error_t(std::exception_ptr)>;

        template <std::execution::receiver Receiver>
          using receiver = receiver_t<std::__x<std::remove_cvref_t<Receiver>>, Shape, FunId>;

        template <class... Tys>
        using set_value_t = 
          std::execution::completion_signatures<
            std::execution::set_value_t(std::decay_t<Tys>...)>;

        template <class Self, class Env>
          using completion_signatures =
            std::execution::__make_completion_signatures<
              std::__member_t<Self, Sender>,
              Env,
              set_error_t,
              std::__q<set_value_t>>;

        template <std::__decays_to<sender_t> Self, std::execution::receiver Receiver>
          requires std::execution::receiver_of<Receiver, completion_signatures<Self, std::execution::env_of_t<Receiver>>>
        friend auto tag_invoke(std::execution::connect_t, Self&& self, Receiver&& rcvr)
          noexcept(std::execution::__nothrow_connectable<std::__member_t<Self, Sender>, receiver<Receiver>>) 
          -> std::execution::connect_result_t<std::__member_t<Self, Sender>, receiver<Receiver>> {
          return std::execution::connect(((Self&&)self).sndr_, 
              receiver<Receiver>{(Receiver&&)rcvr, self.shape_, self.fun_});
        }

        template <std::__decays_to<sender_t> Self, class Env>
        friend auto tag_invoke(std::execution::get_completion_signatures_t, Self&&, Env)
          -> std::execution::dependent_completion_signatures<Env>;

        template <std::__decays_to<sender_t> Self, class Env>
        friend auto tag_invoke(std::execution::get_completion_signatures_t, Self&&, Env)
          -> completion_signatures<Self, Env> requires true;

        template <std::execution::tag_category<std::execution::forwarding_sender_query> Tag, class... As>
          requires std::__callable<Tag, const Sender&, As...>
        friend auto tag_invoke(Tag tag, const sender_t& self, As&&... as)
          noexcept(std::__nothrow_callable<Tag, const Sender&, As...>)
          -> std::__call_result_if_t<std::execution::tag_category<Tag, std::execution::forwarding_sender_query>, Tag, const Sender&, As...> {
          return ((Tag&&) tag)(self.sndr_, (As&&) as...);
        }
      };
  }

  struct scheduler_t {
    template <class R_>
      struct __op {
        using R = std::__t<R_>;
        [[no_unique_address]] R rec_;
        friend void tag_invoke(std::execution::start_t, __op& op) noexcept try {
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
          -> __op<std::__x<std::remove_cvref_t<R>>> {
          return {(R&&) rec};
        }

      friend scheduler_t
      tag_invoke(std::execution::get_completion_scheduler_t<std::execution::set_value_t>, sender_t) noexcept {
        return {};
      }
    };

    template <std::execution::sender Sender, std::integral Shape, class Fun>
      using bulk_sender_t = bulk::sender_t<std::__x<std::remove_cvref_t<Sender>>, Shape, std::__x<std::remove_cvref_t<Fun>>>;

    template <std::execution::sender S, std::integral Shape, class Fn>
    friend bulk_sender_t<S, Shape, Fn>
    tag_invoke(std::execution::bulk_t, const scheduler_t& sch, S&& sndr, Shape shape, Fn fun) noexcept {
      return bulk_sender_t<S, Shape, Fn>{(S&&) sndr, shape, (Fn&&)fun};
    }

    template <std::execution::sender S, class Fn>
    friend bulk_sender_t<S, int, bulk::ignore_index_t<Fn>>
    tag_invoke(std::execution::then_t, const scheduler_t& sch, S&& sndr, Fn fun) noexcept {
      return bulk_sender_t<S, int, bulk::ignore_index_t<Fn>>{(S&&) sndr, 1, bulk::ignore_index_t<Fn>{(Fn&&)fun}};
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

