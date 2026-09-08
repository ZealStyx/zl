#include "zl/compiler/builtin_library.hpp"

namespace zl {
namespace {
constexpr std::string_view kBuiltinListSource = R"ZL(
class List<T> {
    list<T> __native

    func List(): void {
        this.__native = Collection.newList()
    }

    public func push(T item): void {
        Collection.push(this.__native, item)
    }

    public func pop(): T {
        return Collection.pop(this.__native)
    }

    public func get(int index): T {
        return Collection.get(this.__native, index)
    }

    public func put(int index, T item): void {
        Collection.set(this.__native, index, item)
    }

    public func length(): int {
        return Collection.length(this.__native)
    }

    public func isEmpty(): bool {
        return this.length() == 0
    }

    public func isNotEmpty(): bool {
        return this.length() != 0
    }

    public func firstOr(T fallback): T {
        if (this.isEmpty()) { return fallback }
        return this.first()
    }

    public func lastOr(T fallback): T {
        if (this.isEmpty()) { return fallback }
        return this.last()
    }

    public func remove(T item): bool {
        var index = this.indexOf(item)
        if (index < 0) { return false }
        this.removeAt(index)
        return true
    }

    public func removeAll(T item): int {
        var removed = 0
        var index = this.indexOf(item)
        while (index >= 0) {
            this.removeAt(index)
            removed = removed + 1
            index = this.indexOf(item)
        }
        return removed
    }

    public func swap(int first, int second): void {
        if (first < 0 || second < 0 || first >= this.length() || second >= this.length()) {
            throw new Exception("List.swap index out of range")
        }
        if (first == second) { return }
        var value = this.get(first)
        this.put(first, this.get(second))
        this.put(second, value)
    }

    public func take(int count): List<T> {
        if (count < 0) { throw new Exception("List.take count must be non-negative") }
        var end = count
        if (end > this.length()) { end = this.length() }
        return this.slice(0, end)
    }

    public func drop(int count): List<T> {
        if (count < 0) { throw new Exception("List.drop count must be non-negative") }
        var start = count
        if (start > this.length()) { start = this.length() }
        return this.slice(start, this.length())
    }

    public func clear(): void {
        while (this.length() > 0) {
            this.pop()
        }
    }

    public func first(): T {
        if (this.isEmpty()) {
            throw new Exception("List.first called on empty list")
        }
        return this.get(0)
    }

    public func last(): T {
        if (this.isEmpty()) {
            throw new Exception("List.last called on empty list")
        }
        return this.get(this.length() - 1)
    }

    public func insert(int index, T item): void {
        if (index < 0 || index > this.length()) {
            throw new Exception("List.insert index out of range")
        }
        if (index == this.length()) {
            this.push(item)
            return
        }
        this.push(this.last())
        var i = this.length() - 1
        while (i > index) {
            this.put(i, this.get(i - 1))
            i = i - 1
        }
        this.put(index, item)
    }

    public func removeAt(int index): T {
        if (index < 0 || index >= this.length()) {
            throw new Exception("List.removeAt index out of range")
        }
        var removed = this.get(index)
        var i = index
        while (i + 1 < this.length()) {
            this.put(i, this.get(i + 1))
            i = i + 1
        }
        this.pop()
        return removed
    }

    public func reverse(): void {
        var left = 0
        var right = this.length() - 1
        while (left < right) {
            var a = this.get(left)
            this.put(left, this.get(right))
            this.put(right, a)
            left = left + 1
            right = right - 1
        }
    }

    public func copy(): List<T> {
        var result = new List<T>()
        for i in 0..this.length() {
            result.push(this.get(i))
        }
        return result
    }

    public func prepend(T item): void {
        this.insert(0, item)
    }

    public func extend(List<T> other): void {
        for i in 0..other.length() {
            this.push(other.get(i))
        }
    }

    public func slice(int start, int end): List<T> {
        if (start < 0 || end < start || end > this.length()) {
            throw new Exception("List.slice bounds out of range")
        }
        var result = new List<T>()
        var i = start
        while (i < end) {
            result.push(this.get(i))
            i = i + 1
        }
        return result
    }

