use std::sync::Arc;
use std::time::Duration;

use aether_common::time::SystemClock;
use aether_hal::Profile;
use aether_runtime::grpc::{v1, CameraCache, Gateway, Projection, ResourceStore, ServerHandle};
use aether_runtime::Runtime;
use tonic::Request;
use tokio_stream::StreamExt;

fn fixture() -> (Gateway, Arc<ResourceStore>, String) {
  let profile = Profile::from_toml(include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../etc/eos/backends/mock-cxx.toml"
  )))
  .unwrap();
  let robot_name = profile.robot.name.clone();
  let uri = format!("robots/{robot_name}/state/test");
  let store =
    Arc::new(ResourceStore::admitted(&robot_name, &[(uri.clone(), "state".into())]).unwrap());
  let runtime = Arc::new(Runtime::from_cxx_mock_profile(profile, Arc::new(SystemClock)).unwrap());
  let projection =
    Projection::with_sources(runtime, Arc::new(CameraCache::default()), store.clone()).unwrap();
  (Gateway(projection), store, uri)
}

fn product(value: f64) -> prost_types::Struct {
  prost_types::Struct {
    fields: [(
      "value".into(),
      prost_types::Value {
        kind: Some(prost_types::value::Kind::NumberValue(value)),
      },
    )]
    .into(),
  }
}

#[test]
fn list_resources_returns_only_admitted_uris_in_stable_canonical_order() {
  let profile = Profile::from_toml(include_str!(concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../etc/eos/backends/mock-cxx.toml"
  )))
  .unwrap();
  let prefix = format!("robots/{}/", profile.robot.name);
  let descriptors = [
    (format!("{prefix}state/z"), "state".to_owned()),
    (format!("{prefix}events/m"), "event".to_owned()),
    (format!("{prefix}detection/a"), "detection".to_owned()),
  ];
  let store = Arc::new(ResourceStore::admitted(&profile.robot.name, &descriptors).unwrap());
  let runtime = Arc::new(Runtime::from_cxx_mock_profile(profile, Arc::new(SystemClock)).unwrap());
  let projection =
    Projection::with_sources(runtime, Arc::new(CameraCache::default()), store).unwrap();
  let mut server = ServerHandle::start(projection, 0).unwrap();
  let endpoint = format!("http://{}", server.address());
  tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap()
    .block_on(async {
      let mut wire = v1::gateway_service_client::GatewayServiceClient::connect(endpoint)
        .await
        .unwrap();
      let expected = vec![
        (format!("{prefix}detection/a"), "detection".to_owned()),
        (format!("{prefix}events/m"), "event".to_owned()),
        (format!("{prefix}state/z"), "state".to_owned()),
      ];
      for _ in 0..3 {
        let response = wire.list_resources(()).await.unwrap().into_inner();
        let actual: Vec<(String, String)> = response
          .values
          .into_iter()
          .map(|value| {
            let Some(prost_types::value::Kind::StructValue(object)) = value.kind else {
              panic!("resource descriptor is not an object");
            };
            let string = |field: &str| {
              let Some(prost_types::value::Kind::StringValue(value)) = object
                .fields
                .get(field)
                .and_then(|value| value.kind.clone())
              else {
                panic!("resource descriptor {field} is not a string");
              };
              value
            };
            assert_eq!(object.fields.len(), 2, "unexpected descriptor field");
            (string("uri"), string("class"))
          })
          .collect();
        assert_eq!(
          actual, expected,
          "resource listing changed or invented a URI"
        );
      }
    });
  server.shutdown();
}

