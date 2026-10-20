#ifndef SRC_EOS_RUNTIME_NORTHBOUND_LOG_STATUS_H_
#define SRC_EOS_RUNTIME_NORTHBOUND_LOG_STATUS_H_

#include <grpcpp/grpcpp.h>

namespace eos::runtime::northbound {

inline const char* GrpcStatusName(grpc::StatusCode code) {
  switch (code) {
    case grpc::StatusCode::OK:
      return "OK";
    case grpc::StatusCode::CANCELLED:
      return "CANCELLED";
    case grpc::StatusCode::UNKNOWN:
      return "UNKNOWN";
    case grpc::StatusCode::INVALID_ARGUMENT:
      return "INVALID_ARGUMENT";
    case grpc::StatusCode::DEADLINE_EXCEEDED:
      return "DEADLINE_EXCEEDED";
    case grpc::StatusCode::NOT_FOUND:
      return "NOT_FOUND";
    case grpc::StatusCode::ALREADY_EXISTS:
      return "ALREADY_EXISTS";
    case grpc::StatusCode::PERMISSION_DENIED:
      return "PERMISSION_DENIED";
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
      return "RESOURCE_EXHAUSTED";
    case grpc::StatusCode::FAILED_PRECONDITION:
      return "FAILED_PRECONDITION";
    case grpc::StatusCode::ABORTED:
      return "ABORTED";
    case grpc::StatusCode::OUT_OF_RANGE:
      return "OUT_OF_RANGE";
    case grpc::StatusCode::UNIMPLEMENTED:
      return "UNIMPLEMENTED";
    case grpc::StatusCode::INTERNAL:
      return "INTERNAL";
    case grpc::StatusCode::UNAVAILABLE:
      return "UNAVAILABLE";
    case grpc::StatusCode::DATA_LOSS:
      return "DATA_LOSS";
    case grpc::StatusCode::UNAUTHENTICATED:
      return "UNAUTHENTICATED";
    default:
      return "UNKNOWN";
  }
}

inline const char* GrpcReasonCategory(grpc::StatusCode code) {
  switch (code) {
    case grpc::StatusCode::OK:
      return "none";
    case grpc::StatusCode::INVALID_ARGUMENT:
      return "invalid_request";
    case grpc::StatusCode::NOT_FOUND:
      return "not_found";
    case grpc::StatusCode::ALREADY_EXISTS:
      return "already_exists";
    case grpc::StatusCode::FAILED_PRECONDITION:
      return "precondition";
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
      return "resource_limit";
    case grpc::StatusCode::DEADLINE_EXCEEDED:
      return "timeout";
    case grpc::StatusCode::UNAVAILABLE:
      return "dependency_unavailable";
    case grpc::StatusCode::CANCELLED:
      return "cancelled";
    case grpc::StatusCode::PERMISSION_DENIED:
    case grpc::StatusCode::UNAUTHENTICATED:
      return "authorization";
    case grpc::StatusCode::DATA_LOSS:
      return "data_loss";
    default:
      return "internal";
  }
}

}  // namespace eos::runtime::northbound

#endif  // SRC_EOS_RUNTIME_NORTHBOUND_LOG_STATUS_H_
