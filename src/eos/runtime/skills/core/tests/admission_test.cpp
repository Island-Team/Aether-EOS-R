#include "src/eos/runtime/skills/core/admission.h"
#include "src/eos/runtime/skills/core/catalog.h"

#include <gtest/gtest.h>

namespace eos::runtime::skills {
namespace {

class TestProvider final : public Provider {
 public:
  explicit TestProvider(Metadata metadata) : metadata_(std::move(metadata)) {}
  std::vector<Metadata> Discover() const override { return {metadata_}; }
  const Metadata* Find(const std::string&, const std::string&) const override { return nullptr; }
  void ValidateArguments(const std::string&, const std::string&,
                         const nlohmann::json&) const override {}
  RunResult Execute(const std::string&, const std::string&, const nlohmann::json&,
                    const std::string&, Cancelled) const override {
    return {};
  }

 private:
  Metadata metadata_;
};

Metadata ValidMetadata() {
  Metadata metadata;
  metadata.name = "observe_human";
  metadata.version = "1.0.0";
  metadata.parameters_schema = {{"type", "object"}};
  metadata.returns_schema = {{"type", "object"}};
  metadata.primitives.push_back({"head.move", "1.0.0"});
  metadata.resources.push_back({"robots/{robot_name}/detection/state", {}});
  return metadata;
}

TEST(CatalogAdmissionTest, AdmitsCurrentProductNamesAndCompleteDeclarations) {
  TestProvider provider(ValidMetadata());
  EXPECT_NO_THROW({ Catalog catalog(provider); });
}

TEST(CatalogAdmissionTest, RejectsInvalidSharedDeclarations) {
  Metadata metadata = ValidMetadata();
  metadata.version = "1.0";
  TestProvider invalid_identity(std::move(metadata));
  EXPECT_THROW({ Catalog catalog(invalid_identity); }, std::invalid_argument);

  metadata = ValidMetadata();
  metadata.resources.push_back(metadata.resources.front());
  TestProvider duplicate_resource(std::move(metadata));
  EXPECT_THROW({ Catalog catalog(duplicate_resource); }, std::invalid_argument);
}

TEST(CatalogAdmissionTest, ValidatesOptionalExecutionStateSchemas) {
  Metadata metadata = ValidMetadata();
  metadata.execution_state_schema = {
      {"type", "object"}, {"properties", {{"phase", {{"type", "string"}}}}}};
  metadata.related_process_state_schema = {
      {"type", "object"}, {"properties", {{"token", {{"type", "integer"}}}}}};
  TestProvider valid(std::move(metadata));
  EXPECT_NO_THROW({ Catalog catalog(valid); });

  metadata = ValidMetadata();
  metadata.execution_state_schema = {{"type", "string"}};
  TestProvider wrong_type(std::move(metadata));
  EXPECT_THROW({ Catalog catalog(wrong_type); }, std::invalid_argument);

  metadata = ValidMetadata();
  metadata.related_process_state_schema = {{"type", "not-a-json-schema-type"}};
  TestProvider malformed(std::move(metadata));
  EXPECT_THROW({ Catalog catalog(malformed); }, std::invalid_argument);
}

TEST(ResourceTemplateTest, RequiresACompleteSegmentMatch) {
  EXPECT_TRUE(ResourceTemplateMatches("robots/{robot_name}/state", "robots/demo/state"));
  EXPECT_FALSE(ResourceTemplateMatches("robots/{robot_name}/state", "robots/demo"));
  EXPECT_FALSE(ResourceTemplateMatches("robots/{robot_name}/state", "robots/demo/state/extra"));
  EXPECT_FALSE(ResourceTemplateMatches("robots/{robot_name}/state", "robots//state"));
  EXPECT_FALSE(ResourceTemplateMatches("robots/fixed/state", "robots/fixed-other/state"));
}

}  // namespace
}  // namespace eos::runtime::skills
