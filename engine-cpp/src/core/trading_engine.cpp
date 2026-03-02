#include <trading/core/trading_engine.h>
#include <trading/gateways/paper_trading_gateway.h>
#include <trading/persistence/sqlite_journal.h>
#include <trading/persistence/sqlite_order_store.h>

#include <chrono>
#include <thread>

namespace quarcc {

void TradingEngine::Run(const char *config_path) {
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  try {
    process_config(config_path);
  } catch (const std::exception &e) {
    std::cerr << "Config error: " << e.what() << std::endl;
  }

  server_->start();

  while (running_) {
    for (auto &[strategy_id, manager] : managers_)
      manager->process_fills();

    std::this_thread::sleep_for(fill_poll_interval_);
  }

  server_->shutdown();
  google::protobuf::ShutdownProtobufLibrary();
}

// Submit/Cancel/Replace orders through custom gateway
Result<BrokerOrderId>
TradingEngine::SubmitSignal(const v1::StrategySignal &signal) {
  auto it = managers_.find(signal.strategy_id());
  if (it == managers_.end())
    return std::unexpected(Error{"Unknown strategy", ErrorType::Error});
  return it->second->processSignal(signal);
}

Result<std::monostate>
TradingEngine::CancelOrder(const v1::CancelSignal &signal) {
  auto it = managers_.find(signal.strategy_id());
  if (it == managers_.end())
    return std::unexpected(Error{"Unknown strategy", ErrorType::Error});
  return it->second->processSignal(signal);
}

Result<BrokerOrderId>
TradingEngine::ReplaceOrder(const v1::ReplaceSignal &signal) {
  auto it = managers_.find(signal.strategy_id());
  if (it == managers_.end())
    return std::unexpected(Error{"Unknown strategy", ErrorType::Error});
  return it->second->processSignal(signal);
}

// Collects across all strategies, if multiple strategies hold the same symbol,
// it sums up the quantity and makes an weighted average of the price
Result<v1::Position>
TradingEngine::GetPosition(const v1::GetPositionRequest &req) {
  v1::Position combined;
  combined.set_symbol(req.symbol());
  bool found = false;

  for (const auto &[strategy_id, manager] : managers_) {
    auto pos = manager->get_position(req.symbol());
    if (!pos)
      continue;

    found = true;
    const double existing_qty = combined.quantity();
    const double new_qty = existing_qty + pos->quantity();

    if (new_qty != 0.0 && existing_qty != 0.0) {
      combined.set_avg_price((existing_qty * combined.avg_price() +
                              pos->quantity() * pos->avg_price()) /
                             new_qty);
    } else if (pos->quantity() != 0.0) {
      combined.set_avg_price(pos->avg_price());
    }
    combined.set_quantity(new_qty);
  }

  if (!found)
    return std::unexpected(
        Error{"No position found for " + req.symbol(), ErrorType::Error});

  return combined;
}

// Collects positions from every strategy and merges entries for the same symbol
// (sum of quantities, weighted-average price)
Result<v1::PositionList> TradingEngine::GetAllPositions(const v1::Empty &) {
  std::unordered_map<std::string, v1::Position> combined;

  for (const auto &[strategy_id, manager] : managers_) {
    const v1::PositionList list = manager->get_all_positions();
    for (const auto &pos : list.positions()) {
      auto it = combined.find(pos.symbol());
      if (it == combined.end()) {
        combined[pos.symbol()] = pos;
      } else {
        v1::Position &existing = it->second;
        const double new_qty = existing.quantity() + pos.quantity();
        if (new_qty != 0.0) {
          existing.set_avg_price((existing.quantity() * existing.avg_price() +
                                  pos.quantity() * pos.avg_price()) /
                                 new_qty);
        }
        existing.set_quantity(new_qty);
      }
    }
  }

  v1::PositionList result;
  for (auto &[symbol, pos] : combined)
    result.add_positions()->CopyFrom(pos);

  return result;
}

// Stops running, cancels all cancellable orders
Result<std::monostate>
TradingEngine::ActivateKillSwitch(const v1::KillSwitchRequest &req) {
  running_ = false;

  for (auto &[strategy_id, manager] : managers_)
    manager->cancel_all(req.reason(), req.initiated_by());

  return std::monostate{};
}

void TradingEngine::process_config(const std::string &path) {
  const Config config = parse_config(path);

  for (const auto &strat : config.strategies) {
    std::unique_ptr<IExecutionGateway> gateway;

    if (strat.gateway == "alpaca") {
      gateway = std::make_unique<AlpacaGateway>();
    } else if (strat.gateway == "paper trading") {
      gateway = std::make_unique<PaperGateway>();
    } else {
      throw std::runtime_error("Invalid gateway");
    }

    managers_.emplace(
        StrategyId{strat.id},
        OrderManager::CreateOrderManager(
            std::make_unique<PositionKeeper>(), std::move(gateway),
            std::make_unique<SQLiteJournal>(strat.database.journal),
            std::make_unique<SQLiteOrderStore>(strat.database.orders),
            std::make_unique<RiskManager>()));
  }

  server_ = std::make_unique<gRPCServer>(config.network.grpc.host_post, *this);
  fill_poll_interval_ =
      std::chrono::milliseconds{config.app.polling_interval_ms};
}

} // namespace quarcc
