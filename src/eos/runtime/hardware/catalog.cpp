#include "catalog.h"

#include <stdexcept>
#include <utility>

namespace eos::runtime::hardware_runtime {

void BackendCatalog::Register(BackendDescriptor descriptor) {
  if (descriptor.type.empty() || Find(descriptor.type) != nullptr)
    throw std::invalid_argument("backend catalog requires a unique non-empty type");
  descriptors_.push_back(std::move(descriptor));
}

const BackendDescriptor* BackendCatalog::Find(std::string_view type) const {
  for (const BackendDescriptor& descriptor : descriptors_) {
    if (descriptor.type == type) return &descriptor;
  }
  return nullptr;
}

}  // namespace eos::runtime::hardware_runtime
