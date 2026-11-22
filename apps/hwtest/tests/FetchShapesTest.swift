import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Destination-reading draws with no storage form, which the driver draws as one call of its fetch macro (mme.h):
/// the GPU loops over the draw's primitives and instances, one BEGIN each behind the fetch barrier, the strip's
/// assembly kept between them (RenderEncoder::fetchDraw). Each scene (FetchShapes.swift) draws into a BGR5A1Unorm target
/// cleared to 0, which no storage image views, so its fetch is the texture unit's behind that barrier; its fragment adds
/// one red code to the colour it read, so a pixel's red counts the fragments that reached it in order. Each test proves
/// one field of the macro's draw: the primitive count, a long list, strip restart and split-mode parity, points and
/// lines, primitive_id, instance_id, the absolute vertex_id, the indirect forms, and the draws of an indirect command
/// buffer. Fields: wrong= pixels whose codes are not the scene's (first= the first, x,y:red/green/blue against the
/// scene's), path=, sequential= and triangles= the facade's fetch_sequential and fetch_triangles counters over the draws
/// (1 a draw; the draws' primitives times instances, 0 for an indirect draw; reported, not judged, for an indirect
/// command buffer's), failed_cbs= (0).
struct FetchShapeTest: HWTest {
    let id: String
    let name: String
    let shapes: [FetchShape]
    let subsystem = Subsystem.metal

