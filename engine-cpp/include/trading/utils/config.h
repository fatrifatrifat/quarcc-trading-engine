#pragma once

#include <trading/core/order_manager.h>
#include <yaml-cpp/yaml.h>

#include <string>
#include <vector>

namespace quarcc {
struct AppConfig {
  int polling_interval_ms{};
};

struct GrpcConfig {
  std::string host_post;
  std::string host;
  int port{};
};

struct NetworkConfig {
  GrpcConfig grpc;
};

struct DatabaseConfig {
  std::string journal = "default_journal";
  std::string orders = "default_orders";
};

struct StrategyConfig {
  std::string id;
  std::string gateway;
  DatabaseConfig database;
};

struct Config {
  AppConfig app;
  NetworkConfig network;
  std::vector<StrategyConfig> strategies;
};

static inline DatabaseConfig parse_database(const std::string &id,
                                            const YAML::Node &dbNode) {
  DatabaseConfig db;

  if (!dbNode)
    return db;

  if (dbNode["journal"])
    db.journal =
        std::format("{}_{}.db", id, dbNode["journal"].as<std::string>());

  if (dbNode["orders"])
    db.orders = std::format("{}_{}.db", id, dbNode["orders"].as<std::string>());

  return db;
}

inline Config parse_config(const std::string &path) {
  YAML::Node root = YAML::LoadFile(path);

  Config cfg;

  cfg.app.polling_interval_ms = root["app"]["polling_interval_ms"].as<int>();

  cfg.network.grpc.host = root["network"]["grpc"]["host"].as<std::string>();
  cfg.network.grpc.port = root["network"]["grpc"]["port"].as<int>();
  cfg.network.grpc.host_post =
      std::format("{}:{}", cfg.network.grpc.host, cfg.network.grpc.port);

  for (const auto &node : root["strategies"]) {
    StrategyConfig s;
    s.id = node["id"].as<std::string>();
    s.gateway = node["gateway"].as<std::string>();

    s.database = parse_database(s.id, node["database"]);

    cfg.strategies.push_back(std::move(s));
  }

  return cfg;
}

} // namespace quarcc
