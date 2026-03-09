#pragma once

#include "order.pb.h"
#include "strategy_signal.pb.h"

#include <trading/core/position_keeper.h>
#include <trading/interfaces/i_execution_gateway.h>
#include <trading/interfaces/i_journal.h>
#include <trading/interfaces/i_order_store.h>
#include <trading/interfaces/i_risk_check.h>
#include <trading/utils/order_id_generator.h>
#include <trading/utils/order_id_types.h>
#include <trading/utils/result.h>

namespace quarcc {

class OrderManager {
public:
  OrderManager(const OrderManager &) = delete;
  OrderManager &operator=(const OrderManager &) = delete;
  OrderManager(OrderManager &&) = delete;
  OrderManager &operator=(OrderManager &&) = delete;

  static std::unique_ptr<OrderManager> CreateOrderManager(
      std::string account_id, std::unique_ptr<PositionKeeper> pk,
      std::unique_ptr<IExecutionGateway> gw, std::unique_ptr<IJournal> lj,
      std::unique_ptr<IOrderStore> os, std::unique_ptr<RiskManager> rm);

  // BUG TODO: Investigate fills and partial fills, weird behavior noticed
  // through journal/order dbs
  Result<LocalOrderId> processSignal(const v1::StrategySignal &signal);
  Result<std::monostate> processSignal(const v1::CancelSignal &signal);
  Result<LocalOrderId> processSignal(const v1::ReplaceSignal &signal);

  // Poll the gateway for new fills and apply them to the order store and
  // position keeper. Called periodically from TradingEngine::Run().
  void process_fills();

  // Cancel every open order through the gateway and journal the kill-switch
  // event. Called from TradingEngine::ActivateKillSwitch().
  void cancel_all(const std::string &reason, const std::string &initiated_by);

  // Position queries delegated to the internal PositionKeeper.
  Result<v1::Position> get_position(const std::string &symbol) const;
  v1::PositionList get_all_positions() const;

private:
  OrderManager(std::string account_id, std::unique_ptr<PositionKeeper> pk,
               std::unique_ptr<IExecutionGateway> gw,
               std::unique_ptr<IJournal> lj, std::unique_ptr<IOrderStore> os,
               std::unique_ptr<RiskManager> rm);

  v1::Order create_order_from_signal(const v1::StrategySignal &signal);
  v1::Order create_order_from_signal(const v1::ReplaceSignal &signal);

private:
  std::string account_id_;

  std::unique_ptr<PositionKeeper> position_keeper_;
  std::unique_ptr<IExecutionGateway> gateway_;
  std::unique_ptr<IJournal> journal_;
  std::unique_ptr<IOrderStore> order_store_;
  std::unique_ptr<RiskManager> risk_manager_;
  std::unique_ptr<OrderIdGenerator> id_generator_;
  std::unique_ptr<OrderIdMapper> id_mapper_;

  // Fills whose broker_order_id wasn't in the mapper yet (submit/fill race).
  // Re-processed at the start of the next process_fills() call.
  std::vector<v1::ExecutionReport> deferred_fills_;
};

} // namespace quarcc
