#pragma once

#include <trading/gateways/adapter_connection.h>
#include <trading/interfaces/i_market_data_feed.h>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace quarcc {

// IMarketDataFeed implementation that streams ticks and bars from a remote
// Python adapter process via GatewayAdapterService::StreamMarketData
//
// Usage:
//   1. Set handlers via set_bar_handler/set_tick_handler (done by
//   FeedRegistry)
//   2. Call subscribe() for each symbol+period (done by FeedRegistry)
//   3. Call start() which opens the StreamMarketData RPC with all buffered
//      subscriptions. Subscriptions cannot be changed after start()
//   4. Call stop() to cancel the RPC and join the stream thread
//
// Logic very similar to GrpcGateway
class GrpcMarketDataFeed final : public IMarketDataFeed {
public:
  explicit GrpcMarketDataFeed(std::shared_ptr<AdapterConnection> conn);

  void start() override;
  void stop() override;

  // Buffers the instruments to subscribe to when start() is called
  void subscribe(const Symbol &symbol, const BarPeriod &period) override;
  // All subscritions are static, so unsubscribing doesn't really exists unless
  // the client gets killed
  // TODO: DYNAMIC SUBSCRIPTIONS (and also strategies)
  void unsubscribe(const Symbol &symbol, const BarPeriod &period) override;

  void set_bar_handler(std::function<void(const Bar &)> handler) override;
  void set_tick_handler(std::function<void(const Tick &)> handler) override;

private:
  void run_stream(std::stop_token st);

  std::shared_ptr<AdapterConnection> conn_;
  std::function<void(const Bar &)> bar_handler_;
  std::function<void(const Tick &)> tick_handler_;

  std::vector<v1::SymbolSubscription> subscriptions_;

  std::mutex ctx_mu_;
  std::unique_ptr<grpc::ClientContext> ctx_;

  std::condition_variable_any reconnect_cv_;
  std::mutex reconnect_mu_;

  std::jthread stream_thread_;
};

} // namespace quarcc
