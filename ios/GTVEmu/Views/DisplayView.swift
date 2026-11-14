// عرض شاشة الضيف عبر Metal: يسحب أحدث إطار من virtio-gpu (BGRA) مع كل تحديث للشاشة،
// ويرسمه بنسبة العرض إلى الارتفاع الصحيحة، ويحوّل اللمس إلى إحداثيات شاشة اللمس الافتراضية (1280×720).
import SwiftUI
import MetalKit

struct DisplayView: UIViewRepresentable {
    var touchEnabled: Bool = true

    func makeCoordinator() -> Renderer { Renderer() }

    func makeUIView(context: Context) -> TouchMTKView {
        let v = TouchMTKView(frame: .zero, device: MTLCreateSystemDefaultDevice())
        v.colorPixelFormat = .bgra8Unorm
        v.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        v.preferredFramesPerSecond = 60
        v.isMultipleTouchEnabled = false
        v.delegate = context.coordinator
        v.renderer = context.coordinator
        context.coordinator.setup(device: v.device!)
        return v
    }

    func updateUIView(_ v: TouchMTKView, context: Context) {
        v.touchEnabled = touchEnabled
    }

    // MARK: - المرسِم

    final class Renderer: NSObject, MTKViewDelegate {
        private var queue: MTLCommandQueue?
        private var pipeline: MTLRenderPipelineState?
        private var texture: MTLTexture?
        private var staging = [UInt8]()
        private var lastGen: UInt64 = 0
        private(set) var guestSize = CGSize(width: 1280, height: 720)

        private static let shader = """
        #include <metal_stdlib>
        using namespace metal;
        struct VOut { float4 pos [[position]]; float2 uv; };
        vertex VOut vmain(uint vid [[vertex_id]], constant float4 &rect [[buffer(0)]]) {
            float2 p[4] = { float2(0,0), float2(1,0), float2(0,1), float2(1,1) };
            float2 q = p[vid];
            VOut o;
            o.pos = float4(rect.x + q.x * rect.z, rect.y - q.y * rect.w, 0, 1);
            o.uv = q;
            return o;
        }
        fragment float4 fmain(VOut in [[stage_in]], texture2d<float> t [[texture(0)]]) {
            constexpr sampler s(filter::linear, address::clamp_to_edge);
            return float4(t.sample(s, in.uv).rgb, 1.0);
        }
        """

        func setup(device: MTLDevice) {
            queue = device.makeCommandQueue()
            do {
                let lib = try device.makeLibrary(source: Self.shader, options: nil)
                let d = MTLRenderPipelineDescriptor()
                d.vertexFunction = lib.makeFunction(name: "vmain")
                d.fragmentFunction = lib.makeFunction(name: "fmain")
                d.colorAttachments[0].pixelFormat = .bgra8Unorm
                pipeline = try device.makeRenderPipelineState(descriptor: d)
            } catch {
                NSLog("gtv metal: \(error)")
            }
        }

        /// مستطيل الصورة داخل العرض (بالنقاط) بعد الحفاظ على النسبة
        func imageRect(in bounds: CGSize) -> CGRect {
            let s = min(bounds.width / guestSize.width, bounds.height / guestSize.height)
            let w = guestSize.width * s, h = guestSize.height * s
            return CGRect(x: (bounds.width - w) / 2, y: (bounds.height - h) / 2, width: w, height: h)
        }

        func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

        func draw(in view: MTKView) {
            updateTexture(device: view.device!)
            guard let q = queue, let pipe = pipeline, let pass = view.currentRenderPassDescriptor,
                  let drawable = view.currentDrawable, let cb = q.makeCommandBuffer(),
                  let enc = cb.makeRenderCommandEncoder(descriptor: pass) else { return }
            if let tex = texture {
                let r = imageRect(in: view.bounds.size)
                let W = view.bounds.width, H = view.bounds.height
                // تحويل المستطيل إلى إحداثيات NDC: (يسار، أعلى، عرض، ارتفاع)
                var rect = SIMD4<Float>(Float(r.minX / W * 2 - 1), Float(1 - r.minY / H * 2),
                                        Float(r.width / W * 2), Float(r.height / H * 2))
                enc.setRenderPipelineState(pipe)
                enc.setVertexBytes(&rect, length: MemoryLayout<SIMD4<Float>>.size, index: 0)
                enc.setFragmentTexture(tex, index: 0)
                enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            enc.endEncoding()
            cb.present(drawable)
            cb.commit()
        }

        private func updateTexture(device: MTLDevice) {
            var w: UInt32 = 0, h: UInt32 = 0
            let gen = gtv_copy_frame(nil, 0, &w, &h)
            guard gen != 0, gen != lastGen, w > 0, h > 0 else { return }
            let need = Int(w) * Int(h) * 4
            if staging.count != need { staging = [UInt8](repeating: 0, count: need) }
            let got = staging.withUnsafeMutableBytes { gtv_copy_frame($0.baseAddress, need, &w, &h) }
            guard got != 0, Int(w) * Int(h) * 4 == need else { return }
            lastGen = got
            if texture == nil || texture!.width != Int(w) || texture!.height != Int(h) {
                let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: Int(w), height: Int(h), mipmapped: false)
                d.usage = .shaderRead
                texture = device.makeTexture(descriptor: d)
                guestSize = CGSize(width: Int(w), height: Int(h))
            }
            staging.withUnsafeBytes {
                texture?.replace(region: MTLRegionMake2D(0, 0, Int(w), Int(h)), mipmapLevel: 0,
                                 withBytes: $0.baseAddress!, bytesPerRow: Int(w) * 4)
            }
        }
    }
}

/// MTKView يلتقط اللمس ويرسله إلى شاشة اللمس الافتراضية
final class TouchMTKView: MTKView {
    weak var renderer: DisplayView.Renderer?
    var touchEnabled = true
    private let touchW: CGFloat = 1280, touchH: CGFloat = 720

    private func guestPoint(_ t: UITouch) -> (Int32, Int32)? {
        guard let r = renderer?.imageRect(in: bounds.size), r.width > 0 else { return nil }
        let p = t.location(in: self)
        let x = (p.x - r.minX) / r.width * touchW, y = (p.y - r.minY) / r.height * touchH
        return (Int32(max(0, min(touchW - 1, x))), Int32(max(0, min(touchH - 1, y))))
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard touchEnabled, let t = touches.first, let g = guestPoint(t) else { return }
        EmulatorEngine.shared.touch(x: g.0, y: g.1, phase: 0)
    }
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard touchEnabled, let t = touches.first, let g = guestPoint(t) else { return }
        EmulatorEngine.shared.touch(x: g.0, y: g.1, phase: 1)
    }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard touchEnabled, let t = touches.first, let g = guestPoint(t) else { return }
        EmulatorEngine.shared.touch(x: g.0, y: g.1, phase: 2)
    }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        touchesEnded(touches, with: event)
    }
}