    // High-level collection algorithms are intentionally implemented in ZL.
    // They use the small native storage primitive through the existing
    // push/get/length operations, keeping policy out of C++.
    public func contains(T item): bool {
        for i in 0..this.length() {
            if (this.get(i) == item) {
                return true
            }
        }
        return false
    }

    public func indexOf(T item): int {
        for i in 0..this.length() {
            if (this.get(i) == item) {
                return i
            }
        }
        return -1
    }

    public func count(T item): int {
        var result = 0
        for i in 0..this.length() {
            if (this.get(i) == item) {
                result = result + 1
            }
        }
        return result
    }

    public func any(func predicate): bool {
        for i in 0..this.length() {
            if (predicate(this.get(i))) {
                return true
            }
        }
        return false
    }

    public func all(func predicate): bool {
        for i in 0..this.length() {
            if (!predicate(this.get(i))) {
                return false
            }
        }
        return true
    }

    public func filter(func predicate): List<T> {
        var result = new List<T>()
        for i in 0..this.length() {
            var item = this.get(i)
            if (predicate(item)) {
                result.push(item)
            }
        }
        return result
    }

    public func forEach(func action): void {
        for i in 0..this.length() {
            action(this.get(i))
        }
    }

    public func reversed(): List<T> {
        var result = new List<T>()
        var i = this.length() - 1
        while (i >= 0) {
            result.push(this.get(i))
            i = i - 1
        }
        return result
    }

    public func transform(func transform): List<T> {
        var result = new List<T>()
        for i in 0..this.length() {
            result.push(transform(this.get(i)))
        }
        return result
    }

    public func reduce(func combine, T start): T {
        var result = start
        for i in 0..this.length() {
            result = combine(result, this.get(i))
        }
        return result
    }

    // Stable insertion sort. The storage boundary remains Collection.get/put.
    public func sort(func less): void {
        var i = 1
        while (i < this.length()) {
            var current = this.get(i)
            var j = i - 1
            while (j >= 0 && less(current, this.get(j))) {
                this.put(j + 1, this.get(j))
                j = j - 1
            }
            this.put(j + 1, current)
            i = i + 1
        }
    }
}
)ZL";

constexpr std::string_view kBuiltinMapSource = R"ZL(
class Map<K, V> {
    map<K, V> __native

    func Map(): void {
        this.__native = Collection.newMap()
    }

    public func put(K key, V value): void {
        Collection.mapSet(this.__native, key, value)
    }

    public func putIfAbsent(K key, V value): bool {
        if (this.has(key)) { return false }
        this.put(key, value)
        return true
    }

    public func removeIfPresent(K key): bool {
        if (!this.has(key)) { return false }
        this.remove(key)
        return true
    }

    public func get(K key): V {
        return Collection.mapGet(this.__native, key)
    }

    public func has(K key): bool {
        return Collection.mapHas(this.__native, key)
    }

    public func remove(K key): void {
        Collection.mapRemove(this.__native, key)
    }

    public func length(): int {
        return Collection.length(this.__native)
    }

    public func keys(): List<K> {
        var result = new List<K>()
        var raw = Collection.mapKeys(this.__native)
        for i in 0..Collection.length(raw) {
            result.push(Collection.get(raw, i))
        }
        return result
    }

    public func values(): List<V> {
        var result = new List<V>()
        var raw = Collection.mapValues(this.__native)
        for i in 0..Collection.length(raw) {
            result.push(Collection.get(raw, i))
        }
        return result
    }

    public func isEmpty(): bool {
        return this.length() == 0
    }

    public func isNotEmpty(): bool {
        return this.length() != 0
    }

    public func containsKey(K key): bool {
        return this.has(key)
    }

    public func getOr(K key, V fallback): V {
        if (this.has(key)) {
            return this.get(key)
        }
        return fallback
    }

    public func clear(): void {
        var snapshot = this.keys()
        for i in 0..snapshot.length() {
            this.remove(snapshot.get(i))
        }
    }

    public func containsValue(V value): bool {
        var values = this.values()
        return values.contains(value)
    }

