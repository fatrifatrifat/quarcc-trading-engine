#pragma once

#include <trading/gateways/adapter_connection.h>
#include <trading/interfaces/i_execution_gateway.h>

#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace quarcc {

// IExecutionGateway implementation that forwards orders to a remote Python
// adapter process via GatewayAdapterService gRPC
//
// One GrpcGateway per OrderManager. Multiple GrpcGateway instances may share
// the same AdapterConnection (same venue + account_id), distinguished by
// strategy_id which routes fills back to the correct StreamFills stream.
//
// Threading:
//   submit_order/cancel_order/replace_order are called from the
//   OrderManager signal-processing thread. gRPC stubs are thread safe so
//   concurrent calls are chill
//
//   stream_thread_ runs run_fill_stream(), which opens the StreamFills RPC,
//   reads execution reports, deduplicates by execution_id and invokes
//   fill_handler_
//   On RPC failure it reconnects with a 2s delay (interruptible by stop())
class GrpcGateway final : public IExecutionGateway {
public:
  GrpcGateway(std::string strategy_id, std::shared_ptr<AdapterConnection> conn);

  Result<BrokerOrderId> submit_order(const v1::Order &order) override;
  Result<std::monostate> cancel_order(const BrokerOrderId &id) override;
  Result<BrokerOrderId> replace_order(const BrokerOrderId &id,
                                      const v1::Order &new_order) override;

  void set_fill_handler(FillHandler handler) override;
  // Both successful and rejected fills use the fill handler
  // So the reject handler is kinda useless right now
  // TODO: Maybe make it useful?
  void set_reject_handler(RejectHandler handler) override;

  void start() override;
  void stop() override;

private:
  void run_fill_stream(std::stop_token st);

  std::string strategy_id_;
  std::shared_ptr<AdapterConnection> conn_;

  FillHandler fill_handler_;

  // Only used by the stream_thread_ so doesn't need a mutex or anything
  std::unordered_set<std::string> seen_execution_ids_;

  // ctx_ holds the ClientContext for the current StreamFills RPC
  std::mutex ctx_mu_;
  std::unique_ptr<grpc::ClientContext> ctx_;

  // Used to make the reconnect sleep interruptible by stop()
  std::condition_variable_any reconnect_cv_;
  std::mutex reconnect_mu_;

  std::jthread stream_thread_;
};

} // namespace quarcc
