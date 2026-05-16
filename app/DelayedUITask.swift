import Foundation

@objc(DelayedUITask)
final class DelayedUITask: NSObject {
    private let target: AnyObject
    private let action: Selector
    private var timer: Timer?

    @objc(initWithTarget:action:)
    init(target: AnyObject, action: Selector) {
        self.target = target
        self.action = action
        super.init()
    }

    @objc func schedule() {
        guard timer?.isValid != true else {
            return
        }

        let timer = Timer(timeInterval: 1.0 / 60.0, repeats: false) { [weak self] _ in
            guard let self else {
                return
            }
            self.timer = nil
            _ = self.target.perform(self.action)
        }
        self.timer = timer
        RunLoop.main.add(timer, forMode: .default)
    }
}
