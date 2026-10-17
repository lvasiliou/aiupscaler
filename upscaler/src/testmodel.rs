//! A tiny ONNX "upscaler" (one bilinear x2 Resize op), built in memory. Used to check
//! that an inference device really works, and by the `make_test_model` example.

/// Minimal protobuf writer: just the wire types ONNX needs here.
#[derive(Default)]
struct Pb(Vec<u8>);

impl Pb {
    fn varint(&mut self, mut v: u64) {
        while v >= 0x80 {
            self.0.push((v as u8) | 0x80);
            v >>= 7;
        }
        self.0.push(v as u8);
    }
    fn int(mut self, field: u64, v: u64) -> Self {
        self.varint(field << 3);
        self.varint(v);
        self
    }
    fn bytes(mut self, field: u64, b: &[u8]) -> Self {
        self.varint((field << 3) | 2);
        self.varint(b.len() as u64);
        self.0.extend_from_slice(b);
        self
    }
    fn str(self, field: u64, s: &str) -> Self {
        self.bytes(field, s.as_bytes())
    }
    fn msg(self, field: u64, m: Pb) -> Self {
        self.bytes(field, &m.0)
    }
}

/// ValueInfoProto for a float NCHW tensor with 3 channels and symbolic H/W
/// (CHW, without the batch dimension, when `batched` is false).
fn image_value(name: &str, h: &str, w: &str, batched: bool) -> Pb {
    let dim_val = |v| Pb::default().int(1, v);
    let dim_sym = |s| Pb::default().str(2, s);
    let shape = if batched { Pb::default().msg(1, dim_val(1)) } else { Pb::default() };
    let shape = shape.msg(1, dim_val(3)).msg(1, dim_sym(h)).msg(1, dim_sym(w));
    let tensor_type = Pb::default().int(1, 1).msg(2, shape); // elem_type FLOAT
    Pb::default().str(1, name).msg(2, Pb::default().msg(1, tensor_type))
}

/// Serialized ONNX model: input `x` [1,3,h,w] -> output `y` [1,3,2h,2w].
pub fn resize_x2_model() -> Vec<u8> {
    build_resize_x2(false)
}

/// Like [`resize_x2_model`], but the output drops the batch dimension: `y` [3,2h,2w], as some
/// exported upscalers do.
#[cfg(test)]
pub fn resize_x2_squeezed_model() -> Vec<u8> {
    build_resize_x2(true)
}

fn build_resize_x2(squeeze: bool) -> Vec<u8> {
    let scales: Vec<u8> = [1.0f32, 1.0, 2.0, 2.0].iter().flat_map(|f| f.to_le_bytes()).collect();
    let scales_init = Pb::default().int(1, 4).int(2, 1).str(8, "scales").bytes(9, &scales);
    let mode = Pb::default().str(1, "mode").bytes(4, b"linear").int(20, 3); // AttributeProto STRING
    let node = Pb::default()
        .str(1, "x")
        .str(1, "")
        .str(1, "scales")
        .str(2, if squeeze { "resized" } else { "y" })
        .str(3, "resize")
        .str(4, "Resize")
        .msg(5, mode);
    let mut graph = Pb::default().msg(1, node);
    if squeeze {
        let axes = Pb::default().int(1, 1).int(2, 7).str(8, "axes").bytes(9, &0i64.to_le_bytes()); // INT64 [0]
        let squeeze_node = Pb::default().str(1, "resized").str(1, "axes").str(2, "y").str(3, "squeeze").str(4, "Squeeze");
        graph = graph.msg(1, squeeze_node).msg(5, axes);
    }
    let graph = graph
        .str(2, "test_x2")
        .msg(5, scales_init)
        .msg(11, image_value("x", "h", "w", true))
        .msg(12, image_value("y", "oh", "ow", !squeeze));
    let model = Pb::default()
        .int(1, 8) // ir_version
        .str(2, "ai-upscaler")
        .msg(7, graph)
        .msg(8, Pb::default().str(1, "").int(2, 13)); // opset 13
    model.0
}