#[test]
fn admitted_resource_read_watch_reconnect_stale_and_lag() {
  use v1::gateway_service_server::GatewayService;
  let (gateway, store, uri) = fixture();
  let mut server = ServerHandle::start(gateway.0.clone(), 0).unwrap();
  let endpoint = format!("http://{}", server.address());
  let runtime = tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap();
  runtime.block_on(async {
    let mut wire = v1::gateway_service_client::GatewayServiceClient::connect(endpoint)
      .await
      .unwrap();
    assert_eq!(
      wire
        .list_resources(())
        .await
        .unwrap()
        .into_inner()
        .values
        .len(),
      1
    );
    let listed = gateway
      .list_resources(Request::new(()))
      .await
      .unwrap()
      .into_inner();
    assert_eq!(listed.values.len(), 1);
    assert_eq!(
      gateway
        .read_resource(Request::new(v1::ResourceRequest { uri: uri.clone() }))
        .await
        .unwrap_err()
        .code(),
      tonic::Code::Unavailable
    );
    assert_eq!(
      gateway
        .read_resource(Request::new(v1::ResourceRequest {
          uri: "robots/unknown".into()
        }))
        .await
        .unwrap_err()
        .code(),
      tonic::Code::NotFound
    );
    let mut watch = gateway
      .watch_resource(Request::new(v1::ResourceRequest { uri: uri.clone() }))
      .await
      .unwrap()
      .into_inner();
    store.publish(&uri, product(1.0)).unwrap();
    let fresh = wire
      .read_resource(v1::ResourceRequest { uri: uri.clone() })
      .await
      .unwrap();
    assert!(fresh.metadata().get("eos-sample-age-ms").is_some());
    assert_eq!(fresh.into_inner(), product(1.0));
    let update = tokio::time::timeout(Duration::from_secs(1), watch.next())
      .await
      .unwrap()
      .unwrap()
      .unwrap();
    assert_eq!(update.uri, uri);
    assert!(update.resource.is_none());
    assert_eq!(
      gateway
        .read_resource(Request::new(v1::ResourceRequest { uri: uri.clone() }))
        .await
        .unwrap()
        .into_inner(),
      product(1.0)
    );
    // Reconnecting starts a new subscription and requires an explicit read.
    drop(watch);
    let mut reconnect = gateway
      .watch_resource(Request::new(v1::ResourceRequest { uri: uri.clone() }))
      .await
      .unwrap()
      .into_inner();
    store.publish(&uri, product(1.0)).unwrap();
    assert!(
      tokio::time::timeout(Duration::from_millis(30), reconnect.next())
        .await
        .is_err()
    );
    store.publish(&uri, product(2.0)).unwrap();
    assert_eq!(reconnect.next().await.unwrap().unwrap().uri, uri);
    drop(reconnect);
    let slow = gateway
      .watch_resource(Request::new(v1::ResourceRequest { uri: uri.clone() }))
      .await
      .unwrap()
      .into_inner();
    for value in 3..40 {
      store.publish(&uri, product(f64::from(value))).unwrap();
    }
    tokio::time::sleep(Duration::from_millis(150)).await;
    let mut slow = slow;
    let mut saw_exhausted = false;
    while let Some(message) = slow.next().await {
      match message {
        Ok(_) => {}
        Err(status) => {
          saw_exhausted = status.code() == tonic::Code::ResourceExhausted;
          break;
        }
      }
    }
    assert!(
      saw_exhausted,
      "slow watcher must observe bounded gap closure"
    );
    drop(slow);
    tokio::time::sleep(Duration::from_millis(10)).await;
    let mut subscriptions = Vec::new();
    for _ in 0..16 {
      subscriptions.push(
        gateway
          .watch_resource(Request::new(v1::ResourceRequest { uri: uri.clone() }))
          .await
          .unwrap()
          .into_inner(),
      );
    }
    assert_eq!(
      gateway
        .watch_resource(Request::new(v1::ResourceRequest { uri: uri.clone() }))
        .await
        .err()
        .unwrap()
        .code(),
      tonic::Code::ResourceExhausted
    );
    drop(subscriptions);
    let released = tokio::time::Instant::now() + Duration::from_secs(1);
    let recovered_watch = loop {
      match gateway
        .watch_resource(Request::new(v1::ResourceRequest { uri: uri.clone() }))
        .await
      {
        Ok(watch) => break watch,
        Err(status) if status.code() == tonic::Code::ResourceExhausted => {
          assert!(
            tokio::time::Instant::now() < released,
            "watch slots did not drain"
          );
          tokio::time::sleep(Duration::from_millis(10)).await;
        }
        Err(status) => panic!("unexpected watch error: {status}"),
      }
    };
    drop(recovered_watch);
    tokio::time::sleep(Duration::from_millis(1100)).await;
    assert_eq!(
      gateway
        .read_resource(Request::new(v1::ResourceRequest { uri }))
        .await
        .unwrap_err()
        .code(),
      tonic::Code::Unavailable
    );
  });
  server.shutdown();
}