    public func forEach(func action): void {
        var snapshot = this.keys()
        for i in 0..snapshot.length() {
            var key = snapshot.get(i)
            action(key, this.get(key))
        }
    }
}
)ZL";

constexpr std::string_view kBuiltinSetSource = R"ZL(
class Set<T> {
    set<T> __native

    func Set(): void {
        this.__native = Collection.newSet()
    }

    public func add(T item): void {
        Collection.setAdd(this.__native, item)
    }

    public func has(T item): bool {
        return Collection.setHas(this.__native, item)
    }

    public func remove(T item): void {
        Collection.setRemove(this.__native, item)
    }

    public func length(): int {
        return Collection.length(this.__native)
    }

    public func isEmpty(): bool {
        return this.length() == 0
    }

    public func isNotEmpty(): bool {
        return this.length() != 0
    }

    public func contains(T item): bool {
        return this.has(item)
    }

    public func addAll(List<T> values): void {
        for i in 0..values.length() {
            this.add(values.get(i))
        }
    }

    public func union(Set<T> other): Set<T> {
        var result = new Set<T>()
        result.addAll(this.items())
        result.addAll(other.items())
        return result
    }

    public func intersection(Set<T> other): Set<T> {
        var result = new Set<T>()
        var snapshot = this.items()
        for i in 0..snapshot.length() {
            if (other.has(snapshot.get(i))) { result.add(snapshot.get(i)) }
        }
        return result
    }

    public func difference(Set<T> other): Set<T> {
        var result = new Set<T>()
        var snapshot = this.items()
        for i in 0..snapshot.length() {
            if (!other.has(snapshot.get(i))) { result.add(snapshot.get(i)) }
        }
        return result
    }

    public func isSubsetOf(Set<T> other): bool {
        var snapshot = this.items()
        for i in 0..snapshot.length() {
            if (!other.has(snapshot.get(i))) { return false }
        }
        return true
    }

    public func clear(): void {
        var snapshot = this.items()
        for i in 0..snapshot.length() {
            this.remove(snapshot.get(i))
        }
    }

    public func forEach(func action): void {
        var snapshot = this.items()
        for i in 0..snapshot.length() {
            action(snapshot.get(i))
        }
    }

    public func items(): List<T> {
        var result = new List<T>()
        var raw = Collection.setItems(this.__native)
        for i in 0..Collection.length(raw) {
            result.push(Collection.get(raw, i))
        }
        return result
    }
}
)ZL";

constexpr std::string_view kBuiltinOptionSource = R"ZL(
class Option<T> {
    public func isSome(): bool { return false }
    public func isNone(): bool { return true }
    public func unwrap(): T {
        throw new Exception("Option.unwrap called on None")
    }
    public func unwrapOr(T fallback): T {
        return fallback
    }
    public func expect(string message): T {
        throw new Exception(message)
    }
    public func contains(T candidate): bool { return false }
    public func isSomeAnd(func predicate): bool { return false }
    public func unwrapOrElse(func makeFallback): T { throw new Exception("Option.unwrapOrElse called on invalid Option") }
}

class Some<T> extends Option<T> {
    public T value

    public func Some(T value): void {
        this.value = value
    }

    @Override
    public func isSome(): bool { return true }
    @Override
    public func isNone(): bool { return false }
    @Override
    public func unwrap(): T { return this.value }
    @Override
    public func unwrapOr(T fallback): T { return this.value }
    @Override
    public func expect(string message): T { return this.value }
    @Override
    public func contains(T candidate): bool { return this.value == candidate }
    @Override
    public func isSomeAnd(func predicate): bool { return predicate(this.value) }
    @Override
    public func unwrapOrElse(func makeFallback): T { return this.value }
}

class None<T> extends Option<T> {
    public func None(): void { }
    @Override
    public func isSome(): bool { return false }
    @Override
    public func isNone(): bool { return true }
    @Override
    public func unwrap(): T {
        throw new Exception("Option.unwrap called on None")
    }
    @Override
    public func unwrapOr(T fallback): T { return fallback }
    @Override
    public func expect(string message): T {
        throw new Exception(message)
    }
    @Override
    public func contains(T candidate): bool { return false }
    @Override
    public func isSomeAnd(func predicate): bool { return false }
    @Override
    @Override
    public func unwrapOrElse(func makeFallback): T { return makeFallback() }
}

)ZL";

