#include "src/eos/runtime/skills/core/admission.h"

#include <algorithm>
#include <cctype>
#include <regex>

namespace eos::runtime::skills {

bool ValidMetadataName(const std::string& value) {
  static const std::regex pattern("^[a-z][a-z0-9]*(?:[._-][a-z0-9]+)*$");
  return std::regex_match(value, pattern);
}

bool ValidMetadataVersion(const std::string& value) {
  static const std::regex pattern(
      R"(^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$)");
  if (!std::regex_match(value, pattern)) return false;
  const std::size_t dash = value.find('-');
  if (dash == std::string::npos) return true;
  const std::size_t plus = value.find('+', dash);
  const std::string prerelease = value.substr(dash + 1, plus - dash - 1);
  std::size_t start = 0;
  while (start <= prerelease.size()) {
    const std::size_t end = prerelease.find('.', start);
    const std::string identifier = prerelease.substr(start, end - start);
    const bool numeric = std::all_of(identifier.begin(), identifier.end(),
                                     [](unsigned char c) { return std::isdigit(c) != 0; });
    if (numeric && identifier.size() > 1 && identifier.front() == '0') return false;
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return true;
}

bool ValidResourceTemplate(const std::string& value) {
  if (value.empty()) return false;
  static const std::regex variable("^[A-Za-z0-9_%][A-Za-z0-9_.%]*$");
  std::size_t offset = 0;
  while (offset < value.size()) {
    const std::size_t end = value.find('/', offset);
    const std::string segment = value.substr(offset, end - offset);
    if (segment.empty()) return false;
    if (segment.front() == '{' || segment.back() == '}') {
      if (segment.size() < 3 || segment.front() != '{' || segment.back() != '}' ||
          !std::regex_match(segment.substr(1, segment.size() - 2), variable))
        return false;
    } else if (segment.find_first_of("{}") != std::string::npos) {
      return false;
    }
    if (std::any_of(segment.begin(), segment.end(),
                    [](unsigned char c) { return c <= 0x20 || c == 0x7f; }))
      return false;
    if (end == std::string::npos) break;
    offset = end + 1;
  }
  return true;
}

bool ResourceTemplateMatches(const std::string& uri_template, const std::string& uri) {
  std::size_t template_offset = 0;
  std::size_t uri_offset = 0;
  while (true) {
    const std::size_t template_end = uri_template.find('/', template_offset);
    const std::size_t uri_end = uri.find('/', uri_offset);
    const std::string template_segment =
        uri_template.substr(template_offset, template_end - template_offset);
    const std::string uri_segment = uri.substr(uri_offset, uri_end - uri_offset);
    const bool variable = template_segment.size() >= 3 && template_segment.front() == '{' &&
                          template_segment.back() == '}';
    if ((variable && uri_segment.empty()) || (!variable && template_segment != uri_segment))
      return false;
    if (template_end == std::string::npos || uri_end == std::string::npos)
      return template_end == uri_end;
    template_offset = template_end + 1;
    uri_offset = uri_end + 1;
  }
}

}  // namespace eos::runtime::skills
