#pragma once

#include <trading/gateways/adapter_connection.h>

#include <memory>
#include <string>
#include <unordered_map>

namespace quarcc {

// Foward declare, we only need this, not the rest of config.h
// Caller must include config.h tho
struct AdapterConfig;

// Manages the lifecycle for Python adapter subprocesses
//
// It creates one AdapterProcess per <venue, account_id> pair, so multiple
// strategies that share that same pair get the same AdapterConnection via
// get_or_create()
//
// Used statically during the start up of the engine, in the
// TradingEngine::process_config() function
class AdapterManager {
public:
  ~AdapterManager();

  // Creates/Shares a common AdapterConnection between all <venue, account_id>
  // pairs It creates the subprocess and waits for it to ping back to the server
  // Throws on error
  std::shared_ptr<AdapterConnection>
  get_or_create(const std::string &venue, const std::string &account_id,
                const AdapterConfig &cfg);

  // Sends SIGTERM to all adapter processes and waits up to 5s per process
  // Sends SIGKILL to any that have yet been killed
  void stop_all();

private:
  struct AdapterProcess {
    std::string address;
    pid_t pid{-1};
    std::shared_ptr<AdapterConnection> conn;
  };

  using AdapterKey = std::pair<std::string, std::string>; // (venue, account_id)

  struct PairHash {
    size_t operator()(const AdapterKey &k) const noexcept {
      size_t h = std::hash<std::string>{}(k.first);
      h ^=
          std::hash<std::string>{}(k.second) * 0x9e3779b9 + (h << 6) + (h >> 2);
      return h;
    }
  };

  void spawn(AdapterProcess &proc, const AdapterConfig &cfg);
  static bool wait_for_ready(const AdapterConnection &conn,
                             std::chrono::seconds timeout);

  std::unordered_map<AdapterKey, AdapterProcess, PairHash> processes_;
};

} // namespace quarcc
