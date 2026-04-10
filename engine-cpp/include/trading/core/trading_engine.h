#pragma once

#include <trading/core/adapter_manager.h>
#include <trading/core/feed_registry.h>
#include <trading/core/order_manager.h>
#if TRADING_ENABLE_ALPACA_SDK
#include <trading/gateways/alpaca_fix_gateway.h>
#endif
#include <trading/grpc/grpc_server.h>
#include <trading/interfaces/i_execution_service_handler.h>
#include <trading/utils/config.h>
#include <trading/utils/order_id_generator.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <shared_mutex>

namespace quarcc {

class TradingEngine final : public IExecutionServiceHandler {
  using StrategyId = std::string;

public:
  void Run(const char *config_path);

private:
  void process_config(const std::string &path);

  // Creates an OrderManager + registers feeds for one strategy
  // Caller must NOT hold managers_mu_ (process_config calls this before the
  // server starts RegisterStrategy calls this under a unique_lock)
  Result<std::monostate> create_strategy(const StrategyConfig &strat);

private:
  // IExecutionServiceHandler functions
  Result<BrokerOrderId> SubmitSignal(const v1::StrategySignal &req) override;
  Result<std::monostate> CancelOrder(const v1::CancelSignal &req) override;
  Result<BrokerOrderId> ReplaceOrder(const v1::ReplaceSignal &req) override;
  Result<v1::Position> GetPosition(const v1::GetPositionRequest &req) override;
  Result<v1::PositionList> GetAllPositions(const v1::Empty &req) override;
  Result<std::monostate>
  ActivateKillSwitch(const v1::KillSwitchRequest &req) override;
  Result<std::monostate> SetupMarketDataStream(const std::string &strategy_id,
                                               MarketDataSinks sinks) override;
  void ClearMarketDataStream(const std::string &strategy_id) override;
  Result<std::monostate>
  RegisterStrategy(const v1::RegisterStrategyRequest &req) override;
  Result<std::monostate> SetupFillStream(
      const std::string &strategy_id,
      std::function<void(const v1::ExecutionReport &)> sink) override;
  void ClearFillStream(const std::string &strategy_id) override;

private:
  std::atomic<bool> running_{true};

  std::mutex run_mu_;
  std::condition_variable run_cv_;

  // adapter_manager_ must be declared before feed_registry_ and managers_:
  // its destructor sends SIGTERM to adapters, which must happen after the
  // feed and order manager threads have stopped
  AdapterManager adapter_manager_;
  FeedRegistry feed_registry_;

  std::unique_ptr<gRPCServer> server_;

  // managers_ is read by many gRPC threads concurrently and written only by
  // RegisterStrategy
  // Use a shared_mutex so reads don't block each other
  //
  // TODO RESEARCH: Saw that reader/writer locks were pretty bad because of
  // cache efficiency, I should go look at other alternatives if possible
  //
  // Source: https://www.youtube.com/watch?v=tND-wBBZ8RY&t=1286s
  mutable std::shared_mutex managers_mu_;
  std::unordered_map<StrategyId, std::unique_ptr<OrderManager>> managers_;
};

} // namespace quarcc
