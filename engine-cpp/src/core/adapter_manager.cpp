#include <trading/core/adapter_manager.h>
#include <trading/utils/config.h>

#include <csignal>
#include <cstring>
#include <format>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace quarcc {

AdapterManager::~AdapterManager() { stop_all(); }

std::shared_ptr<AdapterConnection>
AdapterManager::get_or_create(const std::string &venue,
                              const std::string &account_id,
                              const AdapterConfig &cfg) {
  // Get if already exists
  AdapterKey key{venue, account_id};
  if (auto it = processes_.find(key); it != processes_.end())
    return it->second.conn;

  AdapterProcess proc;
  // TODO: Client shouldn't worry about giving a port(?), we should be the big
  // boys handling that if possible
  proc.address = std::format("127.0.0.1:{}", cfg.port);
  proc.conn = make_adapter_connection(proc.address);

  spawn(proc, cfg);

  if (!wait_for_ready(*proc.conn, std::chrono::seconds{30}))
    throw std::runtime_error(
        std::format("Adapter for venue='{}' account='{}' at {} did not "
                    "become ready within 30s",
                    venue, account_id, proc.address));

  auto &stored = processes_.emplace(key, std::move(proc)).first->second;
  return stored.conn;
}

void AdapterManager::spawn(AdapterProcess &proc, const AdapterConfig &cfg) {
  const pid_t pid = fork();
  if (pid < 0)
    throw std::runtime_error(std::format("fork() failed: {}", strerror(errno)));

  if (pid == 0) {
    // Creates a copy of the current process with fork and starts the python
    // adapter process with exec()
    const std::string port_str = std::to_string(cfg.port);
    std::vector<const char *> argv = {
        "python3",        cfg.binary_path.c_str(), "--port",
        port_str.c_str(), "--credentials",         cfg.credentials_path.c_str(),
        "--venue",        cfg.venue.c_str(),
    };
    argv.push_back(nullptr);

    execvp("python3", const_cast<char *const *>(argv.data()));
    // Only reached on error.
    std::cerr << "[adapter] execvp failed: " << strerror(errno) << "\n";
    _exit(1);
  }

  proc.pid = pid;
}

bool AdapterManager::wait_for_ready(const AdapterConnection &conn,
                                    std::chrono::seconds timeout) {
  using namespace std::chrono_literals;
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (std::chrono::steady_clock::now() < deadline) {
    grpc::ClientContext ctx;
    // Deadline of the Ping request of 2s
    ctx.set_deadline(std::chrono::system_clock::now() + 2s);

    v1::PingResponse resp;
    const grpc::Status status = conn.stub->Ping(&ctx, v1::Empty{}, &resp);

    if (status.ok() && resp.ready())
      return true;

    std::this_thread::sleep_for(500ms);
  }
  return false;
}

void AdapterManager::stop_all() {
  using namespace std::chrono_literals;

  // Signals all of the processes so they start shutting down in parallel
  for (auto &[key, proc] : processes_) {
    if (proc.pid > 0)
      kill(proc.pid, SIGTERM);
  }

  // Wait for 5 seconds, force shutting them down if they're not already
  // shutdown
  for (auto &[key, proc] : processes_) {
    if (proc.pid <= 0)
      continue;

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (waitpid(proc.pid, nullptr, WNOHANG) != 0) {
        proc.pid = -1;
        break;
      }
      std::this_thread::sleep_for(100ms);
    }

    if (proc.pid > 0) {
      kill(proc.pid, SIGKILL);
      waitpid(proc.pid, nullptr, 0);
      proc.pid = -1;
    }
  }
}

} // namespace quarcc
