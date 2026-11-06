#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "eos/workflow.pb.h"

namespace eos::runtime::northbound {

/// Immutable deployment workflow templates loaded once at the EOS startup barrier.
class WorkflowTemplateRegistry final {
 public:
  explicit WorkflowTemplateRegistry(const std::filesystem::path& directory = {});

  std::vector<island::eos::v1::WorkflowTemplateDescriptor> List() const;
  std::optional<island::eos::v1::WorkflowTemplateDescriptor> Get(
      const std::string& name) const;
  std::size_t size() const { return templates_.size(); }

 private:
  std::map<std::string, island::eos::v1::WorkflowTemplateDescriptor> templates_;
};

}  // namespace eos::runtime::northbound
