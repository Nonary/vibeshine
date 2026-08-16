#include "terminal_session_seat_pool.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace terminal_session::seat_pool {
  pool_t::pool_t(std::unique_ptr<backend_t> backend, const std::size_t maximum_seats):
      backend_(std::move(backend)),
      maximum_seats_(std::max<std::size_t>(maximum_seats, 1)) {}

  capability_t pool_t::preflight() {
    std::lock_guard lock {mutex_};
    if (!backend_) return {.error = "Managed-seat backend is unavailable."};
    try {
      return backend_->preflight();
    } catch (...) {
      return {.error = "Managed-seat backend preflight raised unexpectedly."};
    }
  }

  bool pool_t::same_generation(const request_t &left, const request_t &right) {
    return left.client_uuid == right.client_uuid && left.generation == right.generation;
  }

  bool pool_t::refresh_locked(std::string &error) {
    std::vector<seat_t> discovered;
    error.clear();
    try {
      discovered = backend_->discover(error);
    } catch (...) {
      error = "Managed-seat discovery raised unexpectedly.";
      return false;
    }
    if (!error.empty()) return false;
    std::unordered_set<std::string> seen;
    for (auto &seat : discovered) {
      if (seat.seat_id.empty() || seat.account_name.empty() || !seat.managed) continue;
      seen.emplace(seat.seat_id);
      const auto existing = seats_.find(seat.seat_id);
      if (existing == seats_.end()) {
        seats_.emplace(seat.seat_id, record_t {.seat = std::move(seat)});
      } else {
        existing->second.seat.account_name = std::move(seat.account_name);
        existing->second.seat.account_sid = std::move(seat.account_sid);
        existing->second.seat.windows_session_id = seat.windows_session_id;
        existing->second.seat.connection = seat.connection;
        existing->second.seat.managed = seat.managed;
        existing->second.seat.reusable = seat.reusable;
      }
    }
    for (auto entry = seats_.begin(); entry != seats_.end();) {
      if (seen.contains(entry->first)) {
        ++entry;
      } else if (entry->second.owner) {
        entry->second.seat.connection = connection_state_e::faulted;
        entry->second.seat.reusable = false;
        ++entry;
      } else {
        entry = seats_.erase(entry);
      }
    }
    discovered_ = true;
    reap_locked();
    return true;
  }

  void pool_t::reap_locked() {
    for (auto &[_, record] : seats_) {
      if (!record.draining || !record.owner) continue;
      bool retained = true;
      try {
        retained = backend_->has_retained_applications(record.seat);
      } catch (...) {
        retained = true;
      }
      if (!retained) {
        record.owner.reset();
        record.draining = false;
        record.seat.reusable = true;
      }
    }
  }

  std::optional<std::string> pool_t::select_available_locked() {
    std::vector<std::string> ordered;
    ordered.reserve(seats_.size());
    for (const auto &[seat_id, _] : seats_) ordered.push_back(seat_id);
    std::sort(ordered.begin(), ordered.end());
    if (ordered.empty()) return std::nullopt;
    if (next_index_ >= ordered.size()) next_index_ %= ordered.size();
    for (std::size_t offset = 0; offset < ordered.size(); ++offset) {
      const auto index = (next_index_ + offset) % ordered.size();
      auto &record = seats_.at(ordered[index]);
      if (!record.owner && !record.draining && record.seat.reusable && record.seat.connection == connection_state_e::disconnected) {
        next_index_ = (index + 1) % ordered.size();
        return ordered[index];
      }
    }
    return std::nullopt;
  }

  std::optional<lease_t> pool_t::acquire(const request_t &request, std::string &error) {
    if (request.client_uuid.empty() || request.generation == 0 || request.launch_id == 0) {
      error = "Managed-seat request identity is incomplete.";
      return std::nullopt;
    }

    std::lock_guard lock {mutex_};
    if (!backend_) {
      error = "Managed-seat backend is unavailable.";
      return std::nullopt;
    }
    // Refresh on every admission. A service restart loses in-memory affinity,
    // so the backend must be allowed to quarantine disconnected seats that
    // still contain console-user applications before they can be reused.
    if (!refresh_locked(error)) return std::nullopt;
    reap_locked();

    for (auto &[_, record] : seats_) {
      if (!record.owner || record.owner->client_uuid != request.client_uuid) continue;
      if (!same_generation(*record.owner, request)) {
        error = "The paired client already owns a different retained seat generation.";
        return std::nullopt;
      }
      if (record.draining) {
        error = "The retained seat is draining and cannot be resumed.";
        return std::nullopt;
      }
      if (record.seat.connection == connection_state_e::faulted) {
        error = "The retained managed seat account or session is no longer discoverable.";
        return std::nullopt;
      }
      const bool resumed = record.seat.connection == connection_state_e::disconnected;
      if (resumed) {
        try {
          if (!backend_->connect(record.seat, request, error)) return std::nullopt;
        } catch (...) {
          error = "Managed-seat reconnect raised unexpectedly.";
          return std::nullopt;
        }
      }
      record.owner->launch_id = request.launch_id;
      return lease_t {record.seat, *record.owner, false, resumed};
    }

    bool created = false;
    auto selected = select_available_locked();
    if (!selected) {
      if (seats_.size() >= maximum_seats_) {
        error = "All managed Vibeshine seats are retained or active.";
        return std::nullopt;
      }
      std::optional<seat_t> seat;
      try {
        seat = backend_->create(error);
      } catch (...) {
        error = "Managed-seat account creation raised unexpectedly.";
        return std::nullopt;
      }
      if (!seat || seat->seat_id.empty() || seat->account_name.empty() || !seat->managed) {
        if (error.empty()) error = "Managed-seat backend did not create a valid Vibeshine account.";
        return std::nullopt;
      }
      selected = seat->seat_id;
      seats_.insert_or_assign(*selected, record_t {.seat = std::move(*seat)});
      created = true;
    }

    auto &record = seats_.at(*selected);
    record.owner = request; // Reserve before crossing into the session controller.
    record.draining = false;
    try {
      if (!backend_->connect(record.seat, request, error)) {
        record.owner.reset();
        return std::nullopt;
      }
    } catch (...) {
      record.owner.reset();
      error = "Managed-seat connection raised unexpectedly.";
      return std::nullopt;
    }
    return lease_t {record.seat, request, created, false};
  }

  bool pool_t::release(const request_t &request, const release_disposition_e disposition, std::string &error) {
    std::lock_guard lock {mutex_};
    for (auto &[_, record] : seats_) {
      if (!record.owner || record.owner->client_uuid != request.client_uuid) continue;
      if (!same_generation(*record.owner, request) || record.owner->launch_id != request.launch_id) {
        error = "Managed-seat release does not match the retained generation.";
        return false;
      }
      try {
        if (!backend_->disconnect(record.seat, error)) return false;
      } catch (...) {
        error = "Managed-seat disconnect raised unexpectedly.";
        return false;
      }
      record.seat.connection = connection_state_e::disconnected;
      if (disposition != release_disposition_e::retain) {
        bool retained = true;
        try {
          retained = backend_->has_retained_applications(record.seat);
        } catch (...) {
          retained = true;
        }
        if (retained) {
          record.draining = true;
          record.seat.reusable = false;
        } else {
          record.owner.reset();
          record.draining = false;
          record.seat.reusable = true;
        }
      }
      // retain deliberately preserves owner affinity and processes.
      return true;
    }
    return true;
  }

  std::optional<lease_t> pool_t::snapshot(const std::string_view client_uuid) const {
    std::lock_guard lock {mutex_};
    for (const auto &[_, record] : seats_) {
      if (record.owner && record.owner->client_uuid == client_uuid) {
        return lease_t {record.seat, *record.owner, false, record.seat.connection == connection_state_e::disconnected};
      }
    }
    return std::nullopt;
  }

  std::size_t pool_t::size() const {
    std::lock_guard lock {mutex_};
    return seats_.size();
  }
} // namespace terminal_session::seat_pool
