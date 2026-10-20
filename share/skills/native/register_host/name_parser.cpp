#include "share/skills/native/register_host/name_parser.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace island::skills::native::register_host {
namespace {

std::string Trim(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
    value.remove_prefix(1);
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
    value.remove_suffix(1);
  return std::string(value);
}

std::string LowerAscii(std::string_view value) {
  std::string lowered(value);
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return lowered;
}

void TrimTrailingPunctuation(std::string* value) {
  constexpr std::array<std::string_view, 10> kSuffixes{"。", "，", "！", "？", "；",
                                                       ".",  ",",  "!",  "?",  ";"};
  bool removed = true;
  while (removed && !value->empty()) {
    removed = false;
    for (const auto suffix : kSuffixes) {
      if (value->size() >= suffix.size() && value->ends_with(suffix)) {
        value->erase(value->size() - suffix.size());
        *value = Trim(*value);
        removed = true;
        break;
      }
    }
  }
}

}  // namespace

std::optional<std::string> ParseIntroducedName(std::string_view text) {
  const std::string trimmed = Trim(text);
  const std::string lowered = LowerAscii(trimmed);
  struct Prefix {
    std::string_view text;
    bool ascii_case_insensitive;
  };
  constexpr std::array<Prefix, 9> kPrefixes{{
      {"我的名字是", false}, {"我的名字叫", false}, {"名字是", false},
      {"名字叫", false},     {"我叫", false},       {"我是", false},
      {"my name is", true}, {"i am", true},       {"i'm", true},
  }};

  std::size_t start = std::string::npos;
  for (const auto& prefix : kPrefixes) {
    const std::size_t position =
        prefix.ascii_case_insensitive ? lowered.find(prefix.text) : trimmed.find(prefix.text);
    if (position != std::string::npos) {
      start = position + prefix.text.size();
      break;
    }
  }
  if (start == std::string::npos) return std::nullopt;

  std::string candidate = Trim(std::string_view(trimmed).substr(start));
  constexpr std::array<std::string_view, 10> kDelimiters{"，", "。", "！", "？", "；",
                                                          ",",  ".",  "!",  "?",  ";"};
  std::size_t end = std::string::npos;
  for (const auto delimiter : kDelimiters) {
    const std::size_t position = candidate.find(delimiter);
    if (position != std::string::npos) end = std::min(end, position);
  }
  if (end != std::string::npos) candidate.erase(end);
  candidate = Trim(candidate);
  TrimTrailingPunctuation(&candidate);

  const std::string candidate_lower = LowerAscii(candidate);
  if (candidate.empty() || candidate.size() > 256 || candidate == "不知道" ||
      candidate == "不清楚" || candidate == "主人" || candidate_lower == "unknown") {
    return std::nullopt;
  }
  return candidate;
}

}  // namespace island::skills::native::register_host
