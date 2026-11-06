"""Mission goal → semantic plan → concurrent commands."""

from egia.api.plan import PlanEditKind
from egia.core.decision.command import parse_command_reply, parse_primitive_command


def test_plan_is_runtime_editable_without_todo_commands() -> None:
  reply = parse_command_reply(
    'plan add stage "Stage the sheet and guide at their stations."\n'
    'plan edit stage "Stage both required materials."\n'
    "plan remove obsolete"
  )

  assert reply is not None
  assert [edit.op for edit in reply.plan_edits] == [
    PlanEditKind.ADD,
    PlanEditKind.EDIT,
    PlanEditKind.REMOVE,
  ]
  assert reply.plan_edits[0].sentence == "Stage the sheet and guide at their stations."


def test_step_lines_are_one_concurrent_independent_barrier() -> None:
  reply = parse_command_reply(
    'plan add stage "Stage both required materials."\n'
    "step stage\n"
    "gripper.motion@robot-a/gripper-0 action=close\n"
    "speaker.play@robot-b/speaker-0 text=ready"
  )

  assert reply is not None
  assert reply.plan_step_id == "stage"
  assert reply.commands == [
    "gripper.motion@robot-a/gripper-0 action=close",
    "speaker.play@robot-b/speaker-0 text=ready",
  ]
  assert [action.action for action in reply.actions] == [
    "gripper.motion",
    "speaker.play",
  ]


def test_skill_command_requires_exact_version_and_robot() -> None:
  reply = parse_command_reply(
    'step inspect\nskill inspect.area@2.1.0 robot=robot-a radius_m=2 intent="inspect"'
  )

  assert reply is not None
  assert reply.actions[0].action_kind == "skill"
  assert reply.actions[0].action == "inspect.area"
  assert reply.actions[0].version == "2.1.0"
  assert reply.actions[0].robot_id == "robot-a"
  assert reply.actions[0].arguments == {"radius_m": 2}
  assert parse_command_reply("step inspect\nskill inspect.area robot=robot-a") is None
  assert (
    parse_command_reply("step inspect\nskill inspect.area@2.1.0 radius_m=2") is None
  )


def test_controller_can_request_slow_reasoning() -> None:
  reply = parse_command_reply("reason mission needs failure recovery")

  assert reply is not None
  assert reply.reasoning_request == "mission needs failure recovery"
  assert reply.actions == []
  assert reply.plan_edits == []


def test_controller_can_delegate_and_return_one_ready_action() -> None:
  reply = parse_command_reply(
    'plan add inspect "Current obstacle is inspected."\n'
    "reason complex obstacle layout needs a comprehensive plan\n"
    "step inspect\n"
    "camera.capture@robot-a/camera-0 mode=rgb"
  )

  assert reply is not None
  assert reply.reasoning_request == "complex obstacle layout needs a comprehensive plan"
  assert reply.plan_step_id == "inspect"
  assert [action.action for action in reply.actions] == ["camera.capture"]


def test_final_cannot_be_combined_with_reasoning_request() -> None:
  assert parse_command_reply("reason verify complex outcome\nfinal done") is None


def test_final_can_atomically_settle_plan_edits() -> None:
  reply = parse_command_reply(
    'plan fail move "base unavailable"\nfinal movement could not be completed'
  )

  assert reply is not None
  assert reply.final == "movement could not be completed"
  assert [edit.op for edit in reply.plan_edits] == [PlanEditKind.FAIL]


def test_control_commands_cannot_follow_final_or_step_commands() -> None:
  assert parse_command_reply('final done\nplan fail move "blocked"') is None
  assert (
    parse_command_reply(
      "step inspect\ncamera.capture@robot-a/camera-0 mode=rgb\nreason inspect later"
    )
    is None
  )


def test_one_primitive_command_is_normalized_without_accepting_prose() -> None:
  invocation = parse_primitive_command(
    'speaker.play@default/speaker-0 tone="cheerful" text="hello world"'
  )

  assert invocation is not None
  assert invocation.action == "speaker.play"
  assert invocation.robot_id == "default"
  assert invocation.device_id == "speaker-0"
  assert invocation.arguments == {"tone": "cheerful", "text": "hello world"}
  assert parse_primitive_command("say hello") is None
  assert parse_primitive_command("speaker.play@speaker-0 text=one") is None
  assert parse_primitive_command("speaker.play@default/ text=one") is None
  assert (
    parse_primitive_command("speaker.play@default/speaker-0 robot=default text=one")
    is None
  )
  assert (
    parse_primitive_command("speaker.play@default/speaker-0 text=one\nfinal done")
    is None
  )


def test_step_rejects_same_robot_and_old_parallel_separator() -> None:
  assert (
    parse_command_reply(
      "step stage\n"
      "gripper.motion@robot-a/gripper-0 action=close\n"
      "speaker.play@robot-a/speaker-0 text=ready"
    )
    is None
  )


def test_step_rejects_shared_explicit_resources() -> None:
  assert (
    parse_command_reply(
      'plan add coordinate "Coordinate work around the shared fixture."\n'
      "step coordinate\n"
      "gripper.motion@robot-a/gripper-0 action=close\n"
      "gripper.motion@robot-a/gripper-1 action=open"
    )
    is None
  )


def test_parser_rejects_removed_reply_forms() -> None:
  assert parse_command_reply('{"action":"base.velocity","args":{}}') is None
  assert parse_command_reply("base.velocity linear_x_m_s=0.1") is None
  assert parse_command_reply("run base.velocity linear_x_m_s=0.1") is None
  assert parse_command_reply("execute") is None
  assert (
    parse_command_reply(
      "step stage\n"
      "gripper.motion@robot-a/gripper-0 action=close & "
      "speaker.play@robot-b/speaker-0 text=ready"
    )
    is None
  )


def test_explicit_verification_commands_are_plan_edits() -> None:
  reply = parse_command_reply(
    'plan complete identify-owner "fresh match confidence 0.91"\n'
    "plan retry place-trash\n"
    'plan fail blocked-task "required identity model unavailable"'
  )

  assert reply is not None
  assert [edit.op for edit in reply.plan_edits] == [
    PlanEditKind.COMPLETE,
    PlanEditKind.RETRY,
    PlanEditKind.FAIL,
  ]
  assert reply.plan_edits[0].sentence == "fresh match confidence 0.91"
  assert parse_command_reply('plan complete identify-owner "   "') is None
  assert parse_command_reply('plan fail identify-owner "   "') is None
