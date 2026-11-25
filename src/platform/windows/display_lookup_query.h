#pragma once

#include <utility>

namespace platf {
  // A capture checkpoint may throw after an in-flight query returns.
  struct display_lookup_query_t {
    void (*checkpoint)() = nullptr;

    void check_stop() const {
      if (checkpoint) {
        checkpoint();
      }
    }

    template<class Query>
    auto operator()(Query &&query) const {
      check_stop();
      auto result = std::forward<Query>(query)();
      check_stop();
      return result;
    }
  };
}  // namespace platf