    static let all: [any HWTest] = [
        FetchShapeTest(
            id: "gpu.gm20b.fetch_list", name: "A fetching list's triangles each read the last", shapes: [.list]
        ),
        FetchShapeTest(
            id: "gpu.gm20b.fetch_long", name: "A fetching list of 128 triangles, past the old 96-triangle cap",
            shapes: [.long]
        ),
        FetchShapeTest(
            id: "gpu.gm20b.fetch_strip_restart",
            name: "A fetching indexed strip keeps its winding across primitives and restarts", shapes: [.stripRestart]
        ),
        FetchShapeTest(id: "gpu.gm20b.fetch_points", name: "Fetching points each read the last", shapes: [.points]),
        FetchShapeTest(
            id: "gpu.gm20b.fetch_lines", name: "Fetching lines and a line strip each read the last", shapes: [.lines]
        ),
        FetchShapeTest(
            id: "gpu.gm20b.fetch_primitive_id", name: "A fetching draw's primitive_id counts across its primitives",
            shapes: [.primitiveID]
        ),
        FetchShapeTest(
            id: "gpu.gm20b.fetch_instances", name: "A fetching draw's instances each draw every primitive, in order",
            shapes: [.instances]
        ),
        FetchShapeTest(
            id: "gpu.gm20b.fetch_vertex_id", name: "A fetching draw's vertex_id counts from its vertexStart",
            shapes: [.vertexID]
        ),
        FetchShapeTest(
            id: "gpu.gm20b.fetch_indirect", name: "Indirect fetching draws, a list and a restarted strip",
            shapes: [.indirectList, .indirectStrip]
        ),
        FetchShapeTest(
            id: "gpu.gm20b.fetch_icb", name: "An indirect command buffer's fetching draws: a list, a strip and points",
            shapes: [.icbDraws]
        ),
        FetchShapeTest(
            id: "gpu.gm20b.fetch_icb_indexed",
            name: "An indirect command buffer's indexed fetching draws: a list, a strip and points",
            shapes: [.icbIndexedDraws]
        ),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        let before = GM20BFacade.counters(of: gpu.device)
        var wrong = 0
        var first = "-"
        for (index, shape) in shapes.enumerated() {
            let target = try gpu.texture(.bgr5A1Unorm, shape.side, shape.side, usage: [.renderTarget])
            let state = try gpu.render(vertex: shape.vertex, fragment: shape.fragment, format: .bgr5A1Unorm) {
                $0.supportIndirectCommandBuffers = shape.icb
            }
            let path = try await gpu.run(tolerateError: true) { try Self.encode(shape, state, into: target, gpu, $0) }
                .path
            checks.append(("scene \(index): its command buffer on the driver", path == "gm20b"))
            let got = Self.codes(try await gpu.read(target, bytesPerPixel: 2))
            let off = zip(got, shape.codes).enumerated().filter { $0.element.0 != $0.element.1 }
            wrong += off.count
            if let o = off.first, first == "-" {
                let (x, y) = (o.offset % shape.side, o.offset / shape.side)
                first = "\(index):\(x),\(y):\(Self.shown(o.element.0))_want_\(Self.shown(o.element.1))"
            }
        }
        let after = GM20BFacade.counters(of: gpu.device)
        let moved = { (key: String) in (after[key] ?? 0) - (before[key] ?? 0) }
        let draws = shapes.filter { !$0.icb }.flatMap(\.draws)
        let counted = draws.map(\.counted).reduce(0, +)
        metrics.set("wrong", wrong)
        metrics.set("first", first)
        metrics.set("path", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        metrics.set("sequential", moved("fetch_sequential"))
        metrics.set("triangles", moved("fetch_triangles"))
        metrics.merge(PathTally.counterMetrics(before: before, after: after))
        checks.append(("every pixel at the scene's codes", wrong == 0))
        if !draws.isEmpty {
            checks.append(
                ("fetch_sequential rose by \(draws.count), one a draw", moved("fetch_sequential") == draws.count)
            )
            checks.append(("fetch_triangles rose by \(counted)", moved("fetch_triangles") == counted))
        }
        checks.append(FailedCounts.judged(before: before, after: after, own: gpu.paths, &metrics))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func encode(
        _ shape: FetchShape, _ state: MTLRenderPipelineState, into target: MTLTexture, _ gpu: GPU,
        _ commands: MTLCommandBuffer
    ) throws {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        pass.colorAttachments[0].storeAction = .store
        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
        encoder.setRenderPipelineState(state)
        encoder.setCullMode(shape.cull)
        if shape.icb {
            try indirectCommands(shape, encoder, gpu)
            encoder.endEncoding()
            return
        }
        for draw in shape.draws {
            encoder.setVertexBuffer(try gpu.buffer(draw.points), offset: 0, index: 0)
            switch (draw.indices, draw.indirect) {
            case (nil, false):
                encoder.drawPrimitives(
                    type: draw.primitive, vertexStart: draw.vertexStart, vertexCount: draw.count,
                    instanceCount: draw.instances
                )
            case (let indices?, false):
                encoder.drawIndexedPrimitives(
                    type: draw.primitive, indexCount: draw.count, indexType: .uint16,
                    indexBuffer: try gpu.buffer(indices), indexBufferOffset: 0, instanceCount: draw.instances
                )
            case (nil, true):
                let arguments = MTLDrawPrimitivesIndirectArguments(
                    vertexCount: UInt32(draw.count), instanceCount: UInt32(draw.instances),
                    vertexStart: UInt32(draw.vertexStart), baseInstance: 0
                )
                encoder.drawPrimitives(
                    type: draw.primitive, indirectBuffer: try gpu.buffer([arguments]), indirectBufferOffset: 0
                )
            case (let indices?, true):
                let arguments = MTLDrawIndexedPrimitivesIndirectArguments(
                    indexCount: UInt32(draw.count), instanceCount: UInt32(draw.instances), indexStart: 0,
                    baseVertex: 0, baseInstance: 0
                )
                encoder.drawIndexedPrimitives(
                    type: draw.primitive, indexType: .uint16, indexBuffer: try gpu.buffer(indices),
                    indexBufferOffset: 0, indirectBuffer: try gpu.buffer([arguments]), indirectBufferOffset: 0
                )
            }
        }
        encoder.endEncoding()
    }

    /// The scene's draws as one indirect command buffer's commands, run in the encoder's pass
    static func indirectCommands(_ shape: FetchShape, _ encoder: MTLRenderCommandEncoder, _ gpu: GPU) throws {
        let indexed = shape.draws.contains { $0.indices != nil }
        let descriptor = MTLIndirectCommandBufferDescriptor()
        descriptor.commandTypes = indexed ? .drawIndexed : .draw
        descriptor.inheritPipelineState = true
        descriptor.inheritBuffers = true
        guard
            let icb = gpu.device.makeIndirectCommandBuffer(
                descriptor: descriptor, maxCommandCount: shape.draws.count, options: []
            )
        else { throw TestFailure("no indirect command buffer") }
        encoder.setVertexBuffer(try gpu.buffer(shape.draws[0].points), offset: 0, index: 0)
        for (index, draw) in shape.draws.enumerated() {
            let command = icb.indirectRenderCommandAt(index)
            if let indices = draw.indices {
                let buffer = try gpu.buffer(indices)
                encoder.useResource(buffer, usage: .read, stages: .vertex)
                command.drawIndexedPrimitives(
                    draw.primitive, indexCount: draw.count, indexType: .uint16, indexBuffer: buffer,
                    indexBufferOffset: 0, instanceCount: draw.instances, baseVertex: 0, baseInstance: 0
                )
            } else {
                command.drawPrimitives(
                    draw.primitive, vertexStart: draw.vertexStart, vertexCount: draw.count,
                    instanceCount: draw.instances, baseInstance: 0
                )
            }
        }
        encoder.executeCommandsInBuffer(icb, range: 0 ..< shape.draws.count)
    }

    /// BGR5A1Unorm's texels as (red, green, blue) codes: blue in bits 0-4, green 5-9, red 10-14 (fill_texel.h)
    static func codes(_ bytes: [UInt8]) -> [SIMD3<UInt8>] {
        stride(from: 0, to: bytes.count - 1, by: 2).map { at in
            let word = UInt16(bytes[at]) | UInt16(bytes[at + 1]) << 8
            return SIMD3(UInt8(word >> 10 & 31), UInt8(word >> 5 & 31), UInt8(word & 31))
        }
    }

    static func shown(_ c: SIMD3<UInt8>) -> String { "\(c.x)/\(c.y)/\(c.z)" }
}
