/// gRPC adapter for the profile-neutral skill execution service.
#pragma once

#include <grpcpp/grpcpp.h>

#include "eos/skills.grpc.pb.h"

namespace eos::runtime::skills {
class SkillManager;
}

namespace eos::runtime::northbound {

class SkillGrpcService final : public island::eos::v1::SkillService::Service {
 public:
  explicit SkillGrpcService(skills::SkillManager& service) : service_(service) {}

  grpc::Status ListSkills(grpc::ServerContext*, const island::eos::v1::ListSkillsRequest*,
                          island::eos::v1::ListSkillsResponse*) override;
  grpc::Status GetSkill(grpc::ServerContext*, const island::eos::v1::GetSkillRequest*,
                        island::eos::v1::SkillDescriptor*) override;
  grpc::Status StartExecution(grpc::ServerContext*, const island::eos::v1::StartExecutionRequest*,
                              island::eos::v1::SkillExecution*) override;
  grpc::Status GetExecution(grpc::ServerContext*, const island::eos::v1::GetExecutionRequest*,
                            island::eos::v1::SkillExecution*) override;
  grpc::Status WatchExecution(grpc::ServerContext*, const island::eos::v1::WatchExecutionRequest*,
                               grpc::ServerWriter<island::eos::v1::SkillExecution>*) override;
  grpc::Status GetExecutionState(grpc::ServerContext*,
                                 const island::eos::v1::GetExecutionStateRequest*,
                                 island::eos::v1::SkillExecutionState*) override;
  grpc::Status WatchExecutionState(
      grpc::ServerContext*, const island::eos::v1::WatchExecutionStateRequest*,
      grpc::ServerWriter<island::eos::v1::SkillExecutionState>*) override;
  grpc::Status TerminateExecution(grpc::ServerContext*,
                                  const island::eos::v1::TerminateExecutionRequest*,
                                  island::eos::v1::SkillExecution*) override;
  grpc::Status TerminateExecutions(grpc::ServerContext*,
                                   const island::eos::v1::TerminateExecutionsRequest*,
                                   island::eos::v1::TerminateExecutionsResponse*) override;
  grpc::Status ListExecutions(grpc::ServerContext*, const island::eos::v1::ListExecutionsRequest*,
                              island::eos::v1::ListExecutionsResponse*) override;
  grpc::Status SignalExecution(grpc::ServerContext*, const island::eos::v1::SignalExecutionRequest*,
                               island::eos::v1::SkillSignalReceipt*) override;
  grpc::Status FollowExecutionJournal(
      grpc::ServerContext*, const island::eos::v1::FollowExecutionJournalRequest*,
      grpc::ServerWriter<island::eos::v1::SkillJournalEntry>*) override;
  grpc::Status MonitorExecutions(grpc::ServerContext*,
                                 const island::eos::v1::MonitorExecutionsRequest*,
                                 grpc::ServerWriter<island::eos::v1::SkillMonitorUpdate>*) override;

 private:
  skills::SkillManager& service_;
};

}  // namespace eos::runtime::northbound