constexpr std::string_view kBuiltinResultSource = R"ZL(
class Result<T, E> {
    public func isOk(): bool { return false }
    public func isErr(): bool { return true }
    public func unwrap(): T {
        throw new Exception("Result.unwrap called on Err")
    }
    public func unwrapErr(): E {
        throw new Exception("Result.unwrapErr called on Ok")
    }
    public func unwrapOr(T fallback): T { return fallback }
    public func expect(string message): T { throw new Exception(message) }
    public func contains(T candidate): bool { return false }
    public func isOkAnd(func predicate): bool { return false }
    public func isErrAnd(func predicate): bool { return true }
    public func unwrapErrOr(E fallback): E { return fallback }
}

class Ok<T, E> extends Result<T, E> {
    public T value

    public func Ok(T value): void {
        this.value = value
    }

    @Override
    public func isOk(): bool { return true }
    @Override
    public func isErr(): bool { return false }
    @Override
    public func unwrap(): T { return this.value }
    @Override
    public func unwrapErr(): E {
        throw new Exception("Result.unwrapErr called on Ok")
    }
    @Override
    public func unwrapOr(T fallback): T { return this.value }
    @Override
    public func expect(string message): T { return this.value }
    @Override
    public func contains(T candidate): bool { return this.value == candidate }
    @Override
    public func isOkAnd(func predicate): bool { return predicate(this.value) }
    @Override
    public func isErrAnd(func predicate): bool { return false }
    @Override
    public func unwrapErrOr(E fallback): E { return fallback }
}

class Err<T, E> extends Result<T, E> {
    public E error

    public func Err(E error): void {
        this.error = error
    }

    @Override
    public func isOk(): bool { return false }
    @Override
    public func isErr(): bool { return true }
    @Override
    public func unwrap(): T {
        throw new Exception("Result.unwrap called on Err")
    }
    @Override
    public func unwrapErr(): E { return this.error }
    @Override
    public func unwrapOr(T fallback): T { return fallback }
    @Override
    public func expect(string message): T { throw new Exception(message) }
    @Override
    public func contains(T candidate): bool { return false }
    @Override
    public func isOkAnd(func predicate): bool { return false }
    @Override
    public func isErrAnd(func predicate): bool { return predicate(this.error) }
    @Override
    public func unwrapErrOr(E fallback): E { return this.error }
}

)ZL";

constexpr std::string_view kBuiltinSharedSource = R"ZL(
class Shared<T> {
    T __value

    public func Shared(T value): void {
        this.__value = value
    }

    public func get(): T {
        return Shared.__get(this)
    }

    public func setValue(T value): void {
        Shared.__set(this, value)
    }

    public func withLock(func(): T callback): T {
        return Shared.__withLock(this, callback)
    }
}
)ZL";

constexpr std::string_view kBuiltinExceptionSource = R"ZL(
class Exception {
    public string message
    public Exception cause
    public string stackTrace
    public func Exception(string message): void {
        this.message = message
        this.stackTrace = ""
    }
    public func toString(): string {
        return this.message
    }
}
)ZL";

constexpr std::string_view kBuiltinCancellationExceptionSource = R"ZL(
class CancellationException extends Exception {
    func CancellationException(string message): void {
        super(message)
    }
}
)ZL";

constexpr std::string_view kBuiltinNativeErrorSource = R"ZL(
class NativeError extends Exception {
    func NativeError(string message): void {
        super(message)
    }
}
)ZL";


constexpr std::string_view kBuiltinRegexErrorSource = R"ZL(
class RegexError extends Exception {
    func RegexError(string message): void {
        super(message)
    }
}
)ZL";

constexpr std::string_view kBuiltinReflectionErrorSource = R"ZL(
class ReflectionError extends Exception {
    func ReflectionError(string message): void {
        super(message)
    }
}
)ZL";

constexpr std::string_view kBuiltinInvalidArgumentsSource = R"ZL(
class InvalidArguments extends ReflectionError {
    func InvalidArguments(string message): void {
        super(message)
    }
}
)ZL";

