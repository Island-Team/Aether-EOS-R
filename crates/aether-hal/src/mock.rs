use std::sync::Mutex;

use crate::*;

fn healthy() -> Health {
  Health {
    state: HealthState::Healthy,
    code: None,
    message: None,
    last_ok_monotonic_ns: Some(0),
  }
}

fn ready() -> NodeStatus {
  NodeStatus {
    state: NodeState::Ready,
    active_command_id: None,
  }
}

pub struct MockCamera {
  frame: Mutex<CameraFrame>,
  health: Mutex<Health>,
  status: Mutex<NodeStatus>,
}

impl MockCamera {
  pub fn new(frame: CameraFrame) -> Self {
    Self {
      frame: Mutex::new(frame),
      health: Mutex::new(healthy()),
      status: Mutex::new(ready()),
    }
  }
  pub fn set_frame(&self, frame: CameraFrame) {
    *self.frame.lock().unwrap() = frame;
  }
  pub fn set_health(&self, health: Health) {
    *self.health.lock().unwrap() = health;
  }
}

impl NodeAdapter for MockCamera {
  fn kind(&self) -> NodeKind {
    NodeKind::Camera
  }
  fn adapter_type(&self) -> &'static str {
    "mock"
  }
  fn status(&self) -> NodeStatus {
    self.status.lock().unwrap().clone()
  }
  fn health(&self) -> Health {
    self.health.lock().unwrap().clone()
  }
  fn stop(&self) -> Result<(), HalError> {
    self.status.lock().unwrap().state = NodeState::Stopped;
    Ok(())
  }
}

impl CameraNode for MockCamera {
  fn capture(&self) -> Result<CameraFrame, HalError> {
    Ok(self.frame.lock().unwrap().clone())
  }
}

pub struct MockBase {
  observation: Mutex<BaseObservation>,
  commands: Mutex<Vec<(CommandMeta, BaseMovement)>>,
  health: Mutex<Health>,
  status: Mutex<NodeStatus>,
}

impl MockBase {
  pub fn new(observation: BaseObservation) -> Self {
    Self {
      observation: Mutex::new(observation),
      commands: Mutex::new(Vec::new()),
      health: Mutex::new(healthy()),
      status: Mutex::new(ready()),
    }
  }
  pub fn commands(&self) -> Vec<(CommandMeta, BaseMovement)> {
    self.commands.lock().unwrap().clone()
  }
  pub fn set_observation(&self, observation: BaseObservation) {
    *self.observation.lock().unwrap() = observation;
  }
  pub fn set_health(&self, health: Health) {
    *self.health.lock().unwrap() = health;
  }
}

impl NodeAdapter for MockBase {
  fn kind(&self) -> NodeKind {
    NodeKind::Base
  }
  fn adapter_type(&self) -> &'static str {
    "mock"
  }
  fn status(&self) -> NodeStatus {
    self.status.lock().unwrap().clone()
  }
  fn health(&self) -> Health {
    self.health.lock().unwrap().clone()
  }
  fn stop(&self) -> Result<(), HalError> {
    self.status.lock().unwrap().state = NodeState::Stopped;
    Ok(())
  }
}

impl BaseNode for MockBase {
  fn observe(&self) -> Result<BaseObservation, HalError> {
    Ok(self.observation.lock().unwrap().clone())
  }
  fn move_base(
    &self,
    meta: CommandMeta,
    movement: BaseMovement,
  ) -> Result<CommandOutcome, HalError> {
    let mut status = self.status.lock().unwrap();
    if status.state == NodeState::Stopped {
      return Err(HalError::Adapter("base stop is latched".into()));
    }
    status.active_command_id = Some(meta.command_id.clone());
    self.commands.lock().unwrap().push((meta, movement));
    drop(status);
    Ok(CommandOutcome::Accepted)
  }
}

pub struct MockManipulator {
  observation: Mutex<ManipulatorObservation>,
  commands: Mutex<Vec<(CommandMeta, ManipulatorMovement)>>,
  health: Mutex<Health>,
  status: Mutex<NodeStatus>,
}

impl MockManipulator {
  pub fn new(observation: ManipulatorObservation) -> Self {
    Self {
      observation: Mutex::new(observation),
      commands: Mutex::new(Vec::new()),
      health: Mutex::new(healthy()),
      status: Mutex::new(ready()),
    }
  }
  pub fn commands(&self) -> Vec<(CommandMeta, ManipulatorMovement)> {
    self.commands.lock().unwrap().clone()
  }
  pub fn set_health(&self, health: Health) {
    *self.health.lock().unwrap() = health;
  }
}

impl NodeAdapter for MockManipulator {
  fn kind(&self) -> NodeKind {
    NodeKind::Manipulator
  }
  fn adapter_type(&self) -> &'static str {
    "mock"
  }
  fn status(&self) -> NodeStatus {
    self.status.lock().unwrap().clone()
  }
  fn health(&self) -> Health {
    self.health.lock().unwrap().clone()
  }
  fn stop(&self) -> Result<(), HalError> {
    self.status.lock().unwrap().state = NodeState::Stopped;
    Ok(())
  }
}

impl ManipulatorNode for MockManipulator {
  fn observe(&self) -> Result<ManipulatorObservation, HalError> {
    Ok(self.observation.lock().unwrap().clone())
  }
  fn move_manipulator(
    &self,
    meta: CommandMeta,
    movement: ManipulatorMovement,
  ) -> Result<CommandOutcome, HalError> {
    let mut status = self.status.lock().unwrap();
    if status.state == NodeState::Stopped {
      return Err(HalError::Adapter("manipulator stop is latched".into()));
    }
    status.active_command_id = Some(meta.command_id.clone());
    self.commands.lock().unwrap().push((meta, movement));
    drop(status);
    Ok(CommandOutcome::Accepted)
  }
}
