import UIKit

private final class ScrollbarViewDelegate: NSObject, UIScrollViewDelegate {
    weak var innerDelegate: UIScrollViewDelegate?

    func scrollViewDidScroll(_ scrollView: UIScrollView) {
        guard let scrollView = scrollView as? ScrollbarView else {
            innerDelegate?.scrollViewDidScroll?(scrollView)
            return
        }

        var frame = scrollView.contentView?.frame ?? .zero
        frame.origin.x = scrollView.contentOffset.x + scrollView.contentViewOrigin.x
        frame.origin.y = scrollView.contentOffset.y + scrollView.contentViewOrigin.y
        scrollView.contentView?.frame = frame
        innerDelegate?.scrollViewDidScroll?(scrollView)
    }

    override func forwardingTarget(for selector: Selector!) -> Any? {
        if innerDelegate?.responds(to: selector) == true {
            return innerDelegate
        }
        return super.forwardingTarget(for: selector)
    }
}

@objc(ScrollbarView)
final class ScrollbarView: UIScrollView {
    fileprivate var contentViewOrigin: CGPoint = .zero
    private let outerDelegate = ScrollbarViewDelegate()

    @objc var contentView: UIView? {
        didSet {
            contentViewOrigin = contentView?.frame.origin ?? .zero
        }
    }

    override var delegate: UIScrollViewDelegate? {
        get {
            outerDelegate.innerDelegate
        }
        set {
            outerDelegate.innerDelegate = newValue
        }
    }

    override init(frame: CGRect) {
        super.init(frame: frame)
        super.delegate = outerDelegate
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        super.delegate = outerDelegate
    }
}