constexpr std::string_view kBuiltinAccessViolationSource = R"ZL(
class AccessViolation extends ReflectionError {
    func AccessViolation(string message): void {
        super(message)
    }
}
)ZL";

constexpr std::string_view kBuiltinRegexMatchSource = R"ZL(
class RegexMatch {
    public string value
    public int start
    public int end
    public list<string> groups
    public list<bool> matched
    public map<string, string> namedGroups
    public map<string, bool> namedMatched

    public func group(int index): string {
        if (index < 0 || index >= Collection.length(this.groups)) {
            throw new Exception("RegexMatch.group index out of range")
        }
        return Collection.get(this.groups, index)
    }

    public func groupCount(): int {
        var count = Collection.length(this.groups)
        return count - 1
    }

    public func matched(int index): bool {
        if (index < 0 || index >= Collection.length(this.matched)) {
            throw new Exception("RegexMatch.matched index out of range")
        }
        return Collection.get(this.matched, index)
    }

    public func group(string name): string {
        if (!Collection.mapHas(this.namedGroups, name)) {
            throw new Exception("RegexMatch.group unknown name: " + name)
        }
        return Collection.mapGet(this.namedGroups, name)
    }

    public func matched(string name): bool {
        if (!Collection.mapHas(this.namedMatched, name)) {
            throw new Exception("RegexMatch.matched unknown name: " + name)
        }
        return Collection.mapGet(this.namedMatched, name)
    }
}
)ZL";

constexpr std::string_view kBuiltinRegexSource = R"ZL(
class Regex {
    public string pattern
    public string last

    public func Regex(string pattern): void {
        this.pattern = pattern
        this.last = ""
    }

    public static func build(): Regex {
        return Regex("")
    }

    public func source(): string { return this.pattern }

    public func then(string value): Regex {
        var escaped = String.replace(value, "\\", "\\\\")
        escaped = String.replace(escaped, ".", "\\.")
        escaped = String.replace(escaped, "^", "\\^")
        escaped = String.replace(escaped, "$", "\\$")
        escaped = String.replace(escaped, "|", "\\|")
        escaped = String.replace(escaped, "(", "\\(")
        escaped = String.replace(escaped, ")", "\\)")
        escaped = String.replace(escaped, "[", "\\[")
        escaped = String.replace(escaped, "]", "\\]")
        escaped = String.replace(escaped, "{", "\\{")
        escaped = String.replace(escaped, "}", "\\}")
        escaped = String.replace(escaped, "*", "\\*")
        escaped = String.replace(escaped, "+", "\\+")
        escaped = String.replace(escaped, "?", "\\?")
        this.pattern = this.pattern + escaped
        this.last = escaped
        return this
    }

    public func raw(Regex value): Regex {
        this.pattern = this.pattern + value.source()
        this.last = value.source()
        return this
    }

    public func digit(): Regex {
        this.pattern = this.pattern + "\\d"
        this.last = "\\d"
        return this
    }

    public func digit(int count): Regex {
        if (count < 0) { throw new Exception("Regex.digit count must be non-negative") }
        var fragment = ""
        var i = 0
        while (i < count) {
            fragment = fragment + "\\d"
            i = i + 1
        }
        this.pattern = this.pattern + fragment
        this.last = fragment
        return this
    }

    public func wordCharacter(): Regex {
        this.pattern = this.pattern + "\\w"
        this.last = "\\w"
        return this
    }

    public func unicodeProperty(string property): Regex {
        if (property == "") { throw new Exception("Regex.unicodeProperty requires a property") }
        this.pattern = this.pattern + "\\p{" + property + "}"
        this.last = "\\p{" + property + "}"
        return this
    }

    public func notUnicodeProperty(string property): Regex {
        if (property == "") { throw new Exception("Regex.notUnicodeProperty requires a property") }
        this.pattern = this.pattern + "\\P{" + property + "}"
        this.last = "\\P{" + property + "}"
        return this
    }

