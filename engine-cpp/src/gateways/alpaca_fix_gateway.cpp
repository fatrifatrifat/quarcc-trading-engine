#include <trading/gateways/alpaca_fix_gateway.h>

namespace quarcc {

AlpacaGateway::AlpacaGateway() : trade_(env_) {}

Result<BrokerOrderId> AlpacaGateway::submit_order(const v1::Order &order) {
  const auto o = order_to_alpaca_order(order);
  auto resp = trade_.SubmitOrder(o);
  if (!resp) {
    return std::unexpected(Error{resp.error().message, ErrorType::Error});
  }

  const BrokerOrderId &broker_id = resp->id;

  {
    std::lock_guard lk{orders_mutex_};
    pending_orders_[broker_id] = order;
  }

  return broker_id;
}

Result<std::monostate>
AlpacaGateway::cancel_order(const BrokerOrderId &orderId) {
  auto resp = trade_.DeleteOrderByID(orderId);
  if (!resp) {
    return std::unexpected(Error{resp.error().message, ErrorType::Error});
  }

  {
    std::lock_guard lk{orders_mutex_};
    pending_orders_.erase(orderId);
  }

  return std::monostate{};
}

Result<BrokerOrderId> AlpacaGateway::replace_order(const BrokerOrderId &orderId,
                                                   const v1::Order &new_order) {
  const alpaca::ReplaceOrderParam replace{
      .qty = std::make_optional(new_order.quantity()),
  };

  auto resp = trade_.ReplaceOrderByID(orderId, replace);
  if (!resp) {
    return std::unexpected(Error{resp.error().message, ErrorType::Error});
  }

  const BrokerOrderId &broker_id = resp->id;
  {
    std::lock_guard lk{orders_mutex_};
    pending_orders_.erase(orderId);
    pending_orders_[broker_id] = new_order;
  }

  return broker_id;
}

std::vector<v1::ExecutionReport> AlpacaGateway::get_fills() {
  std::vector<v1::ExecutionReport> fills;
  // Collect IDs to remove *after* the loop — erasing inside a range-for loop
  // over an unordered_map invalidates the iterator and is undefined behaviour.
  std::vector<BrokerOrderId> to_erase;

  std::lock_guard lk{orders_mutex_};

  for (const auto &[broker_id, _] : pending_orders_) {
    auto resp = trade_.GetOrderByID(broker_id);
    if (!resp)
      continue;

    const auto &order = resp.value();
    const double filled_qty = stod(order.filledQty);
    const double avg_fill_price = stod(*order.filledAvgPrice);

    // Skip orders that haven't been filled at all yet.
    if (filled_qty <= 0.0)
      continue;

    v1::ExecutionReport fill;
    fill.set_broker_order_id(broker_id);
    fill.set_symbol(order.symbol.value());
    fill.set_side((order.side == alpaca::OrderSide::buy) ? v1::Side::BUY
                                                         : v1::Side::SELL);
    fill.set_filled_quantity(filled_qty);
    fill.set_fill_time(get_current_time());
    fill.set_avg_fill_price(avg_fill_price);
    fills.push_back(std::move(fill));

    to_erase.push_back(broker_id);
  }

  // Safe to erase now that we're done iterating.
  for (const auto &id : to_erase)
    pending_orders_.erase(id);

  return fills;
}

constexpr alpaca::OrderRequestParam
AlpacaGateway::order_to_alpaca_order(const v1::Order &order) const {
  return alpaca::OrderRequestParam{
      .symbol = order.symbol(),
      .amt = alpaca::ShareAmount{alpaca::Quantity{order.quantity()}},
      .side = order_enum_conversion(order.side()),
      .type = order_enum_conversion(order.type()),
      .timeInForce = order_enum_conversion(order.time_in_force()),
  };
}

constexpr alpaca::OrderSide
AlpacaGateway::order_enum_conversion(v1::Side type) const {
  switch (static_cast<int>(type)) {
  case v1::Side::BUY:
    return alpaca::OrderSide::buy;
  case v1::Side::SELL:
    return alpaca::OrderSide::sell;
  default:
    throw std::invalid_argument("Unknown v1::Side");
  }
}

constexpr alpaca::OrderType
AlpacaGateway::order_enum_conversion(v1::OrderType type) const {
  switch (static_cast<int>(type)) {
  case v1::OrderType::MARKET:
    return alpaca::OrderType::market;
  case v1::OrderType::LIMIT:
    return alpaca::OrderType::limit;
  case v1::OrderType::STOP_LIMIT:
    return alpaca::OrderType::stop_limit;
  case v1::OrderType::STOP:
    return alpaca::OrderType::stop;
  default:
    throw std::invalid_argument("Unknown v1::OrderType");
  }
}

constexpr alpaca::OrderTimeInForce
AlpacaGateway::order_enum_conversion(v1::TimeInForce type) const {
  switch (static_cast<int>(type)) {
  case v1::TimeInForce::DAY:
    return alpaca::OrderTimeInForce::day;
  case v1::TimeInForce::FOK:
    return alpaca::OrderTimeInForce::fok;
  case v1::TimeInForce::GTC:
    return alpaca::OrderTimeInForce::gtc;
  case v1::TimeInForce::IOC:
    return alpaca::OrderTimeInForce::ioc;
  default:
    throw std::invalid_argument("Unknown v1::TimeInForce");
  }
}

} // namespace quarcc
