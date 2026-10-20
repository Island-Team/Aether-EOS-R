#pragma once

#include <grpcpp/grpcpp.h>

#include "eos/skills.pb.h"
#include "src/eos/runtime/skills/core/model.h"

namespace eos::runtime::northbound {

nlohmann::json SkillStructJson(const google::protobuf::Struct& value);
grpc::Status FillSkillStruct(const nlohmann::json& value, google::protobuf::Struct* output);
grpc::Status FillSkillDescriptor(const skills::Metadata& metadata,
                                 island::eos::v1::SkillDescriptor* output);
grpc::Status FillSkillExecution(const skills::ExecutionSnapshot& snapshot,
                                 island::eos::v1::SkillExecution* output);
grpc::Status FillExecutionState(const std::string& execution_id,
                                const skills::RetainedExecutionState& state,
                                island::eos::v1::SkillExecutionState* output);
grpc::Status FillJournalEntry(const skills::JournalEntry& event,
                              island::eos::v1::SkillJournalEntry* output);
grpc::Status FillMonitorUpdate(const skills::MonitorUpdate& update,
                               island::eos::v1::SkillMonitorUpdate* output);

}  // namespace eos::runtime::northbound