    public func exact(int count): Regex {
        if (count < 0) { throw new Exception("Regex.exact count must be non-negative") }
        if (this.last == "") { throw new Exception("Regex.exact requires a preceding builder element") }
        var prefix = String.substring(this.pattern, 0, String.length(this.pattern) - String.length(this.last))
        var fragment = ""
        var i = 0
        while (i < count) {
            fragment = fragment + this.last
            i = i + 1
        }
        this.pattern = prefix + fragment
        this.last = fragment
        return this
    }

    public func range(int minimum, int maximum): Regex {
        if (minimum < 0 || maximum < minimum) { throw new Exception("Regex.range requires 0 <= minimum <= maximum") }
        if (this.last == "") { throw new Exception("Regex.range requires a preceding builder element") }
        var prefix = String.substring(this.pattern, 0, String.length(this.pattern) - String.length(this.last))
        var fragment = ""
        var i = 0
        while (i < minimum) {
            fragment = fragment + this.last
            i = i + 1
        }
        while (i < maximum) {
            fragment = fragment + "(?:" + this.last + ")?"
            i = i + 1
        }
        this.pattern = prefix + fragment
        this.last = fragment
        return this
    }

    public func oneOrMore(): Regex {
        if (this.last == "") { throw new Exception("Regex.oneOrMore requires a preceding builder element") }
        var prefix = String.substring(this.pattern, 0, String.length(this.pattern) - String.length(this.last))
        this.pattern = prefix + "(?:" + this.last + ")+"
        this.last = "(?:" + this.last + ")+"
        return this
    }

    public func zeroOrMore(): Regex {
        if (this.last == "") { throw new Exception("Regex.zeroOrMore requires a preceding builder element") }
        var prefix = String.substring(this.pattern, 0, String.length(this.pattern) - String.length(this.last))
        this.pattern = prefix + "(?:" + this.last + ")*"
        this.last = "(?:" + this.last + ")*"
        return this
    }

    public func startOfLine(): Regex {
        this.pattern = this.pattern + "^"
        this.last = "^"
        return this
    }

    public func endOfLine(): Regex {
        this.pattern = this.pattern + "$"
        this.last = "$"
        return this
    }

    public func any(): Regex {
        this.pattern = this.pattern + "."
        this.last = "."
        return this
    }

    public func alternate(Regex value): Regex {
        var left = this.pattern
        var right = value.source()
        this.pattern = "(?:" + left + ")|(?:" + right + ")"
        this.last = this.pattern
        return this
    }

    public func optional(): Regex {
        if (this.last == "") { throw new Exception("Regex.optional requires a preceding builder element") }
        var prefix = String.substring(this.pattern, 0, String.length(this.pattern) - String.length(this.last))
        this.pattern = prefix + "(?:" + this.last + ")?"
        this.last = "(?:" + this.last + ")?"
        return this
    }

    public func oneOrMoreLazy(): Regex {
        if (this.last == "") { throw new Exception("Regex.oneOrMoreLazy requires a preceding builder element") }
        var prefix = String.substring(this.pattern, 0, String.length(this.pattern) - String.length(this.last))
        this.pattern = prefix + "(?:" + this.last + ")+?"
        this.last = "(?:" + this.last + ")+?"
        return this
    }

    public func zeroOrMoreLazy(): Regex {
        if (this.last == "") { throw new Exception("Regex.zeroOrMoreLazy requires a preceding builder element") }
        var prefix = String.substring(this.pattern, 0, String.length(this.pattern) - String.length(this.last))
        this.pattern = prefix + "(?:" + this.last + ")*?"
        this.last = "(?:" + this.last + ")*?"
        return this
    }

    public func optionalLiteral(string value): Regex {
        return this.then(value).optional()
    }

    public func backreference(int group): Regex {
        if (group < 1 || group > 9) { throw new Exception("Regex.backreference group must be between 1 and 9") }
        var ref = "\\1"
        if (group == 2) { ref = "\\2" }
        if (group == 3) { ref = "\\3" }
        if (group == 4) { ref = "\\4" }
        if (group == 5) { ref = "\\5" }
        if (group == 6) { ref = "\\6" }
        if (group == 7) { ref = "\\7" }
        if (group == 8) { ref = "\\8" }
        if (group == 9) { ref = "\\9" }
        this.pattern = this.pattern + ref
        this.last = ref
        return this
    }

