//! One admitted deployment input passed from graph preparation to the manager.
//! The type is generic so the domain does not depend on runtime process types.

use anyhow::{Result, ensure};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Ownership {
  Managed,
  Legacy,
}

/// Owns the exact graph fence, ordered process specifications, and resolved
/// spawn secrets for one admission. The caller may not substitute any of these
/// values independently after constructing the plan.
pub struct LaunchPlan<S, P> {
  revision: String,
  specs: Vec<S>,
  secrets: P,
  ownership: Ownership,
}

impl<S, P> LaunchPlan<S, P> {
  pub fn new(revision: String, specs: Vec<S>, secrets: P, ownership: Ownership) -> Result<Self> {
    ensure!(
      revision.len() == 64
        && revision
          .bytes()
          .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte)),
      "launch plan requires an exact lowercase saved-byte SHA-256 revision"
    );
    Ok(Self {
      revision,
      specs,
      secrets,
      ownership,
    })
  }

  pub fn revision(&self) -> &str {
    &self.revision
  }

  pub fn specs(&self) -> &[S] {
    &self.specs
  }

  pub fn secrets(&self) -> &P {
    &self.secrets
  }

  pub fn ownership(&self) -> Ownership {
    self.ownership
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn plan_keeps_one_revision_and_rejects_noncanonical_fence() {
    let plan = LaunchPlan::new("a".repeat(64), vec!["eos"], (), Ownership::Managed).unwrap();
    assert_eq!(plan.revision(), "a".repeat(64));
    assert_eq!(plan.specs(), &["eos"]);
    assert_eq!(plan.ownership(), Ownership::Managed);
    assert!(LaunchPlan::new("A".repeat(64), Vec::<()>::new(), (), Ownership::Legacy).is_err());
    assert!(LaunchPlan::new(String::new(), Vec::<()>::new(), (), Ownership::Legacy).is_err());
  }
}
