#include <dlfcn.h>

#include <algorithm>
#include <stdexcept>

#include "src/eos/runtime/skills/native/detail.h"
#include "src/eos/runtime/skills/core/admission.h"

namespace eos::runtime::skills::native::detail {
bool ValidUtf8(const std::string& value) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(value.data());
  std::size_t i = 0;
  while (i < value.size()) {
    const unsigned char first = bytes[i++];
    if (first <= 0x7f) continue;
    std::size_t continuation = 0;
    std::uint32_t code_point = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      continuation = 1;
      code_point = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
      continuation = 2;
      code_point = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
      continuation = 3;
      code_point = first & 0x07;
    } else
      return false;
    if (i + continuation > value.size()) return false;
    for (std::size_t j = 0; j < continuation; ++j) {
      const unsigned char next = bytes[i++];
      if ((next & 0xc0) != 0x80) return false;
      code_point = (code_point << 6) | (next & 0x3f);
    }
    if ((continuation == 2 && code_point < 0x800) || (continuation == 3 && code_point < 0x10000) ||
        (code_point >= 0xd800 && code_point <= 0xdfff) || code_point > 0x10ffff)
      return false;
  }
  return true;
}

std::string CopyText(eos_skill_text_v2 text, std::uint32_t maximum, const std::string& field,
                     bool required) {
  if (text.size > maximum) throw std::runtime_error(field + " exceeds its size limit");
  if (text.size != 0 && text.data == nullptr) throw std::runtime_error(field + " has null data");
  if (required && text.size == 0) throw std::runtime_error(field + " is required");
  if (text.size == 0) return {};
  std::string copied(text.data, text.size);
  if (copied.find('\0') != std::string::npos) throw std::runtime_error(field + " contains NUL");
  if (!ValidUtf8(copied)) throw std::runtime_error(field + " is not valid UTF-8");
  return copied;
}

eos_skill_text_v2 Borrow(const std::string& text) {
  return {text.empty() ? nullptr : text.data(), static_cast<std::uint32_t>(text.size())};
}

bool ValidName(const std::string& value) {
  return ValidMetadataName(value);
}

bool ValidVersion(const std::string& value) {
  return ValidMetadataVersion(value);
}

std::string DlError() {
  const char* error = dlerror();
  return error == nullptr ? std::string("unknown dynamic loader error") : std::string(error);
}

Handle::~Handle() {
  if (value != nullptr && !process_lifetime) dlclose(value);
}
}  // namespace eos::runtime::skills::native::detail

namespace eos::runtime::skills::native {
using detail::LoadedSkill;

Runtime::Runtime(dispatch::Dispatcher& dispatcher, ResourceReader resource_reader,
                 InferenceRunner inference_runner)
    : impl_(std::make_unique<Impl>()) {
  impl_->dispatcher = &dispatcher;
  impl_->resource_reader = std::move(resource_reader);
  impl_->inference_runner = std::move(inference_runner);
}
Runtime::Runtime(std::unique_ptr<Impl> impl, dispatch::Dispatcher& dispatcher,
                  ResourceReader resource_reader, InferenceRunner inference_runner)
    : impl_(std::move(impl)) {
  impl_->dispatcher = &dispatcher;
  impl_->resource_reader = std::move(resource_reader);
  impl_->inference_runner = std::move(inference_runner);
}
Runtime::~Runtime() = default;
Runtime::Runtime(Runtime&&) noexcept = default;
Runtime& Runtime::operator=(Runtime&&) noexcept = default;

std::vector<Metadata> Runtime::Discover() const {
  std::vector<Metadata> found;
  found.reserve(impl_->skills.size());
  for (const LoadedSkill& skill : impl_->skills) found.push_back(skill.metadata);
  return found;
}

const Metadata* Runtime::Find(const std::string& name, const std::string& version) const {
  const auto found = impl_->skills_by_identity.find({name, version});
  return found == impl_->skills_by_identity.end() ? nullptr : &found->second->metadata;
}
}  // namespace eos::runtime::skills::native
