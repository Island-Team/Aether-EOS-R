#ifndef EOS_RUNTIME_MODELS_GRPC_CLIENT_H_
#define EOS_RUNTIME_MODELS_GRPC_CLIENT_H_

#include "src/eos/runtime/models/runtime.h"

namespace eos::runtime::models {

CallResult GrpcCall(const ModelConfig& model, const nlohmann::json& request,
                    const CancelRequested& cancel_requested);
CallResult GrpcHealth(const ModelConfig& model, const CancelRequested& cancel_requested);

}  // namespace eos::runtime::models

#endif