#[test]
fn unchanged_samples_refresh_freshness_without_watch_and_full_queue_reports_gap() {
  use v1::gateway_service_server::GatewayService;
  let (gateway, store, uri) = fixture();
  let runtime = tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap();
  runtime.block_on(async {
    let mut watch = gateway
      .watch_resource(Request::new(v1::ResourceRequest { uri: uri.clone() }))
      .await
      .unwrap()
      .into_inner();
    store.publish(&uri, product(1.0)).unwrap();
    assert!(watch.next().await.unwrap().is_ok());
    tokio::time::sleep(Duration::from_millis(650)).await;
    store.publish(&uri, product(1.0)).unwrap();
    tokio::time::sleep(Duration::from_millis(650)).await;
    assert_eq!(
      gateway
        .read_resource(Request::new(v1::ResourceRequest { uri: uri.clone() }))
        .await
        .unwrap()
        .into_inner(),
      product(1.0)
    );
    assert!(
      tokio::time::timeout(Duration::from_millis(25), watch.next())
        .await
        .is_err()
    );
    for value in 2..=4 {
      store.publish(&uri, product(f64::from(value))).unwrap();
      tokio::time::sleep(Duration::from_millis(20)).await;
    }
    assert_eq!(
      watch.next().await.unwrap().unwrap_err().code(),
      tonic::Code::ResourceExhausted
    );
    assert!(watch.next().await.is_none());
  });
}

#[test]
fn no_poll_lag_releases_watch_slot_and_quiesce_does_not_wait_for_client() {
  use v1::gateway_service_server::GatewayService;
  let (gateway, store, uri) = fixture();
  let mut server = ServerHandle::start(gateway.0.clone(), 0).unwrap();
  let runtime = tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
    .unwrap();
  runtime.block_on(async {
    let mut no_poll = gateway
      .watch_resource(Request::new(v1::ResourceRequest { uri: uri.clone() }))
      .await
      .unwrap()
      .into_inner();
    for value in 1..=3 {
      store.publish(&uri, product(f64::from(value))).unwrap();
      tokio::time::sleep(Duration::from_millis(20)).await;
    }
    let mut other_watches = Vec::new();
    for _ in 0..16 {
      let deadline = tokio::time::Instant::now() + Duration::from_secs(1);
      loop {
        match gateway
          .watch_resource(Request::new(v1::ResourceRequest { uri: uri.clone() }))
          .await
        {
          Ok(watch) => {
            other_watches.push(watch.into_inner());
            break;
          }
          Err(status) if status.code() == tonic::Code::ResourceExhausted => {
            assert!(tokio::time::Instant::now() < deadline);
            tokio::time::sleep(Duration::from_millis(10)).await;
          }
          Err(status) => panic!("unexpected watch error: {status}"),
        }
      }
    }
    assert_eq!(
      no_poll.next().await.unwrap().unwrap_err().code(),
      tonic::Code::ResourceExhausted
    );
    assert!(no_poll.next().await.is_none());
    server.begin_shutdown();
    drop(other_watches);
  });
  server.shutdown();
}
