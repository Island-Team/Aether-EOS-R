#include <iostream>
#include <stdexcept>
#include <string>

#include "src/eos/runtime/skills/manager/journald.h"

namespace {
void Usage() {
  std::cerr << "usage: eos-skill-journal FILE [--execution ID] [--skill NAME]"
               " [--operation ID] [--correlation ID] [--boot ID] [--severity LEVEL]"
               " [--from RFC3339] [--to RFC3339]\n";
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    Usage();
    return 2;
  }
  eos::runtime::skills::DiagnosticQuery query;
  for (int index = 2; index < argc; index += 2) {
    if (index + 1 >= argc) {
      Usage();
      return 2;
    }
    const std::string option = argv[index];
    const std::string value = argv[index + 1];
    if (option == "--execution") query.execution_id = value;
    else if (option == "--skill") query.skill_name = value;
    else if (option == "--operation") query.operation_id = value;
    else if (option == "--correlation") query.correlation_id = value;
    else if (option == "--boot") query.boot_id = value;
    else if (option == "--severity") query.severity = value;
    else if (option == "--from") query.from_timestamp = value;
    else if (option == "--to") query.to_timestamp = value;
    else {
      Usage();
      return 2;
    }
  }
  try {
    for (const auto& record : eos::runtime::skills::ReadSkillDiagnostics(argv[1], query))
      std::cout << record.dump() << '\n';
  } catch (const std::exception& error) {
    std::cerr << "eos-skill-journal: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
