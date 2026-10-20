#include "src/eos/runtime/init/config.h"

#include <fstream>
#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <toml++/toml.hpp>
#include <utility>

namespace eos::runtime::init {
namespace {

[[noreturn]] void Fail(const std::string& path, const std::string& message) {
  throw std::runtime_error("config '" + path + "': " + message);
}

const toml::table* OptionalTable(const toml::table& root, const char* key,
                                 const std::string& path) {
  const toml::node* node = root.get(key);
  if (node == nullptr) return nullptr;
  const toml::table* table = node->as_table();
  if (table == nullptr) Fail(path, "[" + std::string(key) + "] must be a table");
  return table;
}

void AdmitKeys(const toml::table& table, std::initializer_list<std::string_view> allowed,
               const std::string& path, const std::string& where) {
  for (const auto& [key, value] : table) {
    (void)value;
    bool admitted = false;
    for (std::string_view candidate : allowed) admitted = admitted || key == candidate;
    if (!admitted) Fail(path, where + " has unknown key '" + std::string(key.str()) + "'");
  }
}

}  // namespace

Config LoadConfig(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) Fail(path, "cannot open file for reading");
  std::ostringstream contents;
  contents << input.rdbuf();

  toml::table root;
  try {
    root = toml::parse(contents.str(), path);
  } catch (const toml::parse_error& error) {
    Fail(path, std::string(error.description()));
  }
  AdmitKeys(root, {"gateway", "launch", "log", "mode", "perception", "workflow"}, path, "root");

  Config config;
  if (const toml::table* gateway = OptionalTable(root, "gateway", path)) {
    AdmitKeys(*gateway, {"host", "grpc_port", "grpc_uds_path", "grpc_uds_mode"}, path,
              "[gateway]");
    if (const toml::node* host = gateway->get("host")) {
      const auto value = host->value<std::string>();
      if (!value || value->empty()) Fail(path, "[gateway] host must be a non-empty string");
      config.host = *value;
    }
    const std::pair<const char*, std::uint16_t*> ports[] = {{"grpc_port", &config.grpc_port}};
    for (const auto& [key, target] : ports) {
      if (const toml::node* node = gateway->get(key)) {
        const auto value = node->value<std::int64_t>();
        if (!value || *value <= 0 || *value > 65535) {
          Fail(path, "[gateway] " + std::string(key) + " must be an integer in (0, 65535]");
        }
        *target = static_cast<std::uint16_t>(*value);
      }
    }
    if (const toml::node* node = gateway->get("grpc_uds_path")) {
      const auto value = node->value<std::string>();
      if (!value || value->empty() || !std::filesystem::path(*value).is_absolute() ||
          std::filesystem::path(*value).lexically_normal() != std::filesystem::path(*value)) {
        Fail(path, "[gateway] grpc_uds_path must be a non-empty absolute normalized path");
      }
      if (value->size() > 107) {
        Fail(path, "[gateway] grpc_uds_path exceeds the Unix socket path limit");
      }
      config.grpc_uds_path = *value;
    }
    if (const toml::node* node = gateway->get("grpc_uds_mode")) {
      const auto value = node->value<std::int64_t>();
      if (!value || *value <= 0 || *value > 0777) {
        Fail(path, "[gateway] grpc_uds_mode must be an integer in [1, 0777]");
      }
      config.grpc_uds_mode = static_cast<std::uint32_t>(*value);
    }
    if (gateway->contains("grpc_uds_mode") && config.grpc_uds_path.empty()) {
      Fail(path, "[gateway] grpc_uds_mode requires grpc_uds_path");
    }
  }
  if (config.grpc_port == 16004) {
    Fail(path, "[gateway] port 16004 is reserved for the external EOS simulator service");
  }

  if (const toml::table* workflow = OptionalTable(root, "workflow", path)) {
    AdmitKeys(*workflow, {"journal_enabled"}, path, "[workflow]");
    if (const toml::node* node = workflow->get("journal_enabled")) {
      const auto value = node->value<bool>();
      if (!value) Fail(path, "[workflow] journal_enabled must be a boolean");
      config.workflow_journal_enabled = *value;
    }
  }

