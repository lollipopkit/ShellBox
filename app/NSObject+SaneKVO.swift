import Foundation
import ObjectiveC

private var kvoObservationsKey: UInt8 = 0

@objc(KVOObservation)
final class KVOObservation: NSObject {
    private var isEnabled = true
    private weak var observedObject: NSObject?
    private let keyPath: String
    private let block: () -> Void

    init(keyPath: String, object: NSObject, block: @escaping () -> Void) {
        self.keyPath = keyPath
        observedObject = object
        self.block = block
        super.init()
    }

    override func observeValue(
        forKeyPath keyPath: String?,
        of object: Any?,
        change: [NSKeyValueChangeKey: Any]?,
        context: UnsafeMutableRawPointer?
    ) {
        block()
    }

    @objc func disable() {
        guard isEnabled else {
            return
        }
        observedObject?.removeObserver(self, forKeyPath: keyPath, context: nil)
        isEnabled = false
    }

    deinit {
        disable()
    }
}

extension NSObject {
    @objc(observe:options:usingBlock:)
    func shellBoxObserve(
        _ keyPath: String,
        options: NSKeyValueObservingOptions,
        using block: @escaping () -> Void
    ) -> KVOObservation {
        let observation = KVOObservation(keyPath: keyPath, object: self, block: block)
        addObserver(observation, forKeyPath: keyPath, options: options, context: nil)
        return observation
    }

    @objc(observe:options:owner:usingBlock:)
    func shellBoxObserve(
        _ keyPaths: [String],
        options: NSKeyValueObservingOptions,
        owner: Any,
        using block: @escaping (Any) -> Void
    ) {
        weak var weakOwner = owner as AnyObject
        let wrappedBlock = {
            guard let owner = weakOwner else {
                assertionFailure("kvo notification shouldn't come to dead object")
                return
            }
            block(owner)
        }

        objc_sync_enter(owner)
        defer {
            objc_sync_exit(owner)
        }

        let observations: NSMutableSet
        if let existing = objc_getAssociatedObject(owner, &kvoObservationsKey) as? NSMutableSet {
            observations = existing
        } else {
            observations = NSMutableSet()
            objc_setAssociatedObject(owner, &kvoObservationsKey, observations, .OBJC_ASSOCIATION_RETAIN_NONATOMIC)
        }

        for keyPath in keyPaths {
            observations.add(shellBoxObserve(keyPath, options: options, using: wrappedBlock))
        }
    }
}
