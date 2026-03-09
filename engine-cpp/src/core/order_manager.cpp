#include <trading/core/order_manager.h>

namespace quarcc {

std::unique_ptr<OrderManager> OrderManager::CreateOrderManager(
    std::string account_id, std::unique_ptr<PositionKeeper> pk,
    std::unique_ptr<IExecutionGateway> gw, std::unique_ptr<IJournal> lj,
    std::unique_ptr<IOrderStore> os, std::unique_ptr<RiskManager> rm) {
  return std::unique_ptr<OrderManager>(
      new OrderManager(std::move(account_id), std::move(pk), std::move(gw),
                       std::move(lj), std::move(os), std::move(rm)));
}

Result<LocalOrderId>
OrderManager::processSignal(const v1::StrategySignal &signal) {
  // Create order
  std::string local_id = id_generator_->generate();
  v1::Order order = create_order_from_signal(signal);
  order.set_id(local_id);
  journal_->log(Event::ORDER_CREATED, order.DebugString(), order.id());

  StoredOrder stored;
  stored.order = order;
  stored.local_id = local_id;
  stored.status = OrderStatus::PENDING_SUBMISSION;
  stored.created_at = LogEntry::timestamp_to_string(LogEntry::now());

  if (auto store_result = order_store_->store_order(stored); !store_result) {
    journal_->log(Event::ERROR_OCCURRED, store_result.error().message_,
                  local_id);
    return std::unexpected(store_result.error());
  }

  // TODO: Risk check

  // Submit to gateway
  auto result = gateway_->submit_order(order);
  if (!result) {
    journal_->log(Event::ORDER_REJECTED, result.error().message_, local_id);
    if (auto result =
            order_store_->update_order_status(local_id, OrderStatus::REJECTED);
        !result) {
      journal_->log(Event::ERROR_OCCURRED, result.error().message_, local_id);
      return std::unexpected(result.error());
    }
    return result;
  }

  std::string broker_id = result.value();

  // Set SUBMITTED in the DB *before* add_mapping() makes this order visible to
  // the fill-poll thread. Without this ordering, the poll thread can process
  // the fill and set status=FILLED, and then this thread's update_order_status
  // call below would overwrite it back to SUBMITTED, permanently sticking the
  // order.
  if (auto result =
          order_store_->update_order_status(local_id, OrderStatus::SUBMITTED);
      !result) {
    journal_->log(Event::ERROR_OCCURRED, result.error().message_, local_id);
    return std::unexpected(result.error());
  }

  id_mapper_->add_mapping(local_id, broker_id);

  if (auto result = order_store_->update_broker_id(local_id, broker_id);
      !result) {
    journal_->log(Event::ERROR_OCCURRED, result.error().message_, local_id);
    return std::unexpected(result.error());
  }

  std::string log_data = "Local: " + local_id + ", Broker: " + broker_id;
  journal_->log(Event::ORDER_SUBMITTED, log_data, local_id);

  return local_id;
}

Result<std::monostate>
OrderManager::processSignal(const v1::CancelSignal &signal) {
  std::string local_id = signal.order_id();

  auto broker_id = id_mapper_->get_broker_id(local_id);
  if (!broker_id) {
    return std::unexpected(Error{"Cannot find broker ID for order: " + local_id,
                                 ErrorType::Error});
  }

  auto result = gateway_->cancel_order(*broker_id);

  if (result) {
    journal_->log(Event::ORDER_CANCELLED, "Cancelled", local_id);
    // id_mapper_->remove_mapping(local_id); TODO: Removal after a grace period
    // (to wait for the execution to complete)
    id_mapper_->remove_mapping(local_id);
    if (auto result =
            order_store_->update_order_status(local_id, OrderStatus::CANCELLED);
        !result) {
      journal_->log(Event::ERROR_OCCURRED, result.error().message_, local_id);
      return std::unexpected(result.error());
    }
  }

  return result;
}

Result<LocalOrderId>
OrderManager::processSignal(const v1::ReplaceSignal &signal) {
  std::string old_local_id = signal.order_id();

  auto old_broker_id = id_mapper_->get_broker_id(old_local_id);
  if (!old_broker_id) {
    return std::unexpected(Error{
        "Cannot find broker ID for order: " + old_local_id, ErrorType::Error});
  }

  std::string new_local_id = id_generator_->generate();

  v1::Order new_order = create_order_from_signal(signal);
  new_order.set_id(new_local_id);

  journal_->log(Event::ORDER_REPLACED,
                "Replacing " + old_local_id + " with " + new_local_id,
                new_local_id);

  auto result = gateway_->replace_order(*old_broker_id, new_order);
  if (!result) {
    journal_->log(Event::ORDER_REJECTED, result.error().message_, new_local_id);
    return std::unexpected(result.error());
  }

  std::string new_broker_id = result.value();
  id_mapper_->remove_mapping(old_local_id);
  id_mapper_->add_mapping(new_local_id, new_broker_id);

  if (auto result = order_store_->update_order_status(old_local_id,
                                                      OrderStatus::REPLACED);
      !result) {
    journal_->log(Event::ERROR_OCCURRED, result.error().message_, old_local_id);
    return std::unexpected(result.error());
  }

  StoredOrder stored;
  stored.order = new_order;
  stored.local_id = new_local_id;
  stored.broker_id = new_broker_id;
  stored.status = OrderStatus::SUBMITTED;
  stored.created_at = LogEntry::timestamp_to_string(LogEntry::now());

  if (auto store_result = order_store_->store_order(stored); !store_result) {
    journal_->log(Event::ERROR_OCCURRED, store_result.error().message_,
                  new_local_id);
    return std::unexpected(store_result.error());
  }

  std::string log_data = std::format("Old: {} -> New: {} (Broker: {})",
                                     old_local_id, new_local_id, new_broker_id);

  journal_->log(Event::ORDER_SUBMITTED, log_data, new_local_id);

  return new_local_id;
}

// TradingEngine::Run(). Asks the gateway for any fills that arrived since the
// last poll, then for each ExecutionReport:
//   1. Resolves the local order ID via the bidirectional ID mapper.
//   2. Fetches the stored order to compare filled vs original quantity.
//   3. Updates fill info (quantity, avg price) in the order store.
//   4. Sets the order status to FILLED or PARTIALLY_FILLED.
//   5. Calls position_keeper_->on_fill() so positions stay up to date.
//   6. Journals the event.
//   7. Removes fully-filled orders from the ID mapper (they're terminal).
void OrderManager::process_fills() {
  auto fills = gateway_->get_fills();

  // Prepend any fills that were deferred last cycle due to the submit/fill
  // race (mapping not yet established when the fill arrived).
  fills.insert(fills.begin(), deferred_fills_.begin(), deferred_fills_.end());
  deferred_fills_.clear();

  for (const auto &fill : fills) {
    const std::string &broker_id = fill.broker_order_id();

    // 1. Resolve broker → local ID
    auto local_id_opt = id_mapper_->get_local_id(broker_id);
    if (!local_id_opt) {
      // The mapping may not be established yet (submit_order returned but
      // add_mapping hasn't run). Retry next cycle instead of discarding.
      deferred_fills_.push_back(fill);
      continue;
    }
    const std::string &local_id = *local_id_opt;

    // 2. Fetch the stored order to determine full vs partial fill
    auto stored = order_store_->get_order(local_id);
    if (!stored) {
      journal_->log(Event::ERROR_OCCURRED,
                    "Cannot find stored order for local_id: " + local_id,
                    local_id);
      continue;
    }

    const double filled_qty = fill.filled_quantity();
    const double original_qty = stored->order.quantity();

    // 3. Persist fill details
    if (auto r = order_store_->update_fill_info(local_id, filled_qty,
                                                fill.avg_fill_price());
        !r) {
      journal_->log(Event::ERROR_OCCURRED, r.error().message_, local_id);
    }

    // BUG TODO: filled_qty = already filled qty + new filled_qty
    // 4. Determine and persist the new order status
    const bool fully_filled =
        (stored->filled_quantity + filled_qty >= original_qty);
    const OrderStatus new_status =
        fully_filled ? OrderStatus::FILLED : OrderStatus::PARTIALLY_FILLED;

    if (auto r = order_store_->update_order_status(local_id, new_status); !r) {
      journal_->log(Event::ERROR_OCCURRED, r.error().message_, local_id);
    }

    // 5. Update in-memory position
    position_keeper_->on_fill(fill.symbol(), filled_qty, fill.avg_fill_price(),
                              fill.side());

    // 6. Journal the event
    const std::string log_data =
        std::format("Filled: {} / {} @ avg={}", filled_qty, original_qty,
                    fill.avg_fill_price());

    journal_->log(fully_filled ? Event::ORDER_FILLED
                               : Event::ORDER_PARTIALLY_FILLED,
                  log_data, local_id);

    // 7. Remove fully-filled orders from the mapper — they are terminal
    if (fully_filled)
      id_mapper_->remove_mapping(local_id);
  }
}

// Iterates every open order from the order store, attempts a gateway
// cancellation for each one that has a broker ID, and updates the store and
// journal regardless of whether the gateway call succeeds (best-effort during
// an emergency shutdown).
void OrderManager::cancel_all(const std::string &reason,
                              const std::string &initiated_by) {
  const std::string ks_data =
      "Kill switch activated by: " + initiated_by + ". Reason: " + reason;
  journal_->log(Event::KILL_SWITCH_ACTIVATED, ks_data);

  const auto open_orders = order_store_->get_open_orders();
  for (const auto &stored : open_orders) {
    if (!stored.broker_id)
      continue;

    if (auto r = gateway_->cancel_order(*stored.broker_id); r) {
      if (auto r = order_store_->update_order_status(stored.local_id,
                                                     OrderStatus::CANCELLED);
          !r) {
        journal_->log(Event::ERROR_OCCURRED, r.error().message_,
                      stored.local_id);
        continue;
      }

      id_mapper_->remove_mapping(stored.local_id);
      journal_->log(Event::ORDER_CANCELLED, "Cancelled by kill switch",
                    stored.local_id);
    } else {
      journal_->log(Event::ERROR_OCCURRED,
                    "Failed to cancel during kill switch: " +
                        r.error().message_,
                    stored.local_id);
    }
  }
}

Result<v1::Position>
OrderManager::get_position(const std::string &symbol) const {
  return position_keeper_->get_position(symbol);
}

v1::PositionList OrderManager::get_all_positions() const {
  return position_keeper_->get_all_positions();
}

OrderManager::OrderManager(std::string account_id,
                           std::unique_ptr<PositionKeeper> pk,
                           std::unique_ptr<IExecutionGateway> gw,
                           std::unique_ptr<IJournal> lj,
                           std::unique_ptr<IOrderStore> os,
                           std::unique_ptr<RiskManager> rm)
    : account_id_(std::move(account_id)), position_keeper_(std::move(pk)),
      gateway_(std::move(gw)), journal_(std::move(lj)),
      order_store_(std::move(os)), risk_manager_(std::move(rm)),
      id_generator_(std::make_unique<OrderIdGenerator>()),
      id_mapper_(std::make_unique<OrderIdMapper>()) {}

v1::Order
OrderManager::create_order_from_signal(const v1::StrategySignal &signal) {
  v1::Order order;
  order.set_symbol(signal.symbol());
  order.set_side(signal.side());
  order.set_quantity(signal.target_quantity());
  order.set_type(v1::OrderType::MARKET);
  order.set_account_id("quarcc.Rifat");
  order.set_created_at(get_current_time());
  order.set_time_in_force(v1::TimeInForce::DAY);
  order.set_strategy_id(signal.strategy_id());
  // order.metadata(). = signal.metadata(); // TODO: Copy metadata from signal
  return order;
}

v1::Order
OrderManager::create_order_from_signal(const v1::ReplaceSignal &signal) {
  v1::Order order;
  order.set_symbol(signal.symbol());
  order.set_side(signal.side());
  order.set_quantity(signal.target_quantity());
  order.set_type(v1::OrderType::MARKET);
  order.set_account_id("quarcc.Rifat");
  order.set_created_at(get_current_time());
  order.set_time_in_force(v1::TimeInForce::DAY);
  order.set_strategy_id(signal.strategy_id());
  // order.metadata(). = signal.metadata(); // TODO: Copy metadata from signal
  return order;
}

} // namespace quarcc
