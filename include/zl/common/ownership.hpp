#pragma once

namespace zl {

enum class OwnershipKind {
    GC,
    OWNED,
    BORROW,
    SHARED,
};

inline const char* ownershipName(OwnershipKind kind) noexcept {
    switch (kind) {
        case OwnershipKind::GC: return "gc";
        case OwnershipKind::OWNED: return "owned";
        case OwnershipKind::BORROW: return "borrow";
        case OwnershipKind::SHARED: return "shared";
    }
    return "gc";
}

} // namespace zl
