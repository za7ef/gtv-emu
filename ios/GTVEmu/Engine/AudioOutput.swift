// مخرج الصوت: يستقبل عينات PCM من virtio-snd ويشغّلها عبر AVAudioEngine.
// المحاكي يدفع العينات بإيقاع زمن التشغيل الحقيقي، فيكفي مخزن حلقي صغير بين الخيطين.
import AVFoundation
import os

final class AudioOutput {
    private let engine = AVAudioEngine()
    private var source: AVAudioSourceNode?
    private var rate: Double = 48000
    private var running = false

    // مخزن حلقي لعينات ستيريو بصيغة Float (يسار، يمين)
    private var ring = [Float](repeating: 0, count: 48000 * 2 * 2)
    private var readPos = 0, writePos = 0, filled = 0
    private var lock = os_unfair_lock()

    func start() {
        guard !running else { return }
        do {
            let session = AVAudioSession.sharedInstance()
            try session.setCategory(.playback, mode: .default, options: [.mixWithOthers])
            try session.setActive(true)
        } catch {
            NSLog("gtv audio session: \(error)")
        }
        configure(rate: rate)
    }

    func stop() {
        engine.stop()
        if let s = source { engine.detach(s) }
        source = nil
        running = false
        os_unfair_lock_lock(&lock); readPos = 0; writePos = 0; filled = 0; os_unfair_lock_unlock(&lock)
    }

    private func configure(rate: Double) {
        engine.stop()
        if let s = source { engine.detach(s) }
        self.rate = rate
        guard let fmt = AVAudioFormat(standardFormatWithSampleRate: rate, channels: 2) else { return }
        let node = AVAudioSourceNode(format: fmt) { [unowned self] _, _, frameCount, abl -> OSStatus in
            let buffers = UnsafeMutableAudioBufferListPointer(abl)
            guard buffers.count >= 2,
                  let l = buffers[0].mData?.assumingMemoryBound(to: Float.self),
                  let r = buffers[1].mData?.assumingMemoryBound(to: Float.self) else { return noErr }
            self.render(left: l, right: r, frames: Int(frameCount))
            return noErr
        }
        source = node
        engine.attach(node)
        engine.connect(node, to: engine.mainMixerNode, format: fmt)
        do { try engine.start(); running = true } catch { NSLog("gtv audio engine: \(error)") }
    }

    /// يُستدعى من خيط المحاكي
    func enqueue(_ pcm: UnsafePointer<Int16>, frames: Int, rate: Double, channels: Int) {
        if rate != self.rate && rate > 0 {
            DispatchQueue.main.async { [weak self] in self?.configure(rate: rate) }
            self.rate = rate
        }
        let cap = ring.count
        os_unfair_lock_lock(&lock)
        for f in 0..<frames {
            let lv = Float(pcm[f * channels]) / 32768
            let rv = channels > 1 ? Float(pcm[f * channels + 1]) / 32768 : lv
            if filled + 2 > cap { // امتلأ المخزن: نُسقط أقدم العينات
                readPos = (readPos + 2) % cap; filled -= 2
            }
            ring[writePos] = lv; ring[(writePos + 1) % cap] = rv
            writePos = (writePos + 2) % cap; filled += 2
        }
        os_unfair_lock_unlock(&lock)
    }

    private func render(left: UnsafeMutablePointer<Float>, right: UnsafeMutablePointer<Float>, frames: Int) {
        let cap = ring.count
        os_unfair_lock_lock(&lock)
        for f in 0..<frames {
            if filled >= 2 {
                left[f] = ring[readPos]; right[f] = ring[(readPos + 1) % cap]
                readPos = (readPos + 2) % cap; filled -= 2
            } else {
                left[f] = 0; right[f] = 0
            }
        }
        os_unfair_lock_unlock(&lock)
    }
}