  if (const toml::table* launch = OptionalTable(root, "launch", path)) {
    AdmitKeys(*launch, {"prelaunch", "adapter"}, path, "[launch]");
    if (const toml::node* adapter_node = launch->get("adapter")) {
      const toml::table* adapter = adapter_node->as_table();
      if (adapter == nullptr) Fail(path, "[launch.adapter] must be a table");
      AdmitKeys(*adapter, {"shell", "ready_shell", "timeout_ms", "reuse_if_ready"}, path, "[launch.adapter]");
      const auto shell = adapter->get("shell") ? adapter->get("shell")->value<std::string>() : std::nullopt;
      const auto ready = adapter->get("ready_shell") ? adapter->get("ready_shell")->value<std::string>() : std::nullopt;
      if (!shell || shell->empty() || !ready || ready->empty()) {
        Fail(path, "[launch.adapter] requires non-empty shell and ready_shell");
      }
      config.adapter.shell = *shell;
      config.adapter.ready_shell = *ready;
      if (const toml::node* reuse = adapter->get("reuse_if_ready")) {
        const auto value = reuse->value<bool>();
        if (!value) Fail(path, "[launch.adapter] reuse_if_ready must be boolean");
        config.adapter.reuse_if_ready = *value;
      }
      if (const toml::node* timeout = adapter->get("timeout_ms")) {
        const auto value = timeout->value<std::int64_t>();
        if (!value || *value < 1 || *value > 300000) {
          Fail(path, "[launch.adapter] timeout_ms must be 1..300000");
        }
        config.adapter.timeout_ms = static_cast<std::uint32_t>(*value);
      }
    }
    if (const toml::node* commands = launch->get("prelaunch")) {
      const toml::array* list = commands->as_array();
      if (list == nullptr) Fail(path, "[launch] prelaunch must be an array of tables");
      std::size_t index = 0;
      for (const toml::node& node : *list) {
        const toml::table* table = node.as_table();
        const std::string where = "[launch].prelaunch[" + std::to_string(index) + "]";
        if (table == nullptr) Fail(path, where + " must be a table");
        AdmitKeys(*table, {"command", "shell", "ready_shell", "reuse_if_ready",
                           "restart_on_exit"}, path, where);
        const toml::node* command = table->get("command");
        const toml::array* arguments = command == nullptr ? nullptr : command->as_array();
        const toml::node* shell = table->get("shell");
        if ((arguments == nullptr) == (shell == nullptr)) {
          Fail(path, where + " must contain exactly one of command or shell");
        }
        PrelaunchCommand parsed;
        if (arguments != nullptr) {
          if (arguments->empty()) Fail(path, where + " command must be a non-empty string array");
          for (const toml::node& argument : *arguments) {
            const auto value = argument.value<std::string>();
            if (!value || value->empty()) {
              Fail(path, where + " command must contain only non-empty strings");
            }
            parsed.arguments.push_back(*value);
          }
        } else {
          const auto value = shell->value<std::string>();
          if (!value || value->empty()) Fail(path, where + " shell must be a non-empty string");
          parsed.shell = *value;
        }
        if (const toml::node* ready = table->get("ready_shell")) {
          const auto value = ready->value<std::string>();
          if (!value || value->empty()) Fail(path, where + " ready_shell must be non-empty string");
          parsed.ready_shell = *value;
        }
        if (const toml::node* reuse = table->get("reuse_if_ready")) {
          const auto value = reuse->value<bool>();
          if (!value) Fail(path, where + " reuse_if_ready must be boolean");
          parsed.reuse_if_ready = *value;
        }
        if (const toml::node* restart = table->get("restart_on_exit")) {
          const auto value = restart->value<bool>();
          if (!value) Fail(path, where + " restart_on_exit must be boolean");
          parsed.restart_on_exit = *value;
        }
        if (parsed.reuse_if_ready && (parsed.shell.empty() || parsed.ready_shell.empty()))
          Fail(path, where + " reuse_if_ready requires shell and ready_shell");
        if (parsed.restart_on_exit && parsed.shell.empty())
          Fail(path, where + " restart_on_exit requires shell");
        config.prelaunch.push_back(std::move(parsed));
        ++index;
      }
    }
  }

