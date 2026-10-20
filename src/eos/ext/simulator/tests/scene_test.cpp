#include "src/eos/ext/simulator/scene.h"

#include <cassert>
#include <nlohmann/json.hpp>
int main() {
  nlohmann::json value{{"version", 2},
                       {"revision", 4},
                       {"lines", nlohmann::json::array({{{"id", "far"},
                                                         {"source_id", "far-image"},
                                                         {"center", {{"x_m", 5.0}, {"y_m", 0.0}}},
                                                         {"yaw_rad", 1.5707963267948966},
                                                         {"length_m", 2.0}},
                                                        {{"id", "near"},
                                                         {"source_id", "near-video"},
                                                         {"center", {{"x_m", 2.0}, {"y_m", 0.0}}},
                                                         {"yaw_rad", 1.5707963267948966},
                                                         {"length_m", 2.0}}})}};
  const auto scene = eos::ext::simulator::ParseScene(value);
  assert(scene.revision == 4 && scene.lines.size() == 2);
  assert(eos::ext::simulator::SceneJson(scene) == value);
  const auto hit = eos::ext::simulator::SelectSourceLine(scene, {}, 0.0, 10.0);
  assert(hit && hit->line_id == "near" && hit->source_id == "near-video");
  auto invalid = value;
  invalid["lines"][0]["source_id"] = "../escape";
  bool rejected = false;
  try {
    (void)eos::ext::simulator::ParseScene(invalid);
  } catch (...) {
    rejected = true;
  }
  assert(rejected);
}
