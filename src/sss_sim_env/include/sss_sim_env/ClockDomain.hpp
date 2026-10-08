#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <stdexcept>

namespace sss_utils {
// All calls belong to one native clock owner. A generation handle fences reuse.
class ClockDomain {
public:
  static constexpr size_t kClientSlots = 256;
  static constexpr uint64_t kMaxTime = UINT64_C(4294967295999999999);
  static constexpr uint64_t kInactiveTime = UINT64_C(4294967295000000000);
  enum class Error { None, Conflict, Invalid, NotFound, Exhausted };
  struct Snapshot {
    uint64_t revision = 1, desired_revision = 1, applied_revision = 1, time_ns = 0;
    uint32_t clients = 0, waiting_clients = 0, finite_clients = 0;
    bool paused = true;
    double max_speed_ratio = 100;
  };
  struct Result { Error error = Error::None; uint32_t client_id = 0; Snapshot snapshot; };
  ClockDomain(double speed = 100, bool start = false) {
    if (!std::isfinite(speed) || speed < 0.1 || speed > 100) throw std::invalid_argument("max_speed_ratio must be in [0.1,100]");
    snapshot_.max_speed_ratio = speed; snapshot_.paused = !start;
  }
  Snapshot snapshot() const {
    auto s = snapshot_;
    for (const auto& c : clients_) if (c.active) {
      if (!c.pending) ++s.waiting_clients;
      if (c.pending && c.request_ns < kInactiveTime) ++s.finite_clients;
    }
    return s;
  }
  Result control(uint64_t expected, bool proceed, double speed) {
    if (expected != snapshot_.desired_revision) return result(Error::Conflict);
    if (!std::isfinite(speed) || speed < 0.1 || speed > 100) return result(Error::Invalid);
    snapshot_.paused = !proceed; snapshot_.max_speed_ratio = speed;
    ++snapshot_.desired_revision; snapshot_.applied_revision = snapshot_.desired_revision;
    ++snapshot_.revision; return result();
  }
  Result reset(uint64_t expected, uint64_t time_ns) {
    if (expected != snapshot_.desired_revision) return result(Error::Conflict);
    if (time_ns > kMaxTime) return result(Error::Invalid);
    for (auto& c : clients_) { c.active = false; c.pending = false; }
    snapshot_.clients = 0; snapshot_.time_ns = time_ns; regulator_request_ = time_ns;
    regulator_pending_ = false; snapshot_.paused = true;
    ++snapshot_.desired_revision; snapshot_.applied_revision = snapshot_.desired_revision;
    ++snapshot_.revision; return result();
  }
  Result register_client() {
    for (size_t i = 1; i < clients_.size(); ++i) {
      auto& c = clients_[i];
      if (!c.active && c.generation < 0xFFFFFF) {
        ++c.generation; c.active = true; c.pending = false; c.request_ns = snapshot_.time_ns;
        ++snapshot_.clients; ++snapshot_.revision;
        return result(Error::None, uint32_t((c.generation << 8) | i));
      }
    }
    return result(Error::Exhausted);
  }
  Result unregister_client(uint32_t id) {
    auto* c = client(id); if (!c) return result(Error::NotFound);
    c->active = false; c->pending = false; --snapshot_.clients; ++snapshot_.revision;
    advance(); return result();
  }
  bool request(uint32_t id, uint64_t time_ns) {
    auto* c = client(id);
    if (!c || time_ns > kMaxTime || time_ns < snapshot_.time_ns) return false;
    if (!c->pending || c->request_ns != time_ns) ++snapshot_.revision;
    c->request_ns = time_ns; c->pending = true; advance(); return true;
  }
  void tick() {
    if (snapshot_.paused) return;
    if (snapshot_.time_ns >= regulator_request_) {
      regulator_request_ = std::min(kMaxTime, regulator_request_ + UINT64_C(10000000));
      regulator_pending_ = true;
    }
    advance();
  }
private:
  struct Client { uint32_t generation = 0; bool active = false, pending = false; uint64_t request_ns = 0; };
  Client* client(uint32_t id) {
    const auto slot = id & 255, generation = id >> 8;
    if (!slot || !generation) return nullptr;
    auto& c = clients_[slot]; return c.active && c.generation == generation ? &c : nullptr;
  }
  Result result(Error error = Error::None, uint32_t id = 0) const { return {error, id, snapshot()}; }
  void advance() {
    if (snapshot_.paused || !snapshot_.clients || !regulator_pending_) return;
    uint64_t minimum = regulator_request_; bool finite = false;
    for (const auto& c : clients_) if (c.active) {
      if (!c.pending) return;
      minimum = std::min(minimum, c.request_ns); finite |= c.request_ns < kInactiveTime;
    }
    if (!finite) return;
    if (minimum > snapshot_.time_ns) { snapshot_.time_ns = minimum; ++snapshot_.revision; }
    if (regulator_request_ <= snapshot_.time_ns) regulator_pending_ = false;
    for (auto& c : clients_) if (c.active && c.request_ns <= snapshot_.time_ns) c.pending = false;
  }
  std::array<Client, kClientSlots> clients_{};
  Snapshot snapshot_;
  uint64_t regulator_request_ = 0;
  bool regulator_pending_ = false;
};
}
