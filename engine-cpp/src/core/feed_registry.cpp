#include <trading/core/feed_registry.h>
#include <trading/gateways/grpc_market_data_feed.h>
#include <trading/gateways/simulated_feed.h>

namespace quarcc {

void FeedRegistry::register_subscription(FeedKey key, Symbol symbol,
                                         BarPeriod period, OrderManager *om) {
  if (!feeds_.contains(key)) {
    auto feed = create_feed(key);

    // Whenever the IMarketDataFeed gets data, it will enqueue it to it's
    // respective subscribers
    feed->set_bar_handler([this, key](const Bar &b) { on_bar(key, b); });
    feed->set_tick_handler([this, key](const Tick &t) { on_tick(key, t); });

    feeds_.emplace(key, std::move(feed));
    if (running_)
      feeds_.at(key)->start();
  }

  feeds_.at(key)->subscribe(symbol, period);

  bar_subs_[{key, symbol, period}].push_back(om);
  tick_subs_[{key, symbol}].push_back(om);
}

void FeedRegistry::on_bar(const FeedKey &key, const Bar &bar) {
  auto it = bar_subs_.find({key, bar.symbol, bar.period});
  if (it == bar_subs_.end()) [[unlikely]]
    return;

  for (OrderManager *om : it->second)
    om->enqueue(bar);
}

void FeedRegistry::on_tick(const FeedKey &key, const Tick &tick) {
  auto it = tick_subs_.find({key, tick.symbol});
  if (it == tick_subs_.end()) [[unlikely]]
    return;

  for (OrderManager *om : it->second)
    om->enqueue(tick);
}

void FeedRegistry::start_all() {
  running_ = true;
  for (auto &[key, feed] : feeds_)
    feed->start();
}

void FeedRegistry::stop_all() {
  for (auto &[key, feed] : feeds_)
    feed->stop();
}

void FeedRegistry::register_feed(FeedKey key,
                                 std::unique_ptr<IMarketDataFeed> feed) {
  feed->set_bar_handler([this, key](const Bar &b) { on_bar(key, b); });
  feed->set_tick_handler([this, key](const Tick &t) { on_tick(key, t); });
  feeds_.emplace(key, std::move(feed));
  if (running_)
    feeds_.at(key)->start();
}

bool FeedRegistry::has_feed(const FeedKey &key) const {
  return feeds_.contains(key);
}

std::unique_ptr<IMarketDataFeed> FeedRegistry::create_feed(const FeedKey &key) {
  if (key.feed_type == "simulated")
    return std::make_unique<SimulatedFeed>();

  // grpc_adapter feeds are pre registered so we don't create them here, so it
  // shouldn't throw for them
  throw std::runtime_error("Unknown feed type: " + key.feed_type);
}

} // namespace quarcc
