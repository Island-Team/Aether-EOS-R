// Compiled against an exact copied EOS routes.rs from the selected source pin.
#[path = "../routes.rs"]
mod routes;

use aether_hal::DataClass;
use routes::{RouteConfig, Scheduler};
use serde_json::json;

fn main() {
    let config = RouteConfig {
        source_node: "eos/evidence/event/probe".into(),
        source_output: "event".into(),
        target_node: "eos/evidence/queue/probe".into(),
        target_input: "event".into(),
        data_type: "probe_event".into(),
        class: DataClass::Event,
        priority: 0,
        frequency_hz: None,
    };
    let mut scheduler = Scheduler::new(vec![config]).expect("selected FIFO route");
    let before = scheduler.status().remove(0);
    for sequence in 0..65 {
        scheduler.enqueue(0, sequence, 1).expect("bounded enqueue");
    }
    let after = scheduler.status().remove(0);
    assert_eq!(after["queue_class"], "fifo");
    assert_eq!(after["queue_depth"], 64);
    assert_eq!(after["overflow_count"], 1);
    assert_eq!(after["gap_count"], 1);
    println!("{}", json!({"source_kind":"event","bound_records":64,
        "bound_bytes":1048576,"actuator_route_affected":false,
        "before":before,"after":after}));
}
