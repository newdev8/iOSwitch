import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The facade's compression thread, waited for by its own event (drivers/TegraGM20BMetal/
/// TegraGM20BMetalProbe.mm switchMetalProbeCompressionSettled:, debug builds): the block runs once every compressed memory asked
/// of the thread so far is made or refused, at once on the caller's thread when none is asked. It covers compressed
/// memory alone: the ZBC entries the same thread adds after it are not waited for. Past `bound` the event is taken as
/// lost (a FAIL naming it), never waited for again.
public enum CompressionSettled {
    static let selector = NSSelectorFromString("switchMetalProbeCompressionSettled:")
    static let factsSelector = NSSelectorFromString("switchMetalProbeFacts:")
    public static let bound: TimeInterval = 5

    public enum Wait: Equatable, Sendable {
        /// A release facade or another device: no event to wait for
        case absent
        case settled(milliseconds: Double)
        case never
    }

    typealias Call = @convention(c) (AnyObject, Selector, @convention(block) () -> Void) -> Unmanaged<AnyObject>?

    public static func wait(_ device: MTLDevice) async -> Wait {
        guard let facade = device as? NSObject, facade.responds(to: selector) else { return .absent }
        let call = unsafeBitCast(facade.method(for: selector), to: Call.self)
        let waiter = Waiter<Double>()
        let start = CFAbsoluteTimeGetCurrent()
        let refusal = call(facade, selector) { waiter.resume((CFAbsoluteTimeGetCurrent() - start) * 1000) }
        guard refusal == nil, let milliseconds = await waiter.wait(timeout: bound) else { return .never }
        return .settled(milliseconds: milliseconds)
    }

    /// `wait`, throwing where the event never came
    public static func settle(_ device: MTLDevice) async throws {
        if await wait(device) == .never {
            throw TestFailure("the compression thread never settled within \(Int(bound)) s")
        }
    }

    /// What `make` makes, on compressed memory where the facade can bind some: a size's memory is made by the
    /// compression thread after the first texture that wants it, which binds none, so where the first one did not, a
    /// second is made once the thread settled. The first stays where `make` put it until the caller lets it go.
    public static func remade(_ device: MTLDevice, _ make: () throws -> MTLTexture) async throws -> MTLTexture {
        let first = try make()
        guard comptags(device, first) == 0 else { return first }
        try await settle(device)
        return try make()
    }

    /// comptags= of switchMetalProbeFacts: (nil without the selector)
    public static func comptags(_ device: MTLDevice, _ texture: MTLTexture) -> Int? {
        guard let facade = device as? NSObject, facade.responds(to: factsSelector) else { return nil }
        let line = facade.perform(factsSelector, with: texture)?.takeUnretainedValue() as? String ?? ""
        return line.contains("comptags=1") ? 1 : 0
    }
}