  if (const toml::table* log = OptionalTable(root, "log", path)) {
    AdmitKeys(*log,
              {"level", "file_level", "jsonl_level", "overwrite", "dedup", "dedup_window_s",
               "save_attachments", "rotation_size_bytes", "rotation_files"},
              path, "[log]");
    const std::pair<const char*, std::string*> levels[] = {
        {"level", &config.log.console_level},
        {"file_level", &config.log.file_level},
        {"jsonl_level", &config.log.jsonl_level}};
    for (const auto& [key, target] : levels) {
      if (const toml::node* node = log->get(key)) {
        const auto value = node->value<std::string>();
        if (!value) Fail(path, "[log] " + std::string(key) + " must be a string");
        *target = *value;
      }
    }
    if (const toml::node* node = log->get("overwrite")) {
      const auto value = node->value<bool>();
      if (!value) Fail(path, "[log] overwrite must be a boolean");
      config.log.overwrite = *value;
    }
    if (const toml::node* node = log->get("dedup")) {
      const auto value = node->value<bool>();
      if (!value) Fail(path, "[log] dedup must be a boolean");
      config.log.dedup = *value;
    }
    if (const toml::node* node = log->get("dedup_window_s")) {
      const auto value = node->value<double>();
      if (!value || *value <= 0.0) Fail(path, "[log] dedup_window_s must be a positive number");
      config.log.dedup_window_s = *value;
    }
    if (const toml::node* node = log->get("save_attachments")) {
      const auto value = node->value<bool>();
      if (!value) Fail(path, "[log] save_attachments must be a boolean");
      config.log.save_attachments = *value;
    }
    if (const toml::node* node = log->get("rotation_size_bytes")) {
      const auto value = node->value<std::int64_t>();
      if (!value || *value <= 0) Fail(path, "[log] rotation_size_bytes must be positive");
      config.log.rotation_size_bytes = static_cast<std::uint64_t>(*value);
    }
    if (const toml::node* node = log->get("rotation_files")) {
      const auto value = node->value<std::int64_t>();
      if (!value || *value <= 0 || *value > 100) {
        Fail(path, "[log] rotation_files must be an integer in [1, 100]");
      }
      config.log.rotation_files = static_cast<std::uint32_t>(*value);
    }
  }

