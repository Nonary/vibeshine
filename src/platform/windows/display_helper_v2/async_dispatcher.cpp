#include "src/platform/windows/display_helper_v2/async_dispatcher.h"

#include "src/logging.h"

namespace display_helper::v2 {
  AsyncDispatcher::AsyncDispatcher(
    ApplyOperation &apply_operation,
    VerificationOperation &verification_operation,
    RecoveryOperation &recovery_operation,
    RecoveryValidationOperation &recovery_validation_operation,
    IVirtualDisplayDriver &virtual_display,
    IClock &clock
  ):
      apply_operation_(apply_operation),
      verification_operation_(verification_operation),
      recovery_operation_(recovery_operation),
      recovery_validation_operation_(recovery_validation_operation),
      virtual_display_(virtual_display),
      clock_(clock),
      worker_([this](std::stop_token stop_token) {
        worker_loop(stop_token);
      }) {}

  AsyncDispatcher::~AsyncDispatcher() {
    if (worker_.joinable()) {
      worker_.request_stop();
      cv_.notify_all();
      worker_.join();
    }
  }

  void AsyncDispatcher::dispatch_apply(
    const ApplyRequest &request,
    const CancellationToken &token,
    std::chrono::milliseconds delay,
    bool reset_virtual_display,
    std::function<void()> mutation_commit,
    std::function<void(const ApplyOutcome &)> completion
  ) {
    enqueue_task([this,
                  request = ApplyRequest {request},
                  token,
                  delay,
                  reset_virtual_display,
                  mutation_commit = std::move(mutation_commit),
                  completion = std::move(completion)]() mutable {
      bool completion_started = false;
      const auto finish = [&](const ApplyOutcome &outcome) {
        completion(outcome);
        completion_started = true;
      };
      try {
        if (delay > std::chrono::milliseconds::zero()) {
          clock_.sleep_for(delay);
        }

        const bool mutation_already_committed =
          request.deadline_committed ||
          (request.mutation_committed && request.mutation_committed->load(std::memory_order_acquire));
        if (!mutation_already_committed && request.expires_at && clock_.now() >= *request.expires_at) {
          ApplyOutcome outcome;
          outcome.status = ApplyStatus::Expired;
          finish(outcome);
          return;
        }

        if (reset_virtual_display) {
          if (token.is_cancelled()) {
            ApplyOutcome outcome;
            outcome.status = ApplyStatus::Fatal;
            finish(outcome);
            return;
          }
          // This is the first display mutation for the logical Apply. Once it
          // starts, finish the reset+Apply transaction even if the lease expires.
          request.deadline_committed = true;
          if (!mutation_already_committed) {
            if (mutation_commit) {
              mutation_commit();
            }
            apply_operation_.notify_mutation_commit();
          }
          if (!virtual_display_.disable()) {
            ApplyOutcome outcome;
            outcome.status = ApplyStatus::Fatal;
            outcome.deadline_committed = true;
            finish(outcome);
            return;
          }
          clock_.sleep_for(std::chrono::milliseconds(500));
          if (!virtual_display_.enable()) {
            ApplyOutcome outcome;
            outcome.status = ApplyStatus::Fatal;
            outcome.deadline_committed = true;
            finish(outcome);
            return;
          }
          clock_.sleep_for(std::chrono::milliseconds(1000));
          if (token.is_cancelled()) {
            ApplyOutcome outcome;
            outcome.status = ApplyStatus::Fatal;
            outcome.deadline_committed = true;
            finish(outcome);
            return;
          }
        }

        finish(apply_operation_.run(request, token, std::move(mutation_commit)));
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Display helper v2: Apply worker threw: " << e.what();
        if (!completion_started) {
          ApplyOutcome outcome;
          outcome.status = ApplyStatus::Fatal;
          outcome.deadline_committed =
            request.deadline_committed ||
            (request.mutation_committed && request.mutation_committed->load(std::memory_order_acquire));
          try {
            finish(outcome);
          } catch (...) {
            BOOST_LOG(error) << "Display helper v2: Apply failure completion threw.";
          }
        }
      } catch (...) {
        BOOST_LOG(error) << "Display helper v2: Apply worker threw an unknown exception.";
        if (!completion_started) {
          ApplyOutcome outcome;
          outcome.status = ApplyStatus::Fatal;
          outcome.deadline_committed =
            request.deadline_committed ||
            (request.mutation_committed && request.mutation_committed->load(std::memory_order_acquire));
          try {
            finish(outcome);
          } catch (...) {
            BOOST_LOG(error) << "Display helper v2: Apply failure completion threw.";
          }
        }
      }
    });
  }

