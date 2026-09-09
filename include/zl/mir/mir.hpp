#pragma once

// MIR - ZL's mid-level intermediate representation.
//
// This header is the whole layer in one include. It is ordered so the
// dependency direction is readable top to bottom: locations, then types, then
// values, then the structures that hold them, then the tools that build, walk,
// check and print them.
//
// Nothing here depends on the AST, the type checker or the VM. `lowering.hpp`
// is the single boundary that does, and it is the only header in this layer that
// a backend should NOT need.

#include "zl/mir/source_location.hpp" // where an instruction came from
#include "zl/mir/type.hpp"            // TypeKind, TypeArena, FunctionSignature
#include "zl/mir/value.hpp"           // Constant, Operand, Slot, Parameter
#include "zl/mir/instruction.hpp"     // Opcode, OpcodeShape, Instruction, Terminator
#include "zl/mir/function.hpp"        // BasicBlock, Function, ClassLayout, Module
#include "zl/mir/builder.hpp"         // ModuleBuilder, FunctionBuilder
#include "zl/mir/analysis.hpp"        // ControlFlowGraph, reachability, dominance
#include "zl/mir/verifier.hpp"        // VerifierReport, verifyModule
#include "zl/mir/printer.hpp"         // printModule, the textual MIR form
#include "zl/mir/lowering.hpp"        // lowerProgram - AST -> MIR, the boundary