  if (const toml::table* mode = OptionalTable(root, "mode", path)) {
    AdmitKeys(*mode, {"binding", "primitives"}, path, "[mode]");
  }
  if (const toml::table* perception = OptionalTable(root, "perception", path)) {
    AdmitKeys(*perception,
              {"model", "stream", "camera_pose", "camera_face", "camera_object", "arm_grasp",
               "microphone_vad"},
              path, "[perception]");
    if (const toml::node* node = perception->get("stream")) {
      const toml::table* stream = node->as_table();
      if (stream == nullptr) Fail(path, "[perception.stream] must be a table");
      AdmitKeys(*stream, {"vad", "human", "human_pose", "face"}, path,
                "[perception.stream]");
      const std::pair<const char*, bool*> switches[] = {
          {"vad", &config.perception_stream.vad},
          {"human", &config.perception_stream.human},
          {"human_pose", &config.perception_stream.human_pose},
          {"face", &config.perception_stream.face}};
      for (const auto& [key, target] : switches) {
        if (const toml::node* value = stream->get(key)) {
          const auto parsed = value->value<bool>();
          if (!parsed) Fail(path, "[perception.stream] " + std::string(key) + " must be a boolean");
          *target = *parsed;
        }
      }
    }
    if (const toml::node* node = perception->get("camera_pose")) {
      const toml::table* camera_pose = node->as_table();
      if (camera_pose == nullptr) Fail(path, "[perception.camera_pose] must be a table");
      AdmitKeys(*camera_pose,
                {"model_id", "detection_model_id", "posture_model_id", "camera_part_id", "gimbal_part_id", "rate_hz"},
                path, "[perception.camera_pose]");
      const toml::node* model_id = camera_pose->get("model_id");
      const toml::node* detection_model_id = camera_pose->get("detection_model_id");
      const toml::node* posture_model_id = camera_pose->get("posture_model_id");
      const toml::node* camera_part_id = camera_pose->get("camera_part_id");
      const toml::node* gimbal_part_id = camera_pose->get("gimbal_part_id");
      const toml::node* rate_hz = camera_pose->get("rate_hz");
      const auto model =
          model_id == nullptr ? std::optional<std::string>{} : model_id->value<std::string>();
      const auto rate =
          rate_hz == nullptr ? std::optional<std::int64_t>{} : rate_hz->value<std::int64_t>();
      if (!model || model->empty()) {
        Fail(path, "[perception.camera_pose] model_id must be a non-empty string");
      }
      const auto part = camera_part_id == nullptr ? std::optional<std::string>{"default"}
                                                  : camera_part_id->value<std::string>();
      if (!part || part->empty()) {
        Fail(path, "[perception.camera_pose] camera_part_id must be a non-empty string");
      }
      const auto gimbal_part = gimbal_part_id == nullptr
                                   ? std::optional<std::string>{"default"}
                                   : gimbal_part_id->value<std::string>();
      if (!gimbal_part || gimbal_part->empty()) {
        Fail(path, "[perception.camera_pose] gimbal_part_id must be a non-empty string");
      }
      if (!rate || *rate <= 0 || *rate > 30) {
        Fail(path, "[perception.camera_pose] rate_hz must be an integer in [1, 30]");
      }
      config.camera_pose.model_id = *model;
      if (detection_model_id != nullptr) {
        const auto detection_model = detection_model_id->value<std::string>();
        if (!detection_model || detection_model->empty()) {
          Fail(path, "[perception.camera_pose] detection_model_id must be a non-empty string");
        }
        config.camera_pose.detection_model_id = *detection_model;
      }
      if (posture_model_id != nullptr) {
        const auto posture_model = posture_model_id->value<std::string>();
        if (!posture_model || posture_model->empty()) {
          Fail(path, "[perception.camera_pose] posture_model_id must be a non-empty string");
        }
        config.camera_pose.posture_model_id = *posture_model;
      }
      config.camera_pose.camera_part_id = *part;
      config.camera_pose.gimbal_part_id = *gimbal_part;
      config.camera_pose.rate_hz = static_cast<std::uint32_t>(*rate);
    }
    if (const toml::node* node = perception->get("camera_face")) {
      const toml::table* camera_face = node->as_table();
      if (camera_face == nullptr) Fail(path, "[perception.camera_face] must be a table");
      AdmitKeys(*camera_face, {"model_id"}, path, "[perception.camera_face]");
      const toml::node* model_id = camera_face->get("model_id");
      const auto model =
          model_id == nullptr ? std::optional<std::string>{} : model_id->value<std::string>();
      if (!model || model->empty()) {
        Fail(path, "[perception.camera_face] model_id must be a non-empty string");
      }
      config.camera_face.model_id = *model;
    }
    if (const toml::node* node = perception->get("camera_object")) {
      const toml::table* camera_object = node->as_table();
      if (camera_object == nullptr) Fail(path, "[perception.camera_object] must be a table");
      AdmitKeys(*camera_object, {"model_id", "camera_part_id", "rate_hz"}, path,
                "[perception.camera_object]");
      const toml::node* model_id = camera_object->get("model_id");
      const toml::node* camera_part_id = camera_object->get("camera_part_id");
      const toml::node* rate_hz = camera_object->get("rate_hz");
      const auto model =
          model_id == nullptr ? std::optional<std::string>{} : model_id->value<std::string>();
      if (!model || model->empty()) {
        Fail(path, "[perception.camera_object] model_id must be a non-empty string");
      }
      const auto part = camera_part_id == nullptr ? std::optional<std::string>{"default"}
                                                   : camera_part_id->value<std::string>();
      const auto rate =
          rate_hz == nullptr ? std::optional<std::int64_t>{} : rate_hz->value<std::int64_t>();
      if (!part || part->empty()) {
        Fail(path, "[perception.camera_object] camera_part_id must be a non-empty string");
      }
      if (!rate || *rate <= 0 || *rate > 30) {
        Fail(path, "[perception.camera_object] rate_hz must be an integer in [1, 30]");
      }
      config.camera_object.model_id = *model;
      config.camera_object.camera_part_id = *part;
      config.camera_object.rate_hz = static_cast<std::uint32_t>(*rate);
    }
    if (const toml::node* node = perception->get("arm_grasp")) {
      const toml::table* arm_grasp = node->as_table();
      if (arm_grasp == nullptr) Fail(path, "[perception.arm_grasp] must be a table");
      AdmitKeys(*arm_grasp, {"observe_joint_positions_rad", "wrist_camera_device_id",
                             "workspace_min_m", "workspace_max_m"}, path, "[perception.arm_grasp]");
      const auto* array_node = arm_grasp->get("observe_joint_positions_rad");
      const auto* angles = array_node == nullptr ? nullptr : array_node->as_array();
      if (angles == nullptr || angles->size() != 6)
        Fail(path, "[perception.arm_grasp] observe_joint_positions_rad must contain six angles");
      for (const auto& angle : *angles) {
        const auto value = angle.value<double>();
        if (!value || !std::isfinite(*value) || *value < -3.141592653589793 || *value > 3.141592653589793)
          Fail(path, "[perception.arm_grasp] observe angles must be finite radians in [-pi, pi]");
        config.arm_grasp.observe_joint_positions_rad.push_back(*value);
      }
      const auto camera = arm_grasp->get("wrist_camera_device_id")
                              ? arm_grasp->get("wrist_camera_device_id")->value<std::string>()
                              : std::nullopt;
      if (!camera || camera->empty())
        Fail(path, "[perception.arm_grasp] wrist_camera_device_id is required");
      config.arm_grasp.wrist_camera_device_id = *camera;
      auto ReadBounds = [&](const char* key) {
        const auto* value = arm_grasp->get(key);
        const auto* array = value == nullptr ? nullptr : value->as_array();
        if (array == nullptr || array->size() != 3)
          Fail(path, std::string("[perception.arm_grasp] ") + key + " must contain three numbers");
        std::vector<double> output;
        for (const auto& item : *array) {
          const auto coordinate = item.value<double>();
          if (!coordinate || !std::isfinite(*coordinate))
            Fail(path, std::string("[perception.arm_grasp] ") + key + " must be finite");
          output.push_back(*coordinate);
        }
        return output;
      };
      config.arm_grasp.workspace_min_m = ReadBounds("workspace_min_m");
      config.arm_grasp.workspace_max_m = ReadBounds("workspace_max_m");
      for (std::size_t axis = 0; axis < 3; ++axis)
        if (config.arm_grasp.workspace_min_m[axis] >= config.arm_grasp.workspace_max_m[axis])
          Fail(path, "[perception.arm_grasp] workspace bounds must increase on every axis");
    }
    if (const toml::node* node = perception->get("microphone_vad")) {
      const toml::table* microphone_vad = node->as_table();
      if (microphone_vad == nullptr) Fail(path, "[perception.microphone_vad] must be a table");
      AdmitKeys(*microphone_vad,
                {"model_id", "asr_model_id", "window_ms", "save_audio", "overwrite_audio"}, path,
                "[perception.microphone_vad]");
      const toml::node* model_id = microphone_vad->get("model_id");
      const toml::node* asr_model_id = microphone_vad->get("asr_model_id");
      const toml::node* window_ms = microphone_vad->get("window_ms");
      const toml::node* save_audio = microphone_vad->get("save_audio");
      const toml::node* overwrite_audio = microphone_vad->get("overwrite_audio");
      const auto model =
          model_id == nullptr ? std::optional<std::string>{} : model_id->value<std::string>();
      const auto asr_model = asr_model_id == nullptr ? std::optional<std::string>("asr")
                                                     : asr_model_id->value<std::string>();
      const auto window =
          window_ms == nullptr ? std::optional<std::int64_t>{} : window_ms->value<std::int64_t>();
      const auto save =
          save_audio == nullptr ? std::optional<bool>(false) : save_audio->value<bool>();
      const auto overwrite =
          overwrite_audio == nullptr ? std::optional<bool>(false) : overwrite_audio->value<bool>();
      if (!model || model->empty()) {
        Fail(path, "[perception.microphone_vad] model_id must be a non-empty string");
      }
      if (!asr_model || asr_model->empty()) {
        Fail(path, "[perception.microphone_vad] asr_model_id must be a non-empty string");
      }
      if (!window || *window < 20 || *window > 30000) {
        Fail(path, "[perception.microphone_vad] window_ms must be an integer in [20, 30000]");
      }
      if (!save) Fail(path, "[perception.microphone_vad] save_audio must be a boolean");
      if (!overwrite) {
        Fail(path, "[perception.microphone_vad] overwrite_audio must be a boolean");
      }
      if (*overwrite && !*save) {
        Fail(path, "[perception.microphone_vad] overwrite_audio requires save_audio = true");
      }
      config.microphone_vad.model_id = *model;
      config.microphone_vad.asr_model_id = *asr_model;
      config.microphone_vad.window_ms = static_cast<std::uint32_t>(*window);
      config.microphone_vad.save_audio = *save;
      config.microphone_vad.overwrite_audio = *overwrite;
    }
  }

  config.mode = hardware_runtime::ParseMode(contents.str(), path);
  config.models = models::ParseConfig(contents.str(), path);
  return config;
}

}  // namespace eos::runtime::init
