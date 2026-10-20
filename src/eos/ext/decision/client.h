/// SkillService transport used by the external decision runner.
#pragma once

#include <grpcpp/channel.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "types.h"

namespace eos::ext::decision {

class DecisionTransport {
 public:
  using JournalHandler = std::function<bool(const DecisionJournalEntry&)>;
  using MonitorHandler = std::function<bool(const MonitorUpdateView&)>;

  virtual ~DecisionTransport() = default;
  virtual Result<std::vector<SkillDescriptor>> ListSkills() = 0;
  virtual Result<SkillDescriptor> GetSkill(const SkillReference& reference) = 0;
  virtual Result<ExecutionView> StartExecution(const StartAction& action) = 0;
  virtual Result<std::vector<ExecutionView>> ListExecutions() = 0;
  virtual Result<ExecutionView> GetExecution(const std::string& execution_id) = 0;
  virtual Result<SignalReceipt> SignalExecution(const SignalAction& action) = 0;
  virtual Result<ExecutionView> TerminateExecution(const std::string& execution_id) = 0;
  virtual Result<TerminateExecutionsReceipt> TerminateExecutions(
      const TerminateExecutionsAction& action) = 0;

  /// Replays journal entries after `after_sequence`, then follows the stream. The
  /// handler returns false to stop reading. Implementations must unblock when
  /// CancelPendingCalls is invoked.
  virtual TransportError FollowExecutionJournal(const std::string& execution_id,
                                                std::uint64_t after_sequence,
                                                const JournalHandler& handler) = 0;
  virtual TransportError MonitorExecutions(const ExecutionSelector& selector,
                                           std::uint64_t after_sequence,
                                           const MonitorHandler& handler) = 0;
  virtual void CancelPendingCalls() = 0;
};

struct SkillClientOptions {
  std::chrono::milliseconds unary_timeout{5000};
};

class SkillClient final : public DecisionTransport {
 public:
  explicit SkillClient(std::shared_ptr<grpc::Channel> channel, SkillClientOptions options = {});
  ~SkillClient() override;
  SkillClient(const SkillClient&) = delete;
  SkillClient& operator=(const SkillClient&) = delete;

  Result<std::vector<SkillDescriptor>> ListSkills() override;
  Result<SkillDescriptor> GetSkill(const SkillReference& reference) override;
  Result<ExecutionView> StartExecution(const StartAction& action) override;
  Result<std::vector<ExecutionView>> ListExecutions() override;
  Result<ExecutionView> GetExecution(const std::string& execution_id) override;
  Result<SignalReceipt> SignalExecution(const SignalAction& action) override;
  Result<ExecutionView> TerminateExecution(const std::string& execution_id) override;
  Result<TerminateExecutionsReceipt> TerminateExecutions(
      const TerminateExecutionsAction& action) override;
  TransportError FollowExecutionJournal(const std::string& execution_id,
                                        std::uint64_t after_sequence,
                                        const JournalHandler& handler) override;
  TransportError MonitorExecutions(const ExecutionSelector& selector, std::uint64_t after_sequence,
                                   const MonitorHandler& handler) override;
  void CancelPendingCalls() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace eos::ext::decision
