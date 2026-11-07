//! Pure selection of public read-only presentations from registered owner data.

use crate::domain::registry::release::DataType;
use serde::{Deserialize, Serialize};
use std::collections::BTreeSet;

#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Method {
  Json,
  Text,
  Chart,
  Image,
  Canvas,
  Overlay,
  Audio,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Delivery {
  Http,
  WebSocket,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Descriptor {
  pub node_path: String,
  pub port: String,
  pub data_type: DataType,
  pub representation_id: String,
  pub method: Method,
  pub delivery: Delivery,
  /// Opaque public-route key. Resolution belongs to the owning HTTP route.
  pub endpoint_id: String,
  pub run_id: String,
  pub graph_revision: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(tag = "status", rename_all = "snake_case")]
pub enum Selection<'a> {
  Selected { descriptor: &'a Descriptor },
  Unsupported,
  Unavailable,
  Stale,
  Invalid,
}

fn valid_token(value: &str) -> bool {
  !value.is_empty()
    && value.len() <= 128
    && value
      .bytes()
      .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'-' | b'/' | b'@'))
}

fn valid_id(value: &str) -> bool {
  valid_token(value) && !value.contains('/')
}

fn valid_node_path(value: &str) -> bool {
  let segments: Vec<_> = value.split('/').collect();
  segments.len() == 4
    && segments.iter().all(|segment| {
      !segment.is_empty()
        && segment.len() <= 48
        && segment
          .bytes()
          .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-'))
    })
}

fn bounded_plane(descriptor: &Descriptor) -> bool {
  let schema = &descriptor.data_type.schema;
  let Some(width) = schema["width"].as_u64() else {
    return false;
  };
  let Some(height) = schema["height"].as_u64() else {
    return false;
  };
  let channels = match descriptor.data_type.codec.as_str() {
    "rgb8" => 3,
    "rgba8" | "depth32f" => 4,
    _ => return false,
  };
  width > 0
    && height > 0
    && width
      .checked_mul(height)
      .and_then(|pixels| pixels.checked_mul(channels))
      .is_some_and(|bytes| bytes <= descriptor.data_type.max_payload_bytes)
}

fn valid_descriptor(descriptor: &Descriptor) -> bool {
  let data = &descriptor.data_type;
  if !valid_node_path(&descriptor.node_path)
    || !valid_id(&descriptor.port)
    || !valid_id(&descriptor.representation_id)
    || !valid_id(&descriptor.endpoint_id)
    || !valid_id(&descriptor.run_id)
    || descriptor.graph_revision.len() != 64
    || !descriptor
      .graph_revision
      .bytes()
      .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    || !(1..=100_000_000).contains(&data.max_payload_bytes)
    || !valid_token(&data.id)
    || !data.id.ends_with(&format!("@v{}", data.version))
    || !data.schema.is_object()
    || data.purpose.trim().is_empty()
  {
    return false;
  }

  let codec = data.codec.as_str();
  let media = data.media_type.as_str();
  match descriptor.method {
    Method::Json => codec == "json_v1" && media == "application/json",
    Method::Text => codec == "utf8_v1" && media.starts_with("text/"),
    Method::Chart => {
      codec == "json_v1"
        && media == "application/json"
        && data.schema["x-island-presentation"] == "time_series"
    }
    Method::Image => {
      (codec == "jpeg" && media == "image/jpeg") || (codec == "png" && media == "image/png")
    }
    Method::Canvas => {
      matches!(codec, "rgb8" | "rgba8" | "depth32f")
        && media == "application/octet-stream"
        && bounded_plane(descriptor)
        && (codec != "depth32f" || data.schema["x-unit"].as_str().is_some())
    }
    Method::Overlay => {
      codec == "json_v1"
        && media == "application/json"
        && data.schema["x-island-presentation"] == "detections"
    }
    Method::Audio => {
      (codec == "opus" && media == "audio/ogg") || (codec == "pcm_s16le" && media == "audio/pcm")
    }
  }
}

/// Selects the first client-supported method, then the stable representation ID.
/// An owner must supply the descriptors from its verified selected catalog.
pub fn select<'a>(
  descriptors: &'a [Descriptor],
  accept: &[Method],
  requested_path: &str,
  requested_port: &str,
  current_run: &str,
  current_revision: &str,
) -> Selection<'a> {
  if !valid_node_path(requested_path) || !valid_id(requested_port) {
    return Selection::Invalid;
  }
  if accept.is_empty()
    || accept.len() > 7
    || accept.iter().collect::<BTreeSet<_>>().len() != accept.len()
  {
    return Selection::Invalid;
  }
  let candidates: Vec<&Descriptor> = descriptors
    .iter()
    .filter(|descriptor| {
      descriptor.node_path == requested_path && descriptor.port == requested_port
    })
    .collect();
  if candidates.is_empty() {
    return Selection::Unavailable;
  }
  if candidates
    .iter()
    .any(|descriptor| !valid_descriptor(descriptor))
  {
    return Selection::Invalid;
  }
  if candidates.iter().any(|descriptor| {
    descriptor.run_id != current_run || descriptor.graph_revision != current_revision
  }) {
    return Selection::Stale;
  }
  let mut identities = BTreeSet::new();
  if candidates.iter().any(|descriptor| {
    !identities.insert((
      descriptor.node_path.as_str(),
      descriptor.port.as_str(),
      descriptor.representation_id.as_str(),
    ))
  }) {
    return Selection::Invalid;
  }
  for method in accept {
    let choice = candidates
      .iter()
      .filter(|descriptor| descriptor.method == *method)
      .min_by_key(|descriptor| descriptor.representation_id.as_str());
    if let Some(descriptor) = choice {
      return Selection::Selected { descriptor };
    }
  }
  Selection::Unsupported
}

