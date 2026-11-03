use std::sync::Arc;

use aether_common::time::SystemClock;
use aether_hal::Profile;
use aether_runtime::{
  Runtime,
  grpc::{Projection, ServerHandle, v1},
  host_skills::Catalog,
};

fn runtime() -> Arc<Runtime> {
  let profile = Profile::from_toml(include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../etc/eos/backends/mock-cxx.toml"
  )))
  .unwrap();
  Arc::new(Runtime::from_cxx_mock_profile(profile, Arc::new(SystemClock)).unwrap())
}

#[test]
fn empty_admitted_catalog_serves_discovery_and_all_other_methods_stay_explicit() {
  let projection = Projection::new(runtime())
    .unwrap()
    .with_host_skills(
      Arc::new(Catalog::empty()),
      std::env::current_exe().unwrap(),
      None,
    )
    .unwrap();
  let mut server = ServerHandle::start(projection, 0).unwrap();
  let address = format!("http://{}", server.address());
  tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let channel = tonic::transport::Channel::from_shared(address)
        .unwrap()
        .connect()
        .await
        .unwrap();
      let mut health = tonic_health::pb::health_client::HealthClient::new(channel.clone());
      let state = health
        .check(tonic_health::pb::HealthCheckRequest {
          service: "island.eos.v1.SkillService".into(),
        })
        .await
        .unwrap()
        .into_inner();
      assert_eq!(
        state.status,
        tonic_health::pb::health_check_response::ServingStatus::Serving as i32
      );
      let mut skills = v1::skill_service_client::SkillServiceClient::new(channel);
      assert!(skills
        .list_skills(v1::ListSkillsRequest {})
        .await
        .unwrap()
        .into_inner()
        .skills
        .is_empty());
      assert_eq!(
        skills
          .get_skill(v1::GetSkillRequest {
            reference: Some(v1::SkillReference {
              name: "missing.skill".into(),
              version: "1.0.0".into(),
            })
          })
          .await
          .unwrap_err()
          .code(),
        tonic::Code::NotFound
      );
      assert_eq!(
        skills
          .start_execution(v1::StartExecutionRequest::default())
          .await
          .unwrap_err()
          .code(),
        tonic::Code::Unavailable
      );
      assert_eq!(
        skills
          .get_execution(v1::GetExecutionRequest {
            execution_id: "missing".into()
          })
          .await
          .unwrap_err()
          .code(),
        tonic::Code::Unavailable
      );
      assert_eq!(
        skills
          .watch_execution(v1::WatchExecutionRequest {
            execution_id: "missing".into(),
            after_sequence: 0
          })
          .await
          .unwrap_err()
          .code(),
        tonic::Code::Unavailable
      );
      assert_eq!(
        skills
          .list_executions(v1::ListExecutionsRequest {})
          .await
          .unwrap_err()
          .code(),
        tonic::Code::Unavailable
      );
      assert_eq!(
        skills
          .terminate_execution(v1::TerminateExecutionRequest {
            execution_id: "missing".into()
          })
          .await
          .unwrap_err()
          .code(),
        tonic::Code::Unavailable
      );
      assert_eq!(
        skills
          .get_execution_state(v1::GetExecutionStateRequest {
            execution_id: "missing".into()
          })
          .await
          .unwrap_err()
          .code(),
        tonic::Code::Unimplemented
      );
      assert_eq!(
        skills
          .terminate_executions(v1::TerminateExecutionsRequest::default())
          .await
          .unwrap_err()
          .code(),
        tonic::Code::Unimplemented
      );
      assert_eq!(
        skills
          .signal_execution(v1::SignalExecutionRequest::default())
          .await
          .unwrap_err()
          .code(),
        tonic::Code::Unimplemented
      );
    });
  server.shutdown();
}

#[test]
fn unbound_catalog_does_not_claim_skill_service_health() {
  let mut server = ServerHandle::start(Projection::new(runtime()).unwrap(), 0).unwrap();
  let address = format!("http://{}", server.address());
  tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let channel = tonic::transport::Channel::from_shared(address)
        .unwrap()
        .connect()
        .await
        .unwrap();
      let mut health = tonic_health::pb::health_client::HealthClient::new(channel.clone());
      let state = health
        .check(tonic_health::pb::HealthCheckRequest {
          service: "island.eos.v1.SkillService".into(),
        })
        .await
        .unwrap()
        .into_inner();
      assert_eq!(
        state.status,
        tonic_health::pb::health_check_response::ServingStatus::NotServing as i32
      );
      let mut skills = v1::skill_service_client::SkillServiceClient::new(channel);
      assert_eq!(
        skills
          .list_skills(v1::ListSkillsRequest {})
          .await
          .unwrap_err()
          .code(),
        tonic::Code::Unavailable
      );
    });
  server.shutdown();
}
