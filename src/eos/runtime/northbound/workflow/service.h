#pragma once

#include <filesystem>
#include <grpcpp/grpcpp.h>

#include "eos/workflow.grpc.pb.h"
#include "src/eos/runtime/northbound/workflow/registry.h"

namespace eos::runtime::actionlist {
class Manager;
}

namespace eos::runtime::northbound {

class WorkflowGrpcService final : public island::eos::v1::WorkflowService::Service {
 public:
  explicit WorkflowGrpcService(actionlist::Manager& manager,
                               const std::filesystem::path& template_directory = {});

  grpc::Status ListWorkflowTemplates(
      grpc::ServerContext*, const island::eos::v1::ListWorkflowTemplatesRequest*,
      island::eos::v1::ListWorkflowTemplatesResponse*) override;
  grpc::Status GetWorkflowTemplate(
      grpc::ServerContext*, const island::eos::v1::GetWorkflowTemplateRequest*,
      island::eos::v1::WorkflowTemplateDescriptor*) override;
  grpc::Status RunWorkflowTemplate(grpc::ServerContext*,
                                   const island::eos::v1::RunWorkflowTemplateRequest*,
                                   island::eos::v1::ActionList*) override;

  grpc::Status ApplyActionList(grpc::ServerContext*, const island::eos::v1::ApplyActionListRequest*,
                               island::eos::v1::ActionList*) override;
  grpc::Status GetActionList(grpc::ServerContext*, const island::eos::v1::GetActionListRequest*,
                             island::eos::v1::ActionList*) override;
  grpc::Status ListActionLists(grpc::ServerContext*, const island::eos::v1::ListActionListsRequest*,
                               island::eos::v1::ListActionListsResponse*) override;
  grpc::Status GetRuntimeSnapshot(grpc::ServerContext*,
                                  const island::eos::v1::GetRuntimeSnapshotRequest*,
                                  island::eos::v1::ManagerRuntimeSnapshot*) override;
  grpc::Status WatchActionListEvents(
      grpc::ServerContext*, const island::eos::v1::WatchActionListEventsRequest*,
      grpc::ServerWriter<island::eos::v1::ActionListEvent>*) override;

 private:
  actionlist::Manager& manager_;
  WorkflowTemplateRegistry registry_;
};

}  // namespace eos::runtime::northbound
