//! Writes a tiny ONNX "upscaler" (a single bilinear x2 Resize op) for testing the
//! inference pipeline and GPU backends without downloading a real model.
//!
//! cargo run --example make_test_model -- test_x2.onnx

#[path = "../src/testmodel.rs"]
mod testmodel;

fn main() {
    let path = std::env::args().nth(1).unwrap_or_else(|| "test_x2.onnx".into());
    std::fs::write(&path, testmodel::resize_x2_model()).expect("failed to write model");
    println!("wrote {path}");
}
