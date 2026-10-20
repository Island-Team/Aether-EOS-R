#pragma once

#include <optional>
#include <string>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

namespace eos::runtime::skills::builtin {

// A stable, transport-neutral error returned by a built-in capability.
struct Error {
  std::string code;
  std::string message;
  nlohmann::json details = nlohmann::json::object();
};

template <typename T>
class Result {
 public:
  static Result Success(T value) { return Result(std::move(value)); }
  static Result Failure(Error error) { return Result(std::move(error)); }

  bool ok() const { return std::holds_alternative<T>(value_); }
  explicit operator bool() const { return ok(); }
  const T& value() const { return std::get<T>(value_); }
  T& value() { return std::get<T>(value_); }
  const Error& error() const { return std::get<Error>(value_); }

 private:
  explicit Result(T value) : value_(std::move(value)) {}
  explicit Result(Error error) : value_(std::move(error)) {}
  std::variant<T, Error> value_;
};

template <>
class Result<void> {
 public:
  static Result Success() { return Result(std::nullopt); }
  static Result Failure(Error error) { return Result(std::move(error)); }

  bool ok() const { return !error_.has_value(); }
  explicit operator bool() const { return ok(); }
  const Error& error() const { return error_.value(); }

 private:
  explicit Result(std::optional<Error> error) : error_(std::move(error)) {}
  explicit Result(Error error) : error_(std::move(error)) {}
  std::optional<Error> error_;
};

}  // namespace eos::runtime::skills::builtin
