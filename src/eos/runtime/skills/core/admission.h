#pragma once

#include <string>

namespace eos::runtime::skills {

bool ValidMetadataName(const std::string& value);
bool ValidMetadataVersion(const std::string& value);
bool ValidResourceTemplate(const std::string& value);
bool ResourceTemplateMatches(const std::string& uri_template, const std::string& uri);

}  // namespace eos::runtime::skills
