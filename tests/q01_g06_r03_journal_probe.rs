// Q01 test overlay: compiled only against an exact selected EOS checkout.
// The parent owner fixture supplies the counted adapter and managed journal.
include!("gateway_stop.rs");

#[test]
fn q01_archived_journal_identity_and_stop_fence() {
  let output = PathBuf::from(std::env::var("Q01_G06_R03_PROBE_OUTPUT").unwrap());
  std::fs::create_dir_all(&output).unwrap();
  let path = directory();
  let a = Arc::new(ProofBase::new());
  let b = Arc::new(ProofBase::new());
  a.movement_outcome(CommandOutcome::Accepted);
  let dispatcher = managed_fixture(
    &path,
    a.clone(),
    b,
    Arc::new(ManualClock::new(0, None)),
    "run-a",
  );
  let context = managed_context(AuthorityRole::EgiaControl, AdmissionIngress::Grpc);
  let request = move_a("q01-uncertain");
  assert_eq!(
    dispatcher
      .dispatch_managed_movement(context.clone(), request.clone())
      .status,
    OutcomeStatus::OutcomeUnknown
  );
  assert_eq!(a.movement_calls(), 1);
  assert_eq!(
    dispatcher
      .dispatch_managed_movement(context.clone(), request.clone())
      .status,
    OutcomeStatus::OutcomeUnknown
  );
  assert_eq!(a.movement_calls(), 1);
  let mut conflicting = context;
  conflicting.authority_role = AuthorityRole::OperatorControl;
  conflicting.ingress = AdmissionIngress::PrivateBridge;
  assert_eq!(
    dispatcher
      .dispatch_managed_movement(conflicting, request)
      .status,
    OutcomeStatus::Rejected
  );
  assert_eq!(a.movement_calls(), 1);
  let stopped = dispatcher.dispatch_managed_stop_checked(
    stop_context(AuthorityRole::EgiaControl, "safety.estop", None),
    stop("q01-stop", StopKind::SafetyEstop { robot_id: "r".into() }, false),
  ).result();
  assert_eq!(stopped.status, OutcomeStatus::Completed);
  assert!(dispatcher.movement_inhibited());
  assert!(a.emergency_calls.load(Ordering::SeqCst) > 0);
  let summary = format!(
    "movement_calls={}\nemergency_stop_calls={}\nmovement_inhibited={}\nstop_status={:?}\n",
    a.movement_calls(), a.emergency_calls.load(Ordering::SeqCst), dispatcher.movement_inhibited(), stopped.status
  );
  std::fs::write(output.join("probe-result.txt"), summary).unwrap();
  for item in std::fs::read_dir(&path).unwrap() {
    let item = item.unwrap();
    if item.path().extension().is_some_and(|extension| extension == "rec") {
      std::fs::copy(item.path(), output.join(item.file_name())).unwrap();
    }
  }
  drop(dispatcher);
  std::fs::remove_dir_all(path).unwrap();
}
