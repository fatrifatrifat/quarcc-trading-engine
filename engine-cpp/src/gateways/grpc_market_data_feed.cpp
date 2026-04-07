#include <trading/gateways/grpc_market_data_feed.h>

#include <algorithm>
#include <chrono>

namespace quarcc {

namespace {

Bar bar_from_proto(const v1::BarEvent &b) {
  Bar bar;
  bar.symbol = b.symbol();
  bar.period = b.period();
  bar.open = b.open();
  bar.high = b.high();
  bar.low = b.low();
  bar.close = b.close();
  bar.volume = b.volume();
  bar.vwap = b.vwap();
  bar.ts_ns = b.timestamp_ns();
  bar.ts_recv_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  return bar;
}

Tick tick_from_proto(const v1::TickEvent &t) {
  Tick tick;
  tick.symbol = t.symbol();
  tick.bid = t.bid();
  tick.ask = t.ask();
  tick.last = t.last();
  tick.bid_size = t.bid_size();
  tick.ask_size = t.ask_size();
  tick.last_size = t.last_size();
  tick.ts_ns = t.timestamp_ns();
  tick.ts_recv_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
  return tick;
}

} // namespace

GrpcMarketDataFeed::GrpcMarketDataFeed(std::shared_ptr<AdapterConnection> conn)
    : conn_(std::move(conn)) {}

void GrpcMarketDataFeed::set_bar_handler(
    std::function<void(const Bar &)> handler) {
  bar_handler_ = std::move(handler);
}

void GrpcMarketDataFeed::set_tick_handler(
    std::function<void(const Tick &)> handler) {
  tick_handler_ = std::move(handler);
}

void GrpcMarketDataFeed::subscribe(const Symbol &symbol,
                                   const BarPeriod &period) {
  auto it = std::ranges::find_if(
      subscriptions_, [&](const auto &s) { return s.symbol() == symbol; });
  if (it == subscriptions_.end()) {
    v1::SymbolSubscription sub;
    sub.set_symbol(symbol);
    if (!period.empty())
      sub.add_bar_periods(period);
    subscriptions_.push_back(std::move(sub));
  } else {
    // If symbol is already subscribed to, check if that specific period is
    // subscribed to
    if (!period.empty()) {
      const auto &periods = it->bar_periods();
      if (std::ranges::find(periods, period) == periods.end())
        it->add_bar_periods(period);
    }
  }
}

void GrpcMarketDataFeed::unsubscribe(const Symbol &, const BarPeriod &) {}

void GrpcMarketDataFeed::start() {
  stream_thread_ = std::jthread([this](std::stop_token st) { run_stream(st); });
}

void GrpcMarketDataFeed::stop() {
  stream_thread_.request_stop();
  {
    std::lock_guard lk{ctx_mu_};
    if (ctx_)
      ctx_->TryCancel();
  }
  reconnect_cv_.notify_all();
}

// Pretty much exact same logic as GrpcGateway::run_fill_stream
void GrpcMarketDataFeed::run_stream(std::stop_token st) {
  using namespace std::chrono_literals;

  while (!st.stop_requested()) {
    {
      std::lock_guard lk{ctx_mu_};
      ctx_ = std::make_unique<grpc::ClientContext>();
    }

    v1::AdapterMarketDataRequest req;
    for (const auto &sub : subscriptions_)
      *req.add_subscriptions() = sub;

    auto reader = conn_->stub->StreamMarketData(ctx_.get(), req);

    v1::MarketDataEvent event;
    while (reader->Read(&event)) {
      switch (event.event_case()) {
      case v1::MarketDataEvent::kTick:
        if (tick_handler_) [[likely]]
          tick_handler_(tick_from_proto(event.tick()));
        break;
      case v1::MarketDataEvent::kBar:
        if (bar_handler_) [[likely]]
          bar_handler_(bar_from_proto(event.bar()));
        break;
      default:
        break;
      }
    }

    reader->Finish();

    if (st.stop_requested())
      break;

    std::unique_lock lk{reconnect_mu_};
    reconnect_cv_.wait_for(lk, st, 2s, [] { return false; });
  }

  std::lock_guard lk{ctx_mu_};
  ctx_.reset();
}

} // namespace quarcc
