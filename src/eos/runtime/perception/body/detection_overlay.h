#pragma once

#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

namespace eos::runtime::perception::body {

// Session-owned corrections projected over the raw camera detection stream.
// The owner is the skill execution ID; replacing or clearing one owner's entry
// cannot affect another execution.
class DetectionOverlayStore {
 public:
  void Upsert(const std::string& execution_id, nlohmann::json overlay);
  void Clear(const std::string& execution_id);
  void Apply(nlohmann::json* snapshot) const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, nlohmann::json> overlays_;
};

}  // namespace eos::runtime::perception::body
