#include <trading/grpc/grpc_server.h>
#include <trading/utils/order_id_generator.h>

#include <iostream>
#include <utility>

namespace quarcc {

gRPCServer::gRPCServer(std::string server_address,
                       IExecutionServiceHandler &handler)
    : server_address_(std::move(server_address)), handler_(&handler) {}

void gRPCServer::start() {
  service_ = std::make_unique<ExecutionServiceImpl>(this);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(server_address_, grpc::InsecureServerCredentials());
  builder.RegisterService(service_.get());

  server_ = builder.BuildAndStart();
  std::cout << "gRPC server listening on " << server_address_ << std::endl;
}

void gRPCServer::wait() {
  if (server_)
    server_->Wait();
}

void gRPCServer::shutdown() {
  if (server_)
    server_->Shutdown();
}

gRPCServer::ExecutionServiceImpl::ExecutionServiceImpl(gRPCServer *owner)
    : owner_(owner) {}

grpc::Status gRPCServer::ExecutionServiceImpl::SubmitSignal(
    grpc::ServerContext *context, const v1::StrategySignal *request,
    v1::SubmitSignalResponse *response) {

  if (!owner_ || !owner_->handler_) {
    response->set_accepted(false);
    response->set_rejection_reason("Server handler not initialized");
    return grpc::Status(grpc::ABORTED, "Server handler not initialized");
  }

  std::cout << "Received signal from " << context->peer() << " - "
            << request->strategy_id() << " " << request->side() << " "
            << request->symbol() << std::endl;

  auto r = owner_->handler_->SubmitSignal(*request);

  if (!r) {
    response->set_accepted(false);
    response->set_rejection_reason(r.error().message_);
    return grpc::Status(grpc::INVALID_ARGUMENT, r.error().message_);
  }

  response->set_accepted(true);
  response->set_order_id(r.value());

  return grpc::Status::OK;
}

grpc::Status gRPCServer::ExecutionServiceImpl::CancelOrder(
    grpc::ServerContext *context, const v1::CancelSignal *request,
    v1::CancelOrderResponse *response) {

  response->set_received_at(get_current_time());

  if (!owner_ || !owner_->handler_) {
    response->set_accepted(false);
    response->set_rejection_reason("Server handler not initialized");
    return grpc::Status(grpc::ABORTED, "Server handler not initialized");
  }

  std::cout << "Cancel signal received from " << context->peer() << " - "
            << request->strategy_id() << " " << request->order_id()
            << std::endl;

  auto r = owner_->handler_->CancelOrder(*request);

  if (!r) {
    response->set_accepted(false);
    response->set_rejection_reason(r.error().message_);
    return grpc::Status(grpc::INVALID_ARGUMENT, r.error().message_);
  }

  response->set_accepted(true);

  return grpc::Status::OK;
}

grpc::Status gRPCServer::ExecutionServiceImpl::ReplaceOrder(
    grpc::ServerContext *context, const v1::ReplaceSignal *request,
    v1::ReplaceOrderResponse *response) {

  response->set_received_at(get_current_time());

  if (!owner_ || !owner_->handler_) {
    response->set_accepted(false);
    response->set_rejection_reason("Server handler not initialized");
    return grpc::Status(grpc::ABORTED, "Server handler not initialized");
  }

  std::cout << "Replace signal received from " << context->peer() << " - "
            << request->strategy_id() << " " << request->side() << " "
            << request->symbol() << request->order_id() << std::endl;

  auto r = owner_->handler_->ReplaceOrder(*request);

  if (!r) {
    response->set_accepted(false);
    response->set_rejection_reason(r.error().message_);
    return grpc::Status(grpc::INVALID_ARGUMENT, r.error().message_);
  }

  response->set_accepted(true);
  response->set_order_id(r.value());

  return grpc::Status::OK;
}

grpc::Status gRPCServer::ExecutionServiceImpl::StreamSignals(
    grpc::ServerContext *context,
    grpc::ServerReaderWriter<v1::SubmitSignalResponse, v1::StrategySignal>
        *stream) {

  if (!owner_ || !owner_->handler_) {
    return grpc::Status(grpc::FAILED_PRECONDITION,
                        "Server handler not initialized");
  }

  std::cout << "Client " << context->peer() << " opened signal stream";

  v1::StrategySignal signal;
  while (stream->Read(&signal)) {
    auto r = owner_->handler_->SubmitSignal(signal);

    v1::SubmitSignalResponse response;
    if (!r) {
      response.set_accepted(false);
      response.set_rejection_reason(r.error().message_);
    } else {
      response.set_accepted(true);
      response.set_order_id(r.value());
    }

    stream->Write(response);
  }

  std::cout << "Client " << context->peer() << " closed signal stream\n";
  return grpc::Status::OK;
}

grpc::Status gRPCServer::ExecutionServiceImpl::GetPosition(
    grpc::ServerContext *context, const v1::GetPositionRequest *request,
    v1::Position *response) {

  if (!owner_ || !owner_->handler_) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "Server handler not initialized");
  }

  std::cout << "Received position request from " << context->peer()
            << std::endl;

  auto r = owner_->handler_->GetPosition(*request);
  if (!r)
    return grpc::Status(grpc::StatusCode::INTERNAL, r.error().message_);

  *response = std::move(r.value());
  return grpc::Status::OK;
}

grpc::Status
gRPCServer::ExecutionServiceImpl::GetAllPositions(grpc::ServerContext *context,
                                                  const v1::Empty *request,
                                                  v1::PositionList *response) {

  if (!owner_ || !owner_->handler_) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "Server handler not initialized");
  }

  std::cout << "Received position request for all from " << context->peer()
            << std::endl;

  auto r = owner_->handler_->GetAllPositions(*request);
  if (!r)
    return grpc::Status(grpc::StatusCode::INTERNAL, r.error().message_);

  *response = std::move(r.value());
  return grpc::Status::OK;
}

grpc::Status gRPCServer::ExecutionServiceImpl::ActivateKillSwitch(
    grpc::ServerContext *context, const v1::KillSwitchRequest *request,
    v1::Empty *response) {

  (void)response;

  if (!owner_ || !owner_->handler_) {
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                        "Server handler not initialized");
  }

  std::cout << "Received kill switch request from " << context->peer()
            << std::endl;

  auto r = owner_->handler_->ActivateKillSwitch(*request);
  if (!r)
    return grpc::Status(grpc::StatusCode::INTERNAL, r.error().message_);

  return grpc::Status::OK;
}

} // namespace quarcc
