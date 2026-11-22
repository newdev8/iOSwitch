import Foundation
import HWTestCore
import Metal

/// Blits Metal takes that the driver does not yet run as Metal does (gpu.gm20b.ms_buffer_copy,
/// gpu.gm20b.integer_mipmaps): a 4-sample texture copied to a buffer at the full row of its samples, and
/// generateMipmapsForTexture: of an integer format.
public enum BlitsOpen {
    public static let msSide = 64, msSamples = 4

    public struct MsCopy: Sendable {
        public var path = "-"
        /// Words at (y * 1024 + (x * 4 + s) * 4) holding sample s of texel (x, y), of 16384; bytes past the 64 rows
        /// changed
        public var inOrder = -1, past = -1
    }

    /// A 4-sample RGBA8 64x64 whose sample s of texel (x, y) holds (x, y, s, 0xa0) (mirrored_sample_codes, shaded per
    /// sample), copied whole by copyFromTexture:toBuffer: at bytesPerRow 1024 (64 texels of 4 samples of 4 bytes) into
    /// a buffer of 0x22 bytes 64 KiB long and 4 KiB more
    public static func msToBuffer(_ gpu: GPU) async throws -> MsCopy {
        let texture = try gpu.texture(.rgba8Unorm, msSide, msSide, samples: msSamples)
        let state = try gpu.render(
            vertex: "mirrored_cover_vertex", fragment: "mirrored_sample_codes", format: .rgba8Unorm,
            samples: msSamples
        )
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            encoder.endEncoding()
        }
        let row = msSide * msSamples * 4, image = row * msSide
        let out = try gpu.buffer([UInt8](repeating: 0x22, count: image + 4096))
        var copy = MsCopy()
        copy.path = try await gpu.run(tolerateError: true) { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: texture, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(),
                sourceSize: MTLSize(width: msSide, height: msSide, depth: 1), to: out, destinationOffset: 0,
                destinationBytesPerRow: row, destinationBytesPerImage: image
            )
            blit.endEncoding()
        }.facadePath
        let bytes = UnsafeRawBufferPointer(start: out.contents(), count: image + 4096)
        var inOrder = 0
        for y in 0 ..< msSide {
            for x in 0 ..< msSide {
                for s in 0 ..< msSamples {
                    let at = y * row + (x * msSamples + s) * 4
                    let want: [UInt8] = [UInt8(x), UInt8(y), UInt8(s), 0xa0]
                    if Array(bytes[at ..< (at + 4)]) == want { inOrder += 1 }
                }
            }
        }
        copy.inOrder = inOrder
        copy.past = bytes[image...].filter { $0 != 0x22 }.count
        return copy
    }

    public static let mipWidth = 63, mipHeight = 61, mipLevels = 6

    public struct Mips: Sendable {
        public var path = "-"
        /// Per level from 1: texels other than the previous level's texel at (2x + 1, 2y + 1) (clamped to its last), of
        /// how many
        public var off: [Int] = [], texels: [Int] = []
    }

    /// generateMipmapsForTexture: of an RGBA8Uint 63x61 of 6 levels whose level 0 texel (x, y) holds (x, y, 7, 9),
    /// every other level 0x22 bytes first: each level against the previous level's texel at (2x + 1, 2y + 1), clamped
    /// to its last column and row
    public static func integerMipmaps(_ gpu: GPU) async throws -> Mips {
        let texture = try gpu.texture(
            .rgba8Uint, mipWidth, mipHeight, usage: [.shaderRead, .renderTarget], mipmaps: mipLevels
        )
        var level0: [UInt8] = []
        for y in 0 ..< mipHeight {
            for x in 0 ..< mipWidth { level0 += [UInt8(x), UInt8(y), 7, 9] }
        }
        try await gpu.upload(level0, to: texture, bytesPerRow: mipWidth * 4)
        for level in 1 ..< mipLevels {
            let (w, h) = (max(1, mipWidth >> level), max(1, mipHeight >> level))
            try await gpu.upload(
                [UInt8](repeating: 0x22, count: w * h * 4), to: texture, bytesPerRow: w * 4, level: level
            )
        }
        var out = Mips()
        out.path = try await gpu.run(tolerateError: true) { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.generateMipmaps(for: texture)
            blit.endEncoding()
        }.facadePath
        var above = level0, (aw, ah) = (mipWidth, mipHeight)
        for level in 1 ..< mipLevels {
            let (w, h) = (max(1, mipWidth >> level), max(1, mipHeight >> level))
            let got = try await gpu.read(texture, bytesPerPixel: 4, level: level)
            var off = 0
            for y in 0 ..< h {
                for x in 0 ..< w {
                    let (sx, sy) = (min(2 * x + 1, aw - 1), min(2 * y + 1, ah - 1))
                    let from = (sy * aw + sx) * 4, at = (y * w + x) * 4
                    if got[at ..< (at + 4)] != above[from ..< (from + 4)] { off += 1 }
                }
            }
            out.off.append(off)
            out.texels.append(w * h)
            (above, aw, ah) = (got, w, h)
        }
        return out
    }
}
