#include "zl/vm/runtime_exception.hpp"
#include "zl/vm/gc.hpp"

#include <stdexcept>

namespace zl {
ZlThrownException::ZlThrownException(ObjectRef value) : value_(value) {
    if (value_) root_ = std::make_shared<ProtectedGCRoot>(value_.get());
}
ZlThrownException::~ZlThrownException() = default;

StoredException::StoredException(const std::exception_ptr& error, Retention retention) {
    if (!error) return;
    try {
        std::rethrow_exception(error);
    } catch (const ZlThrownException& exception) {
        managed_ = exception.value();
        // Only an untraced holder pins. A traced holder (Task, static field)
        // reports the payload through appendGCRoots instead: pinning there
        // would make any cycle running back through the failed program
        // permanently unreclaimable.
        if (managed_ && retention == Retention::Untraced)
            root_ = std::make_shared<ProtectedGCRoot>(managed_.get());
        message_ = managed_ ? managed_->className : "ZL exception";
        if (managed_) {
            const auto message = managed_->fields.find("message");
            if (message != managed_->fields.end()) message_ = valueToString(message->second);
        }
    } catch (const std::exception& exception) {
        native_ = error;
        message_ = exception.what();
    } catch (...) {
        native_ = error;
        message_ = "unknown exception";
    }
}

void StoredException::rethrow() const {
    if (managed_) throw ZlThrownException(managed_);
    if (native_) std::rethrow_exception(native_);
    throw std::logic_error("cannot rethrow an empty stored failure");
}

void StoredException::appendGCRoots(std::vector<Value>& roots) const {
    if (managed_) roots.emplace_back(managed_);
}
} // namespace zl
