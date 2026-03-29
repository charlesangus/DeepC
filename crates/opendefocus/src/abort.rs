use std::sync::{
    Arc, LazyLock,
    atomic::{AtomicBool, Ordering},
};

static ABORTED: LazyLock<Arc<AtomicBool>> = LazyLock::new(|| Arc::new(AtomicBool::new(false)));

/// Gets the current abort status.
///
/// This can be read across threads to allow for early exits if application has abort status set
pub fn get_aborted() -> bool {
    ABORTED.load(Ordering::SeqCst)
}

/// Sets the abort status.
pub fn set_aborted(status: bool) {
    ABORTED.store(status, Ordering::SeqCst);
}
