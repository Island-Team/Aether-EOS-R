#include "src/eos/runtime/northbound/workflow/registry.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace eos::runtime::northbound {
namespace {

class TemporaryTemplates {
 public:
  TemporaryTemplates() {
    path_ = std::filesystem::temp_directory_path() /
            ("eos-workflow-test-" + std::to_string(++sequence_));
    std::filesystem::create_directory(path_);
  }
  ~TemporaryTemplates() { std::filesystem::remove_all(path_); }
  void Write(const std::string& name, const std::string& json) const {
    std::ofstream output(path_ / (name + ".json"));
    output << json;
  }
  const std::filesystem::path& path() const { return path_; }

 private:
  static inline unsigned sequence_{0};
  std::filesystem::path path_;
};

TEST(WorkflowTemplateRegistryTest, LoadsGenericTemplateByFilename) {
  TemporaryTemplates templates;
  templates.Write("move", R"json({
    "workflowId":"move-example",
    "actions":[{"actionId":"move","primitive":{"name":"base.navigate","version":"1.0.0"},"parameters":{}}]
  })json");
  WorkflowTemplateRegistry registry(templates.path());
  ASSERT_EQ(registry.size(), 1u);
  const auto descriptor = registry.Get("move");
  ASSERT_TRUE(descriptor.has_value());
  EXPECT_EQ(descriptor->definition().workflow_id(), "move-example");
  EXPECT_FALSE(registry.Get("missing").has_value());
}

TEST(WorkflowTemplateRegistryTest, RejectsMalformedTemplate) {
  TemporaryTemplates templates;
  templates.Write("bad", R"json({"workflowId":"bad","actions":[]})json");
  EXPECT_THROW(WorkflowTemplateRegistry registry(templates.path()), std::runtime_error);
}

TEST(WorkflowTemplateRegistryTest, LoadsCapabilityTargetAndRejectsEmptyName) {
  TemporaryTemplates valid;
  valid.Write("capability", R"json({
    "workflowId":"capability-example",
    "actions":[{"actionId":"move","capability":{"capabilityName":"hardware/demo/base/main/base.navigate@1.0.0"},"parameters":{}}]
  })json");
  WorkflowTemplateRegistry registry(valid.path());
  ASSERT_EQ(registry.size(), 1u);
  const auto descriptor = registry.Get("capability");
  ASSERT_TRUE(descriptor.has_value());
  EXPECT_EQ(descriptor->definition().actions(0).capability().capability_name(),
            "hardware/demo/base/main/base.navigate@1.0.0");

  TemporaryTemplates invalid;
  invalid.Write("empty", R"json({
    "workflowId":"empty-capability",
    "actions":[{"actionId":"move","capability":{"capabilityName":""},"parameters":{}}]
  })json");
  EXPECT_THROW(WorkflowTemplateRegistry rejected(invalid.path()), std::runtime_error);
}

}  // namespace
}  // namespace eos::runtime::northbound
