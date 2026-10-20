/// Pure context-aware completion and hint generation for the REPL command language.
#pragma once

#include <string>
#include <vector>

#include "catalog.h"

namespace eos::ext::repl {

class CompletionEngine final {
 public:
  explicit CompletionEngine(const Catalog* catalog) : catalog_(catalog) {}

  std::vector<std::string> Complete(const std::string& input, int* context_length) const;
  std::string Hint(const std::string& input, int* context_length) const;
  bool IsCommand(const std::string& word) const;

 private:
  const Catalog* catalog_;
};

}  // namespace eos::ext::repl