#[cfg(test)]
mod tests {
  use super::*;
  use serde_json::json;

  fn descriptor(codec: &str, media_type: &str, method: Method) -> Descriptor {
    Descriptor {
      node_path: "eos/interface/camera/front".into(),
      port: "frame".into(),
      data_type: DataType {
        id: "camera.frame@v1.0".into(),
        version: "1.0".into(),
        purpose: "Front camera".into(),
        media_type: media_type.into(),
        codec: codec.into(),
        schema: json!({"type":"object"}),
        max_payload_bytes: 65536,
      },
      representation_id: "primary".into(),
      method,
      delivery: Delivery::WebSocket,
      endpoint_id: "front-frame".into(),
      run_id: "r_test".into(),
      graph_revision: "a".repeat(64),
    }
  }

  #[test]
  fn test_select_supported_image_returns_registered_representation() {
    let image = descriptor("jpeg", "image/jpeg", Method::Image);
    assert!(matches!(
      select(
        &[image],
        &[Method::Image],
        "eos/interface/camera/front",
        "frame",
        "r_test",
        &"a".repeat(64)
      ),
      Selection::Selected { .. }
    ));
  }

  #[test]
  fn test_select_mime_without_matching_codec_rejects_descriptor() {
    let image = descriptor("unknown", "image/jpeg", Method::Image);
    assert!(matches!(
      select(
        &[image],
        &[Method::Image],
        "eos/interface/camera/front",
        "frame",
        "r_test",
        &"a".repeat(64)
      ),
      Selection::Invalid
    ));
  }

  #[test]
  fn test_select_audio_rejects_cross_codec_media_pairs() {
    for (codec, media) in [("opus", "audio/pcm"), ("pcm_s16le", "audio/ogg")] {
      let audio = descriptor(codec, media, Method::Audio);
      assert!(matches!(
        select(
          &[audio],
          &[Method::Audio],
          "eos/interface/camera/front",
          "frame",
          "r_test",
          &"a".repeat(64)
        ),
        Selection::Invalid
      ));
    }
  }

  #[test]
  fn test_select_rejects_unqualified_data_type() {
    let mut image = descriptor("jpeg", "image/jpeg", Method::Image);
    image.data_type.id = "camera.frame@v2.0".into();
    assert!(matches!(
      select(
        &[image.clone()],
        &[Method::Image],
        "eos/interface/camera/front",
        "frame",
        "r_test",
        &"a".repeat(64)
      ),
      Selection::Invalid
    ));
    image.data_type.id = "camera.frame@v1.0".into();
    image.data_type.max_payload_bytes = 100_000_001;
    assert!(matches!(
      select(
        &[image.clone()],
        &[Method::Image],
        "eos/interface/camera/front",
        "frame",
        "r_test",
        &"a".repeat(64)
      ),
      Selection::Invalid
    ));
    image.data_type.max_payload_bytes = 65_536;
    image.data_type.schema = json!(null);
    assert!(matches!(
      select(
        &[image],
        &[Method::Image],
        "eos/interface/camera/front",
        "frame",
        "r_test",
        &"a".repeat(64)
      ),
      Selection::Invalid
    ));
  }

  #[test]
  fn test_select_previous_run_rejects_stale_description() {
    let image = descriptor("jpeg", "image/jpeg", Method::Image);
    assert!(matches!(
      select(
        &[image],
        &[Method::Image],
        "eos/interface/camera/front",
        "frame",
        "r_new",
        &"a".repeat(64)
      ),
      Selection::Stale
    ));
  }

  #[test]
  fn test_select_unimplemented_client_method_reports_unsupported() {
    let image = descriptor("jpeg", "image/jpeg", Method::Image);
    assert!(matches!(
      select(
        &[image],
        &[Method::Canvas],
        "eos/interface/camera/front",
        "frame",
        "r_test",
        &"a".repeat(64)
      ),
      Selection::Unsupported
    ));
  }

  #[test]
  fn test_select_unbounded_canvas_rejects_descriptor() {
    let mut pixels = descriptor("rgb8", "application/octet-stream", Method::Canvas);
    pixels.data_type.schema = json!({"width":4096,"height":4096});
    assert!(matches!(
      select(
        &[pixels],
        &[Method::Canvas],
        "eos/interface/camera/front",
        "frame",
        "r_test",
        &"a".repeat(64)
      ),
      Selection::Invalid
    ));
  }

  #[test]
  fn test_select_duplicate_representation_rejects_ambiguity() {
    let image = descriptor("jpeg", "image/jpeg", Method::Image);
    assert!(matches!(
      select(
        &[image.clone(), image],
        &[Method::Image],
        "eos/interface/camera/front",
        "frame",
        "r_test",
        &"a".repeat(64)
      ),
      Selection::Invalid
    ));
  }

  #[test]
  fn test_select_other_source_does_not_satisfy_requested_port() {
    let image = descriptor("jpeg", "image/jpeg", Method::Image);
    assert!(matches!(
      select(
        &[image],
        &[Method::Image],
        "eos/interface/camera/rear",
        "frame",
        "r_test",
        &"a".repeat(64)
      ),
      Selection::Unavailable
    ));
  }

  #[test]
  fn test_select_empty_or_duplicate_client_methods_rejects_request() {
    let image = descriptor("jpeg", "image/jpeg", Method::Image);
    for accepted in [vec![], vec![Method::Image, Method::Image]] {
      assert!(matches!(
        select(
          std::slice::from_ref(&image),
          &accepted,
          "eos/interface/camera/front",
          "frame",
          "r_test",
          &"a".repeat(64)
        ),
        Selection::Invalid
      ));
    }
  }
}
