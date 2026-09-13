// Native struct-view regressions: the ABI struct transport is a borrowed
// byte view with a size and an alignment the ABI requires. The view enforces
// those invariants at construction (power-of-two alignment, aligned data,
// null/size consistency), tracks its owner's lifetime, and packs into the C
// ABI value with all three fields intact.
#include "zl/compiler/native_ffi_transport.hpp"
#include "zl/vm/native_resource.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

namespace {

using namespace zl;

int failures = 0;

void require(bool condition, const std::string& message) {
    if (condition) return;
    std::cerr << "native struct regression: " << message << '\n';
    ++failures;
}

struct Point3 {
    std::int64_t x;
    std::int64_t y;
    std::int64_t z;
};

void testAlignmentContract() {
    alignas(8) Point3 point{1, 2, 3};
    NativeResourceOwner owner(0x100, [](NativeResourceOwner::Handle) {});

    {
        NativeStructView view(owner, &point, sizeof(point), 8);
        require(view.valid(), "an aligned struct view is valid");
        require(view.data() == &point, "the view points at the bytes");
        require(view.size() == sizeof(point), "the view carries the byte size");
        require(view.alignment() == 8, "the view carries the required alignment");
    }
    {
        bool zeroAlignment = false;
        try {
            NativeStructView view(owner, &point, sizeof(point), 0);
            (void)view;
        } catch (const std::runtime_error& e) {
            zeroAlignment = std::string(e.what()).find("power of two") != std::string::npos;
        }
        require(zeroAlignment, "a zero alignment is rejected");
    }
    {
        bool oddAlignment = false;
        try {
            NativeStructView view(owner, &point, sizeof(point), 12);
            (void)view;
        } catch (const std::runtime_error& e) {
            oddAlignment = std::string(e.what()).find("power of two") != std::string::npos;
        }
        require(oddAlignment, "a non-power-of-two alignment is rejected");
    }
    {
        // 128 is 8-aligned, so this pointer passes; a deliberately
        // misaligned buffer must not.
        alignas(8) char raw[136];
        std::memset(raw, 0, sizeof(raw));
        bool misaligned = false;
        try {
            NativeStructView view(owner, raw + 1, 8, 8);
            (void)view;
        } catch (const std::runtime_error& e) {
            misaligned = std::string(e.what()).find("not aligned") != std::string::npos;
        }
        require(misaligned, "a misaligned pointer is rejected");

        NativeStructView ok(owner, raw, 8, 8);
        require(ok.valid(), "the same buffer aligned is accepted");
    }
    {
        bool nullWithSize = false;
        try {
            NativeStructView view(owner, nullptr, 4, 8);
            (void)view;
        } catch (const std::runtime_error& e) {
            nullWithSize = std::string(e.what()).find("null data") != std::string::npos;
        }
        require(nullWithSize, "a null view with a non-zero size is rejected");
        NativeStructView empty(owner, nullptr, 0, 8);
        require(empty.valid(), "an empty (null, zero-size) view is representable");
    }
}

void testLifetimeTracking() {
    alignas(16) Point3 point{4, 5, 6};
    NativeResourceOwner owner(0x200, [](NativeResourceOwner::Handle) {});
    NativeStructView view(owner, &point, sizeof(point), 16);
    require(view.valid(), "the view is valid while the owner lives");
    owner.reset();
    require(!view.valid(), "the view dies with the owner");
    bool threw = false;
    try {
        static_cast<void>(view.data());
    } catch (const std::runtime_error&) {
        threw = true;
    }
    require(threw, "reading a dead struct view throws");
}

void testBorrowedConstruction() {
    alignas(8) Point3 point{7, 8, 9};
    NativeResourceOwner owner(0x300, [](NativeResourceOwner::Handle) {});
    NativeResourceBorrow borrow(owner);
    NativeStructView view(borrow, &point, sizeof(point), 8);
    require(view.valid() && view.data() == &point, "a view built from a borrow is valid");
    owner.reset();
    require(!view.valid(), "and it dies with the owner too");
}

void testPackIntegrity() {
    alignas(8) Point3 point{0x11, 0x22, 0x33};
    NativeResourceOwner owner(0x400, [](NativeResourceOwner::Handle) {});
    NativeStructView view(owner, &point, sizeof(point), 8);
    const auto packed = packNativeStructView(view);
    require(packed.tag == ZL_NATIVE_STRUCT_VIEW, "the tag is the struct-view tag");
    require(packed.data.structView.data == &point, "the pointer field is exact");
    require(packed.data.structView.size == sizeof(point), "the size field is exact");
    require(packed.data.structView.alignment == 8, "the alignment field is exact");

    // The bytes themselves must still be the struct's bytes.
    const auto* bytes = static_cast<const std::int64_t*>(packed.data.structView.data);
    require(bytes[0] == 0x11 && bytes[1] == 0x22 && bytes[2] == 0x33,
            "the packed view addresses the struct's fields");

    bool threw = false;
    try {
        packNativeStructView(NativeStructView{});
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()).find("invalid native struct view") != std::string::npos;
    }
    require(threw, "an invalid struct view cannot be packed");
}

void testEquality() {
    alignas(8) Point3 point{1, 2, 3};
    NativeResourceOwner owner(0x500, [](NativeResourceOwner::Handle) {});
    NativeStructView a(owner, &point, sizeof(point), 8);
    NativeStructView b(owner, &point, sizeof(point), 8);
    NativeStructView c(owner, &point, sizeof(point), 16);
    require(a == b, "identical views are equal");
    require(a != c, "different alignments are not equal");
    NativeStructView d(owner, &point, sizeof(point) - 8, 8);
    require(a != d, "different sizes are not equal");
}

} // namespace

int main() {
    testAlignmentContract();
    testLifetimeTracking();
    testBorrowedConstruction();
    testPackIntegrity();
    testEquality();

    if (failures != 0) {
        std::cerr << failures << " native struct regression(s) failed\n";
        return 1;
    }
    std::cout << "all native struct regressions passed\n";
    return 0;
}
