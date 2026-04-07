#include <trading/gateways/grpc_gateway.h>

#include <format>

namespace quarcc {

GrpcGateway::GrpcGateway(std::string strategy_id,
                         std::shared_ptr<AdapterConnection> conn)
    : strategy_id_(std::move(strategy_id)), conn_(std::move(conn)) {}

void GrpcGateway::set_fill_handler(FillHandler handler) {
  fill_handler_ = std::move(handler);
}

void GrpcGateway::set_reject_handler(RejectHandler /*handler*/) {}

void GrpcGateway::start() {
  stream_thread_ =
      std::jthread([this](std::stop_token st) { run_fill_stream(st); });
}

void GrpcGateway::stop() {
  // 1. Signal the stream thread to exit instead of reconnecting.
  stream_thread_.request_stop();

  // 2. Cancel the in-flight StreamFills RPC so Read() unblocks immediately.
  {
    std::lock_guard lk{ctx_mu_};
    if (ctx_)
      ctx_->TryCancel();
  }

  // 3. Wake the stream thread if it is in the reconnect sleep.
  reconnect_cv_.notify_all();
}

Result<BrokerOrderId> GrpcGateway::submit_order(const v1::Order &order) {
  v1::AdapterSubmitRequest req;
  req.set_strategy_id(strategy_id_);
  *req.mutable_order() = order;

  v1::AdapterSubmitResponse resp;
  grpc::ClientContext ctx;
  const grpc::Status status = conn_->stub->SubmitOrder(&ctx, req, &resp);

  if (!status.ok())
    return std::unexpected(
        Error{std::format("SubmitOrder RPC failed: {}", status.error_message()),
              ErrorType::Error});

  if (!resp.accepted())
    return std::unexpected(
        Error{resp.rejection_reason(), ErrorType::FailedOrder});

  return resp.broker_order_id();
}

Result<std::monostate> GrpcGateway::cancel_order(const BrokerOrderId &id) {
  v1::AdapterCancelRequest req;
  req.set_strategy_id(strategy_id_);
  req.set_broker_order_id(id);

  v1::AdapterCancelResponse resp;
  grpc::ClientContext ctx;
  const grpc::Status status = conn_->stub->CancelOrder(&ctx, req, &resp);

  if (!status.ok())
    return std::unexpected(
        Error{std::format("CancelOrder RPC failed: {}", status.error_message()),
              ErrorType::Error});

  if (!resp.accepted())
    return std::unexpected(
        Error{resp.rejection_reason(), ErrorType::FailedOrder});

  return std::monostate{};
}

Result<BrokerOrderId> GrpcGateway::replace_order(const BrokerOrderId &id,
                                                 const v1::Order &new_order) {
  v1::AdapterReplaceRequest req;
  req.set_strategy_id(strategy_id_);
  req.set_broker_order_id(id);
  *req.mutable_new_order() = new_order;

  v1::AdapterReplaceResponse resp;
  grpc::ClientContext ctx;
  const grpc::Status status = conn_->stub->ReplaceOrder(&ctx, req, &resp);

  if (!status.ok())
    return std::unexpected(Error{
        std::format("ReplaceOrder RPC failed: {}", status.error_message()),
        ErrorType::Error});

  if (!resp.accepted())
    return std::unexpected(
        Error{resp.rejection_reason(), ErrorType::FailedOrder});

  return resp.new_broker_order_id();
}

void GrpcGateway::run_fill_stream(std::stop_token st) {
  using namespace std::chrono_literals;

  while (!st.stop_requested()) {
    // Creates a new context every time it restarts
    {
      std::lock_guard lk{ctx_mu_};
      ctx_ = std::make_unique<grpc::ClientContext>();
    }

    v1::StreamFillsRequest req;
    req.set_strategy_id(strategy_id_);

    auto reader = conn_->stub->StreamFills(ctx_.get(), req);

    v1::ExecutionReport report;
    while (reader->Read(&report)) {
      // Deduplication logic
      // If execution_id is empty, means the broker probably doesn't provide one
      // so we can skip the deduplication logic for it
      const std::string &exec_id = report.execution_id();
      if (!exec_id.empty()) {
        if (seen_execution_ids_.contains(exec_id))
          continue;
        seen_execution_ids_.insert(exec_id);
      }

      if (fill_handler_) [[likely]]
        fill_handler_(report);
    }

    reader->Finish();
    if (st.stop_requested())
      break;

    // If there's an unintentional disconnection, it waits 2s then reconnects
    // Still listens on the stop_token so if stop() is called it still works
    // fine
    {
      std::unique_lock lk{reconnect_mu_};
      reconnect_cv_.wait_for(lk, st, 2s, [] { return false; });
    }
  }

  std::lock_guard lk{ctx_mu_};
  ctx_.reset();
}

} // namespace quarcc
