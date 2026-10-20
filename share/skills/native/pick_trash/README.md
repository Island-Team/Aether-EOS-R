# `pick_trash`

## Version and purpose

`pick_trash@1.0.0` is the competition litter-observation skill. Despite the historical name, it does not grasp or use the gripper. It moves the arm to the wrist-camera observation pose, announces the detected broad litter class, and returns the arm to its initial pose.

## Input contract

`robot_name`, `arm_device_id`, and `speaker_device_id` are required non-empty strings. The observation joint pose and wrist camera come from `[perception.arm_grasp]`. `observation_hold_ms` is an optional 0–10000 ms pause at the observation pose, defaulting to 2000 ms. Additional properties are rejected.

## Execution flow

1. Move `arm_device_id` to the configured wrist-camera observation joints.
2. Hold the observation pose for the configured delay, then wait for a camera frame captured after that delay and run object detection through EOS.
3. Select the highest-confidence supported object among `peel`, `empty_box`, `bottle`, and `paper_ball`.
4. Announce `这是果皮`, `这是空盒子`, `这是空瓶`, or `这是纸团`. If no supported object is found, announce `没有发现垃圾`.
5. Move the arm back with `arm.init` and return the observation result.

The skill never calls a grasp planner, `arm.movel`, or `gripper.motion` and does not claim the gripper. It is intended to run at a room trash waypoint before the workflow continues with navigation and other tasks.

## Outcomes

Success returns `observed`, `label`, and `confidence`. `observed` is false and `label` is empty when the fresh frame contains no supported litter. Observation and announcement failures still attempt `arm.init`; failure to reset the arm is reported as `init.*` because continuing navigation would be unsafe.

## Tests and limitations

The tests cover detection announcement, empty-frame announcement, arm motion ordering, and the absence of planner and gripper calls. Detection uses the four normalized EOS labels. Original model-specific class names are not needed for this observation-only workflow.