  void AsyncDispatcher::dispatch_verification(
    const ApplyRequest &request,
    const std::optional<ActiveTopology> &expected_topology,
    const CancellationToken &token,
    std::function<void(bool)> completion
  ) {
    enqueue_task([this,
                  request,
                  expected_topology,
                  token,
                  completion = std::move(completion)]() mutable {
      bool completion_started = false;
      const auto finish = [&](bool success) {
        completion(success);
        completion_started = true;
      };
      try {
        finish(verification_operation_.run(request, expected_topology, token));
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Display helper v2: verification worker threw: " << e.what();
        if (!completion_started) {
          try {
            finish(false);
          } catch (...) {
          }
        }
      } catch (...) {
        BOOST_LOG(error) << "Display helper v2: verification worker threw an unknown exception.";
        if (!completion_started) {
          try {
            finish(false);
          } catch (...) {
          }
        }
      }
    });
  }

  void AsyncDispatcher::dispatch_recovery(
    const CancellationToken &token,
    std::chrono::milliseconds delay,
    std::function<void(const RecoveryOutcome &)> completion
  ) {
    enqueue_task([this,
                  token,
                  delay,
                  completion = std::move(completion)]() mutable {
      bool completion_started = false;
      const auto finish = [&](const RecoveryOutcome &outcome) {
        completion(outcome);
        completion_started = true;
      };
      try {
        // Sleep in slices so a DISARM/APPLY during the revert grace window can
        // cancel the pending restore before it touches the display stack.
        auto remaining = delay;
        constexpr auto kSlice = std::chrono::milliseconds(50);
        while (remaining > std::chrono::milliseconds::zero()) {
          if (token.is_cancelled()) {
            finish(RecoveryOutcome {});
            return;
          }
          const auto slice = remaining > kSlice ? kSlice : remaining;
          clock_.sleep_for(slice);
          remaining -= slice;
        }

        finish(recovery_operation_.run(token));
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Display helper v2: recovery worker threw: " << e.what();
        if (!completion_started) {
          try {
            finish(RecoveryOutcome {});
          } catch (...) {
          }
        }
      } catch (...) {
        BOOST_LOG(error) << "Display helper v2: recovery worker threw an unknown exception.";
        if (!completion_started) {
          try {
            finish(RecoveryOutcome {});
          } catch (...) {
          }
        }
      }
    });
  }

  void AsyncDispatcher::dispatch_recovery_validation(
    const Snapshot &snapshot,
    const CancellationToken &token,
    std::function<void(bool)> completion
  ) {
    enqueue_task([this,
                  snapshot,
                  token,
                  completion = std::move(completion)]() mutable {
      bool completion_started = false;
      const auto finish = [&](bool success) {
        completion(success);
        completion_started = true;
      };
      try {
        finish(recovery_validation_operation_.run(snapshot, token));
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Display helper v2: recovery validation worker threw: " << e.what();
        if (!completion_started) {
          try {
            finish(false);
          } catch (...) {
          }
        }
      } catch (...) {
        BOOST_LOG(error) << "Display helper v2: recovery validation worker threw an unknown exception.";
        if (!completion_started) {
          try {
            finish(false);
          } catch (...) {
          }
        }
      }
    });
  }

  void AsyncDispatcher::enqueue_task(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      tasks_.push_back(std::move(task));
    }
    cv_.notify_one();
  }

  void AsyncDispatcher::worker_loop(std::stop_token st) {
    while (!st.stop_requested()) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&]() {
          return st.stop_requested() || !tasks_.empty();
        });
        if (st.stop_requested()) {
          break;
        }
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }

      if (task) {
        try {
          task();
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "Display helper v2: unhandled async task exception: " << e.what();
        } catch (...) {
          BOOST_LOG(error) << "Display helper v2: unhandled unknown async task exception.";
        }
      }
    }
  }
}  // namespace display_helper::v2
