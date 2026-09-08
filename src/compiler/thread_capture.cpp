#include "zl/compiler/thread_capture.hpp"

#include "zl/compiler/semantic_types.hpp"
#include "zl/compiler/type_checker.hpp" // SymbolTable / SymbolTable::VarInfo

namespace zl {

namespace {

// Names a match pattern binds (variable patterns and `Type name`), including
// anything nested inside a data/list/map pattern. These are locals in scope for
// the arm's guard and result, so references to them are NOT outer captures.
void collectPatternBindings(const MatchExpr::DataFieldPattern& f, std::unordered_set<std::string>& out);
void collectPatternBindings(const MatchExpr::MapEntryPattern& m, std::unordered_set<std::string>& out);
void collectPatternBindings(const MatchExpr::Pattern& p, std::unordered_set<std::string>& out) {
    if (!p.bindingName.empty()) out.insert(p.bindingName);
    for (const auto& f : p.fields) collectPatternBindings(f, out);
    for (const auto& e : p.elements)
        if (e) collectPatternBindings(*e, out);
    for (const auto& m : p.mapEntries) collectPatternBindings(m, out);
}
void collectPatternBindings(const MatchExpr::DataFieldPattern& f, std::unordered_set<std::string>& out) {
    if (f.pattern) collectPatternBindings(*f.pattern, out);
}
void collectPatternBindings(const MatchExpr::MapEntryPattern& m, std::unordered_set<std::string>& out) {
    if (m.key) collectPatternBindings(*m.key, out);
    if (m.value) collectPatternBindings(*m.value, out);
}

// Recursive free-variable walk. `params`/`bindings` is the set of names bound in
// the current scope (lambda parameters, then pattern bindings); references to
// anything else are recorded as outer names. The switch is intentionally
// exhaustive (no `default`) so -Wswitch flags any node kind not yet visited.
void collectRefs(const AstNode* node, std::unordered_set<std::string>& refs, bool& usesThis,
                 const std::unordered_set<std::string>& bound) {
    if (!node) return;
    switch (node->kind) {
        case NodeKind::Identifier: {
            const auto* n = static_cast<const Identifier*>(node);
            if (!bound.count(n->name)) refs.insert(n->name);
            break;
        }
        case NodeKind::ThisExpr:
            usesThis = true;
            refs.insert("this");
            break;
        case NodeKind::UnaryExpr: { auto* n = static_cast<const UnaryExpr*>(node); collectRefs(n->operand.get(), refs, usesThis, bound); break; }
        case NodeKind::AwaitExpr: { auto* n = static_cast<const AwaitExpr*>(node); collectRefs(n->operand.get(), refs, usesThis, bound); break; }
        case NodeKind::BinaryExpr: { auto* n = static_cast<const BinaryExpr*>(node); collectRefs(n->left.get(), refs, usesThis, bound); collectRefs(n->right.get(), refs, usesThis, bound); break; }
        case NodeKind::CallExpr: { auto* n = static_cast<const CallExpr*>(node);
            // The callee is a NAME, not a child node, so walking only the
            // arguments misses it. `f(g(x))` references both f and g, and a
            // lambda that calls a captured func value must capture it - only
            // names that actually resolve to a local are captured at runtime,
            // so recording a class or method name here is harmless.
            if (n->namespaceName.empty() && !n->calleeName.empty() && !bound.count(n->calleeName))
                refs.insert(n->calleeName);
            for (auto& a : n->arguments) collectRefs(a.get(), refs, usesThis, bound);
            break; }
        case NodeKind::AssignExpr: { auto* n = static_cast<const AssignExpr*>(node); if (!bound.count(n->name)) refs.insert(n->name); collectRefs(n->value.get(), refs, usesThis, bound); break; }
        case NodeKind::MoveExpr: { auto* n = static_cast<const MoveExpr*>(node); if (!bound.count(n->name)) refs.insert(n->name); break; }
        case NodeKind::FieldAccessExpr: { auto* n = static_cast<const FieldAccessExpr*>(node); collectRefs(n->object.get(), refs, usesThis, bound); break; }
        case NodeKind::IndexAccessExpr: { auto* n = static_cast<const IndexAccessExpr*>(node); collectRefs(n->object.get(), refs, usesThis, bound); collectRefs(n->index.get(), refs, usesThis, bound); break; }
        case NodeKind::FieldAssignExpr: { auto* n = static_cast<const FieldAssignExpr*>(node); collectRefs(n->object.get(), refs, usesThis, bound); collectRefs(n->value.get(), refs, usesThis, bound); break; }
        case NodeKind::MethodCallExpr: { auto* n = static_cast<const MethodCallExpr*>(node); collectRefs(n->object.get(), refs, usesThis, bound); for (auto& a : n->arguments) collectRefs(a.get(), refs, usesThis, bound); break; }
        case NodeKind::NewExpr: { auto* n = static_cast<const NewExpr*>(node); for (auto& a : n->arguments) collectRefs(a.get(), refs, usesThis, bound); break; }
        case NodeKind::CollectionLiteral: { auto* n = static_cast<const CollectionLiteral*>(node); for (auto& a : n->elements) collectRefs(a.get(), refs, usesThis, bound); for (auto& e : n->entries) { collectRefs(e.first.get(), refs, usesThis, bound); collectRefs(e.second.get(), refs, usesThis, bound); } break; }
        case NodeKind::BlockStmt: { auto* n = static_cast<const BlockStmt*>(node); for (auto& a : n->statements) collectRefs(a.get(), refs, usesThis, bound); break; }
        case NodeKind::VarDecl: { auto* n = static_cast<const VarDecl*>(node); collectRefs(n->initializer.get(), refs, usesThis, bound); break; }
        case NodeKind::LogStmt: { auto* n = static_cast<const LogStmt*>(node); collectRefs(n->argument.get(), refs, usesThis, bound); break; }
        case NodeKind::LogExpr: { auto* n = static_cast<const LogExpr*>(node); collectRefs(n->argument.get(), refs, usesThis, bound); break; }
        case NodeKind::IfStmt: { auto* n = static_cast<const IfStmt*>(node); for (auto& b : n->branches) { collectRefs(b.condition.get(), refs, usesThis, bound); collectRefs(b.body.get(), refs, usesThis, bound); } collectRefs(n->elseBody.get(), refs, usesThis, bound); break; }
        case NodeKind::ReturnStmt: { auto* n = static_cast<const ReturnStmt*>(node); collectRefs(n->value.get(), refs, usesThis, bound); break; }
        case NodeKind::ForStmt: { auto* n = static_cast<const ForStmt*>(node); collectRefs(n->start.get(), refs, usesThis, bound); collectRefs(n->end.get(), refs, usesThis, bound); collectRefs(n->step.get(), refs, usesThis, bound); collectRefs(n->body.get(), refs, usesThis, bound); break; }
        case NodeKind::WhileStmt: { auto* n = static_cast<const WhileStmt*>(node); collectRefs(n->condition.get(), refs, usesThis, bound); collectRefs(n->body.get(), refs, usesThis, bound); break; }
        case NodeKind::RepeatStmt: { auto* n = static_cast<const RepeatStmt*>(node); collectRefs(n->body.get(), refs, usesThis, bound); collectRefs(n->condition.get(), refs, usesThis, bound); break; }
        case NodeKind::TryStmt: { auto* n = static_cast<const TryStmt*>(node); collectRefs(n->tryBlock.get(), refs, usesThis, bound); for (auto& c : n->catches) collectRefs(c.block.get(), refs, usesThis, bound); collectRefs(n->finallyBlock.get(), refs, usesThis, bound); break; }
        case NodeKind::ThrowStmt: { auto* n = static_cast<const ThrowStmt*>(node); collectRefs(n->value.get(), refs, usesThis, bound); break; }
        case NodeKind::ExprStmt: { auto* n = static_cast<const ExprStmt*>(node); collectRefs(n->expression.get(), refs, usesThis, bound); break; }
        case NodeKind::SuperCallExpr: { auto* n = static_cast<const SuperCallExpr*>(node); for (auto& a : n->arguments) collectRefs(a.get(), refs, usesThis, bound); break; }
        case NodeKind::SuperMethodCallExpr: { auto* n = static_cast<const SuperMethodCallExpr*>(node); for (auto& a : n->arguments) collectRefs(a.get(), refs, usesThis, bound); break; }
        case NodeKind::DataLiteralExpr: { auto* n = static_cast<const DataLiteralExpr*>(node); for (auto& f : n->fields) collectRefs(f.second.get(), refs, usesThis, bound); break; }
        case NodeKind::DataUpdateExpr: { auto* n = static_cast<const DataUpdateExpr*>(node); collectRefs(n->base.get(), refs, usesThis, bound); for (auto& f : n->fields) collectRefs(f.second.get(), refs, usesThis, bound); break; }
        case NodeKind::LambdaExpr: {
            // A lambda nested in this body needs whatever IT references, so the
            // enclosing lambda has to carry those names across for it: at
            // runtime MakeClosure captures from the frame it executes in, and
            // that frame holds exactly this lambda's captures. Without this,
            // `func() { var inner = func() => n; inner() }` captures nothing for
            // `inner` to see and dies with "undefined variable 'n'". The nested
            // lambda's own parameters shadow outer names, so they must not be
            // recorded as captures.
            auto* n = static_cast<const LambdaExpr*>(node);
            std::unordered_set<std::string> nested(bound);
            for (const auto& p : n->params) nested.insert(p.name);
            collectRefs(n->hasExprBody ? n->exprBody.get() : n->blockBody.get(), refs, usesThis, nested);
            break;
        }
        case NodeKind::MatchExpr: {
            // The subject is evaluated in the OUTER scope; each arm's guard and
            // result additionally see the names that arm's pattern binds - those
            // are locals, not captures, so add them to the bound set for the arm.
            auto* n = static_cast<const MatchExpr*>(node);
            collectRefs(n->subject.get(), refs, usesThis, bound);
            for (const auto& arm : n->arms) {
                std::unordered_set<std::string> armBound(bound);
                if (!arm.bindingName.empty()) armBound.insert(arm.bindingName);
                for (const auto& f : arm.dataFields) collectPatternBindings(f, armBound);
                for (const auto& e : arm.listElements)
                    if (e) collectPatternBindings(*e, armBound);
                for (const auto& m : arm.mapEntries) collectPatternBindings(m, armBound);
                collectRefs(arm.guard.get(), refs, usesThis, armBound);
                collectRefs(arm.result.get(), refs, usesThis, armBound);
            }
            break;
        }
        // Leaves and declarations: no free-variable sub-expressions to walk.
        case NodeKind::Literal:
        case NodeKind::BreakStmt:
        case NodeKind::ContinueStmt:
        case NodeKind::Program:
        case NodeKind::ImportDecl:
        case NodeKind::ClassDecl:
        case NodeKind::InterfaceDecl:
        case NodeKind::DataDecl:
        case NodeKind::EnumDecl:
        case NodeKind::FunctionDecl:
            break;
    }
}

} // namespace

LambdaCaptureRefs collectLambdaCaptureRefs(const AstNode* body,
                                           const std::unordered_set<std::string>& params) {
    LambdaCaptureRefs out;
    collectRefs(body, out.names, out.usesThis, params);
    return out;
}

bool capturedValueCrossesThreadBoundary(const SymbolTable& symbols, const std::string& captured) {
    const auto info = symbols.lookupVar(captured);
    if (!info) return false;
    if (info->type != ZlType::OBJECT) return true;
    const auto& cls = info->className;
    // Keep this list in step with isThreadSafeClassName in src/vm/native.cpp.
    const bool threadSafe = cls == "Shared" || cls.rfind("Shared<", 0) == 0 ||
                            cls == "Atomic" || cls == "Mutex" || cls == "RwLock" ||
                            cls == "Semaphore" || cls == "Channel" || cls == "Condition";
    return !threadSafe;
}

} // namespace zl