    public func backreference(string name): Regex {
        if (name == "") { throw new Exception("Regex.backreference name must not be empty") }
        var ref = "\\k<" + name + ">"
        this.pattern = this.pattern + ref
        this.last = ref
        return this
    }

    public func positiveLookahead(Regex value): Regex {
        this.pattern = this.pattern + "(?=" + value.source() + ")"
        this.last = "(?=" + value.source() + ")"
        return this
    }

    public func negativeLookahead(Regex value): Regex {
        this.pattern = this.pattern + "(?!" + value.source() + ")"
        this.last = "(?!" + value.source() + ")"
        return this
    }

    public func positiveLookbehind(Regex value): Regex {
        if (this.pattern != "") { throw new Exception("Regex.positiveLookbehind must be the first builder element") }
        this.pattern = "(?<=" + value.source() + ")"
        this.last = "(?<=" + value.source() + ")"
        return this
    }

    public func negativeLookbehind(Regex value): Regex {
        if (this.pattern != "") { throw new Exception("Regex.negativeLookbehind must be the first builder element") }
        this.pattern = "(?<!" + value.source() + ")"
        this.last = "(?<!" + value.source() + ")"
        return this
    }

    public func atomic(Regex value): Regex {
        var raw = value.source()
        this.pattern = this.pattern + "(?>" + raw + ")"
        this.last = "(?>" + raw + ")"
        return this
    }

    public func oneOrMorePossessive(): Regex {
        if (this.last == "") { throw new Exception("Regex.oneOrMorePossessive requires a preceding builder element") }
        var prefix = String.substring(this.pattern, 0, String.length(this.pattern) - String.length(this.last))
        this.pattern = prefix + "(?:" + this.last + ")++"
        this.last = "(?:" + this.last + ")++"
        return this
    }

    public func zeroOrMorePossessive(): Regex {
        if (this.last == "") { throw new Exception("Regex.zeroOrMorePossessive requires a preceding builder element") }
        var prefix = String.substring(this.pattern, 0, String.length(this.pattern) - String.length(this.last))
        this.pattern = prefix + "(?:" + this.last + ")*+"
        this.last = "(?:" + this.last + ")*+"
        return this
    }

    public func matches(string value): bool {
        return String.regexFullMatches(value, this.pattern)
    }

    public func findAll(string value): list<string> {
        return String.regexFindAll(value, this.pattern)
    }

    public func replace(string value, string replacement): string {
        return String.regexReplace(value, this.pattern, replacement)
    }

    public func find(string value): RegexMatch {
        return String.regexFind(value, this.pattern)
    }

    public func findMatches(string value): list<RegexMatch> {
        return String.regexFindMatches(value, this.pattern)
    }

    public func named(string name): Regex {
        if (this.last == "") { throw new Exception("Regex.named requires a preceding builder element") }
        var prefix = String.substring(this.pattern, 0, String.length(this.pattern) - String.length(this.last))
        this.pattern = prefix + "(?<" + name + ">" + this.last + ")"
        this.last = "(?<" + name + ">" + this.last + ")"
        return this
    }
}
)ZL";

constexpr std::string_view kBuiltinMathSource = R"ZL(
class Math {
    public static func clamp(double value, double lo, double hi): double {
        if (value != value || lo != lo || hi != hi) {
            return value
        }
        if (lo > hi) {
            throw new Exception("Math.clamp: minimum must not exceed maximum")
        }
        if (value < lo) { return lo }
        if (value > hi) { return hi }
        return value
    }

    public static func sign(double x): double {
        if (x != x) { return x }
        if (x > 0.0) { return 1.0 }
        if (x < 0.0) { return -1.0 }
        return 0.0
    }

    public static func lerp(double a, double b, double t): double {
        return a + (b - a) * t
    }

    public static func degrees(double radians): double {
        return radians * (180.0 / Math.PI)
    }

    public static func radians(double degrees): double {
        return degrees * (Math.PI / 180.0)
    }

    public static func gcd(int a, int b): int {
        if (a < 0) { a = -a }
        if (b < 0) { b = -b }
        while (b != 0) {
            var r = a % b
            a = b
            b = r
        }
        return a
    }

