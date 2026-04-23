#include <trading/core/trading_engine.h>
#include <trading/gateways/grpc_gateway.h>
#include <trading/gateways/grpc_market_data_feed.h>
#include <trading/gateways/paper_trading_gateway.h>
#include <trading/gateways/simulated_feed.h>
#include <trading/persistence/sqlite_journal.h>
#include <trading/persistence/sqlite_order_store.h>

#include <shared_mutex>

namespace quarcc {

void TradingEngine::Run(const char *config_path) {
  GOOGLE_PROTOBUF_VERIFY_VERSION;

  try {
    process_config(config_path);
  } catch (const std::exception &e) {
    std::cerr << "Config error: " << e.what() << std::endl;
    return;
  }

  feed_registry_.start_all();
  server_->start();

  // Block until ActivateKillSwitch() sets running_ = false and notifies
  {
    std::unique_lock lk{run_mu_};
    run_cv_.wait(lk, [&] { return !running_.load(); });
  }

  server_->shutdown();
  feed_registry_.stop_all();
  google::protobuf::ShutdownProtobufLibrary();
}

// Creates one OrderManager + registers its feeds
// Called by process_config (single threaded, before the server starts) and by
// RegisterStrategy (under a unique_lock on managers_mu_)
Result<std::monostate>
TradingEngine::create_strategy(const StrategyConfig &strat) {
  // If the client already exists (e.g. it disconnected and reconnected), no
  // need to recreate it, it will resubscribe to it's data feeds in
  // RegisterStrategy anyways
  if (managers_.contains(strat.id))
    return std::monostate{};

  std::unique_ptr<IExecutionGateway> gateway;

  if (strat.gateway == "grpc_adapter") {
    if (!strat.adapter)
      return std::unexpected(Error{
          "Strategy '" + strat.id +
              "' uses gateway 'grpc_adapter' but has no 'adapter' config block",
          ErrorType::Error});

    auto conn = adapter_manager_.get_or_create(
        strat.adapter->venue, strat.account_id, *strat.adapter);
    gateway = std::make_unique<GrpcGateway>(strat.id, conn);

  } else if (strat.gateway == "alpaca") {
#if TRADING_ENABLE_ALPACA_SDK
    gateway = std::make_unique<AlpacaGateway>();
#else
    return std::unexpected(
        Error{"Alpaca gateway not compiled in (TRADING_ENABLE_ALPACA_SDK=OFF)",
              ErrorType::Error});
#endif
  } else if (strat.gateway == "paper trading") {
    gateway = std::make_unique<PaperGateway>();
  } else {
    return std::unexpected(
        Error{"Invalid gateway: " + strat.gateway, ErrorType::Error});
  }

  managers_.emplace(StrategyId{strat.id},
                    OrderManager::create_order_manager(
                        strat.account_id, std::make_unique<PositionKeeper>(),
                        std::move(gateway),
                        std::make_unique<SQLiteJournal>(strat.id),
                        std::make_unique<SQLiteOrderStore>(strat.id),
                        std::make_unique<RiskManager>()));

  if (strat.market_data) {
    OrderManager *om = managers_.at(strat.id).get();
    const FeedKey feed_key{strat.market_data->feed, strat.account_id};

    if (strat.gateway == "grpc_adapter" && strat.adapter &&
        !feed_registry_.has_feed(feed_key)) {
      // Creates the adapter process for the current strategy's market data feed
      auto conn = adapter_manager_.get_or_create(
          strat.adapter->venue, strat.account_id, *strat.adapter);
      feed_registry_.register_feed(
          feed_key, std::make_unique<GrpcMarketDataFeed>(std::move(conn)));
    } else if (strat.market_data && strat.market_data->feed == "simulated") {
      feed_registry_.register_feed(feed_key, std::make_unique<SimulatedFeed>());
    }

    for (const auto &sub : strat.market_data->subscriptions)
      feed_registry_.register_subscription(feed_key, sub.symbol, sub.period,
                                           om);
  }

  return std::monostate{};
}

void TradingEngine::process_config(const std::string &path) {
  const Config config = parse_config(path);

  for (const auto &strat : config.strategies) {
    if (auto r = create_strategy(strat); !r)
      throw std::runtime_error("Failed to create strategy '" + strat.id +
                               "': " + r.error().message_);
  }

  server_ = std::make_unique<gRPCServer>(config.network.grpc.host_post, *this);
}

// This is the epic dynamic strategy registration function
Result<std::monostate>
TradingEngine::RegisterStrategy(const v1::RegisterStrategyRequest &req) {
  // Convert proto message -> StrategyConfig
  StrategyConfig strat;
  strat.id = req.strategy_id();
  strat.account_id = req.account_id();
  strat.gateway = req.gateway();

  if (req.has_adapter()) {
    AdapterConfig ac;
    ac.venue = req.adapter().venue();
    ac.credentials_path = req.adapter().credentials_path();
    ac.port = req.adapter().port();
    strat.adapter = std::move(ac);
  }

  if (req.has_market_data()) {
    MarketDataConfig md;
    md.feed = req.market_data().feed();
    for (const auto &sub : req.market_data().subscriptions())
      md.subscriptions.push_back({sub.symbol(), sub.period()});
    strat.market_data = std::move(md);
  }

  std::unique_lock lk{managers_mu_};
  return create_strategy(strat);
}

Result<BrokerOrderId>
TradingEngine::SubmitSignal(const v1::StrategySignal &signal) {
  OrderManager *om;
  {
    std::shared_lock lk{managers_mu_};
    auto it = managers_.find(signal.strategy_id());
    if (it == managers_.end()) [[unlikely]]
      return std::unexpected(Error{"Unknown strategy", ErrorType::Error});
    om = it->second.get();
  }
  return om->process_signal(signal);
}

Result<std::monostate>
TradingEngine::CancelOrder(const v1::CancelSignal &signal) {
  OrderManager *om;
  {
    std::shared_lock lk{managers_mu_};
    auto it = managers_.find(signal.strategy_id());
    if (it == managers_.end()) [[unlikely]]
      return std::unexpected(Error{"Unknown strategy", ErrorType::Error});
    om = it->second.get();
  }
  return om->process_signal(signal);
}

Result<BrokerOrderId>
TradingEngine::ReplaceOrder(const v1::ReplaceSignal &signal) {
  OrderManager *om;
  {
    std::shared_lock lk{managers_mu_};
    auto it = managers_.find(signal.strategy_id());
    if (it == managers_.end()) [[unlikely]]
      return std::unexpected(Error{"Unknown strategy", ErrorType::Error});
    om = it->second.get();
  }
  return om->process_signal(signal);
}

Result<v1::Position>
TradingEngine::GetPosition(const v1::GetPositionRequest &req) {
  v1::Position combined;
  combined.set_symbol(req.symbol());
  bool found = false;

  std::shared_lock lk{managers_mu_};
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

Result<v1::PositionList> TradingEngine::GetAllPositions(const v1::Empty &) {
  std::unordered_map<std::string, v1::Position> combined;

  std::shared_lock lk{managers_mu_};
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

Result<std::monostate>
TradingEngine::SetupMarketDataStream(const std::string &strategy_id,
                                     MarketDataSinks sinks) {
  OrderManager *om;
  {
    std::shared_lock lk{managers_mu_};
    auto it = managers_.find(strategy_id);
    if (it == managers_.end())
      return std::unexpected(
          Error{"Unknown strategy: " + strategy_id, ErrorType::Error});
    om = it->second.get();
  }
  om->set_market_data_sinks(std::move(sinks.on_tick), std::move(sinks.on_bar));
  return std::monostate{};
}

void TradingEngine::ClearMarketDataStream(const std::string &strategy_id) {
  std::shared_lock lk{managers_mu_};
  auto it = managers_.find(strategy_id);
  if (it != managers_.end())
    it->second->clear_market_data_sinks();
}

Result<std::monostate> TradingEngine::SetupFillStream(
    const std::string &strategy_id,
    std::function<void(const v1::ExecutionReport &)> sink) {
  OrderManager *om;
  {
    std::shared_lock lk{managers_mu_};
    auto it = managers_.find(strategy_id);
    if (it == managers_.end())
      return std::unexpected(
          Error{"Unknown strategy: " + strategy_id, ErrorType::Error});
    om = it->second.get();
  }
  om->set_fill_sink(std::move(sink));
  return std::monostate{};
}

void TradingEngine::ClearFillStream(const std::string &strategy_id) {
  std::shared_lock lk{managers_mu_};
  auto it = managers_.find(strategy_id);
  if (it != managers_.end())
    it->second->clear_fill_sink();
}

Result<std::monostate>
TradingEngine::ActivateKillSwitch(const v1::KillSwitchRequest &req) {
  {
    std::shared_lock lk{managers_mu_};
    for (auto &[strategy_id, manager] : managers_)
      manager->cancel_all(req.reason(), req.initiated_by());
  }

  {
    std::lock_guard lk{run_mu_};
    running_ = false;
  }
  run_cv_.notify_one();

  return std::monostate{};
}

} // namespace quarcc
