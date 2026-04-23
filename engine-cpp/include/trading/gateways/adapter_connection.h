#pragma once

#include "gateway_adapter_service.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>

namespace quarcc {

// Shared connection created by AdapterManager that GrpcGateway &
// GrpcMarketDataFeed potentially share based on their venue and account id
struct AdapterConnection {
  std::string address;
  std::shared_ptr<grpc::Channel> channel;
  std::shared_ptr<v1::GatewayAdapterService::Stub> stub;
};

inline std::shared_ptr<AdapterConnection>
make_adapter_connection(const std::string &address) {
  auto conn = std::make_shared<AdapterConnection>();
  conn->address = address;
  conn->channel =
      grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
  conn->stub = v1::GatewayAdapterService::NewStub(conn->channel);
  return conn;
}

} // namespace quarcc