    public static func lcm(int a, int b): int {
        if (a == 0 || b == 0) { return 0 }
        return Math.abs((a / Math.gcd(a, b)) * b)
    }

    public static func hypot(double a, double b): double {
        return Math.sqrt(a * a + b * b)
    }
}
)ZL";

constexpr std::string_view kBuiltinTypeSource = R"ZL(
class Type {
    object __value

    public func name(): string { return Reflection.name(this) }
    public func fields(): list<Field> { return Reflection.fields(this) }
    public func methods(): list<Method> { return Reflection.methods(this) }
    public func constructors(): list<Constructor> { return Reflection.constructors(this) }
    public func base(): string { return Reflection.base(this) }
    public func interfaces(): list<string> { return Reflection.interfaces(this) }
    public func typeParameters(): list<string> { return Reflection.typeParameters(this) }
    public func field(string name): Field { return Reflection.field(this, name) }
    public func method(string name): Method { return Reflection.method(this, name) }
    public func constructor(int index): Constructor { return Reflection.constructor(this, index) }
    public func callable(): Function { return Reflection.function(this) }
    public func kind(): string { return Reflection.kind(this) }
    public func isData(): bool { return Reflection.isData(this) }
    public func isEnum(): bool { return Reflection.isEnum(this) }
    public func enumMembers(): list<string> { return Reflection.enumMembers(this) }
}
)ZL";

constexpr std::string_view kBuiltinFieldSource = R"ZL(
class Field {
    public func name(): string { return Reflection.fieldName(this) }
    public func type(): string { return Reflection.fieldType(this) }
    public func access(): string { return Reflection.fieldAccess(this) }
}
)ZL";

constexpr std::string_view kBuiltinMethodSource = R"ZL(
class Method {
    public func name(): string { return Reflection.methodName(this) }
    public func returnType(): string { return Reflection.methodReturnType(this) }
    public func access(): string { return Reflection.methodAccess(this) }
    public func isStatic(): bool { return Reflection.methodIsStatic(this) }
    public func isAsync(): bool { return Reflection.methodIsAsync(this) }
    public func parameters(): list<string> { return Reflection.methodParameters(this) }
    public func invoke(object target, list<unknown> args): object { return Reflection.methodInvoke(this, target, args) }
}
)ZL";

constexpr std::string_view kBuiltinConstructorSource = R"ZL(
class Constructor {
    public func parameters(): list<string> { return Reflection.constructorParameters(this) }
    public func invoke(list<unknown> args): object { return Reflection.constructorInvoke(this, args) }
}
)ZL";

constexpr std::string_view kBuiltinFunctionSource = R"ZL(
class Function {
    public func name(): string { return Reflection.functionName(this) }
    public func parameters(): list<string> { return Reflection.functionParameters(this) }
    public func returnType(): string { return Reflection.functionReturnType(this) }
    public func isAsync(): bool { return Reflection.functionIsAsync(this) }
    public func isNative(): bool { return Reflection.functionIsNative(this) }
    public func invoke(list<unknown> args): object { return Reflection.functionInvoke(this, args) }
}
)ZL";

const std::vector<std::string_view> kBuiltinSources = {
    kBuiltinOptionSource,
    kBuiltinResultSource,
    kBuiltinSharedSource,
    kBuiltinExceptionSource,
    kBuiltinCancellationExceptionSource,
    kBuiltinNativeErrorSource,
    kBuiltinRegexErrorSource,
    kBuiltinReflectionErrorSource,
    kBuiltinInvalidArgumentsSource,
    kBuiltinAccessViolationSource,
    kBuiltinMathSource,
    kBuiltinRegexSource,
    kBuiltinRegexMatchSource,
    kBuiltinListSource,
    kBuiltinMapSource,
    kBuiltinSetSource,
    kBuiltinTypeSource,
    kBuiltinFieldSource,
    kBuiltinMethodSource,
    kBuiltinConstructorSource,
    kBuiltinFunctionSource,
};

} // namespace

const std::vector<std::string_view>& builtinLibrarySources() {
    return kBuiltinSources;
}

} // namespace zl
