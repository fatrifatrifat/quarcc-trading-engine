#pragma once

#include <trading/core/order_manager.h>
#include <yaml-cpp/yaml.h>

#include <optional>
#include <string>
#include <vector>

namespace quarcc {

struct GrpcConfig {
  std::string host_post;
  std::string host;
  int port{};
};

struct NetworkConfig {
  GrpcConfig grpc;
};

struct MarketDataSubscription {
  Symbol symbol;
  BarPeriod period; // e.g. "1m", "5m", "1d"
};

struct MarketDataConfig {
  std::string feed; // "alpaca", "csv", "simulated"
  std::vector<MarketDataSubscription> subscriptions;
};

// Only when gateway == "grpc_adapter" in the config.yaml file
struct AdapterConfig {
  static inline const std::string binary_path =
      "python_client/adapters/adapter.py"; // path to the Python adapter script
                                           // has to be ran through the project
                                           // root
  std::string venue; // "ibkr", "binance", "polymarket", "paper trading", etc.
                     // (even tho none of them are implemented yet)
  std::string credentials_path; // path to credentials YAML for this venue
  int port{0};                  // port the adapter listens on (must be unique
                                // per (venue, account_id) on this host)
};

struct StrategyConfig {
  std::string id;
  std::string account_id;
  std::string gateway;
  std::optional<AdapterConfig>
      adapter; // required when gateway == "grpc_adapter"
  // Optional: not all strategies need market data from the engine
  std::optional<MarketDataConfig> market_data;
};

struct Config {
  std::string account_id;
  NetworkConfig network;
  std::vector<StrategyConfig> strategies;
};

inline Config parse_config(const std::string &path) {
  YAML::Node root = YAML::LoadFile(path);

  Config cfg;

  cfg.network.grpc.host = root["network"]["grpc"]["host"].as<std::string>();
  cfg.network.grpc.port = root["network"]["grpc"]["port"].as<int>();
  cfg.network.grpc.host_post =
      std::format("{}:{}", cfg.network.grpc.host, cfg.network.grpc.port);

  for (const auto &node : root["strategies"]) {
    StrategyConfig s;
    s.id = node["id"].as<std::string>();
    s.account_id = node["account_id"].as<std::string>();
    s.gateway = node["gateway"].as<std::string>();

    if (node["adapter"]) {
      const auto &a = node["adapter"];
      AdapterConfig ac;
      ac.venue = a["venue"].as<std::string>();
      ac.credentials_path = a["credentials"].as<std::string>();
      ac.port = a["port"].as<int>();
      s.adapter = std::move(ac);
    }

    if (node["market_data"]) {
      MarketDataConfig md;
      md.feed = node["market_data"]["feed"].as<std::string>();

      for (const auto &sub : node["market_data"]["subscriptions"]) {
        md.subscriptions.push_back({
            sub["symbol"].as<std::string>(),
            sub["period"].as<std::string>(),
        });
      }

      s.market_data = std::move(md);
    }

    cfg.strategies.push_back(std::move(s));
  }

  return cfg;
}

} // namespace quarcc
