#include <trading/gateways/paper_trading_gateway.h>

namespace quarcc {

PaperGateway::PaperGateway() : id_gen_("BROKER") {}

Result<BrokerOrderId> PaperGateway::submit_order(const v1::Order &order) {
  // "Submit order"
  const BrokerOrderId broker_id = id_gen_.generate();

  {
    std::lock_guard lk{orders_mutex_};
    pending_orders_[broker_id] = order;
  }

  return broker_id;
}

Result<std::monostate>
PaperGateway::cancel_order(const BrokerOrderId &orderId) {
  {
    std::lock_guard lk{orders_mutex_};
    pending_orders_.erase(orderId);
  }

  return std::monostate{};
}

Result<BrokerOrderId> PaperGateway::replace_order(const BrokerOrderId &orderId,
                                                  const v1::Order &new_order) {
  const BrokerOrderId broker_id = id_gen_.generate();
  {
    std::lock_guard lk{orders_mutex_};
    pending_orders_.erase(orderId);
    pending_orders_[broker_id] = new_order;
  }

  return broker_id;
}

std::vector<v1::ExecutionReport> PaperGateway::get_fills() {
  std::vector<v1::ExecutionReport> fills;
  std::vector<BrokerOrderId> to_erase;

  std::lock_guard lk{orders_mutex_};

  for (const auto &[broker_id, order] : pending_orders_) {
    v1::ExecutionReport fill;
    fill.set_broker_order_id(broker_id);
    fill.set_symbol(order.symbol());
    fill.set_side(order.side());
    fill.set_filled_quantity(order.quantity());
    fill.set_fill_time(get_current_time());
    fills.push_back(std::move(fill));

    to_erase.push_back(broker_id);
  }

  for (const auto &id : to_erase)
    pending_orders_.erase(id);

  return fills;
}

void PaperGateway::advance_prices() noexcept {
  constexpr double spread_bps = 5.0;
  for (auto &[symbol, ps] : prices_) {
    double mid = (ps.bid + ps.ask) / 2.0;
    double move = mid * config_.price_volatility * price_dist_(rng_);
    mid = std::max(mid + move, 0.01);
    double half_spread = mid * (spread_bps / 20000.0);
    ps.bid = mid - half_spread;
    ps.ask = mid + half_spread;
  }
}

double PaperGateway::get_or_init_price(const std::string &symbol) noexcept {
  if (!prices_.contains(symbol)) {
    double mid = config_.initial_price;
    double half_spread = mid * 0.00025;
    prices_[symbol] = {mid - half_spread, mid + half_spread};
  }

  return (prices_[symbol].bid + prices_[symbol].ask) / 2.0;
}

// TODO: MarketDataService that would call this
void PaperGateway::update_price(const std::string &symbol, double bid,
                                double ask) noexcept {
  std::lock_guard lk{orders_mutex_};
  prices_[symbol] = {bid, ask};
}

} // namespace quarcc
