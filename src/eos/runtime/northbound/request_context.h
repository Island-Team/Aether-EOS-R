/// Shared request-correlation handling for EOS gRPC services.
#pragma once

#include <grpcpp/server_context.h>

#include <iterator>
#include <string>

#include "src/eos/lib/log.h"

namespace eos::runtime::northbound {

inline grpc::Status ResolveRequestId(grpc::ServerContext* context, std::string* request_id) {
  const auto& metadata = context->client_metadata();
  const auto range = metadata.equal_range(eos::lib::kRequestIdMetadataKey);
  if (range.first == range.second) {
    *request_id = eos::lib::ResolveRequestId();
    return grpc::Status::OK;
  }
  auto next = range.first;
  ++next;
  if (next != range.second) {
    return {grpc::StatusCode::INVALID_ARGUMENT, "x-request-id must be supplied at most once"};
  }
  const std::string supplied(range.first->second.data(), range.first->second.size());
  try {
    *request_id = eos::lib::ResolveRequestId(supplied);
    return grpc::Status::OK;
  } catch (const std::invalid_argument&) {
    return {grpc::StatusCode::INVALID_ARGUMENT, "x-request-id is invalid"};
  }
}

}  // namespace eos::runtime::northbound
