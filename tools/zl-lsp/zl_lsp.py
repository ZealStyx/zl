#!/usr/bin/env python3
"""A small zero-dependency language-server/tooling frontend for ZL.

The server intentionally starts conservative: it does not try to replace the C++
compiler's parser/type checker. Instead it provides fast editor feedback that is
safe to run on every keystroke: lexical diagnostics, delimiter matching, ZL's
file/class-name rule, and document symbols for navigation.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple
from urllib.parse import unquote, urlparse


KEYWORDS = {
    "var",
    "let",
    "data",
    "enum",
    "import",
    "func",
    "async",
    "await",
    "operator",
    "return",
    "class",
    "static",
    "public",
    "private",
    "protected",
    "new",
    "this",
    "extends",
    "with",
    "implements",
    "interface",
    "super",
    "void",
    "array",
    "list",
    "set",
    "map",
    "gc",
    "owned",
    "borrow",
    "shared",
    "move",
    "if",
    "else",
    "elif",
    "for",
    "in",
    "step",
    "repeat",
    "while",
    "break",
    "continue",
    "try",
    "catch",
    "throw",
    "finally",
    "match",
    "true",
    "false",
    "null",
    "log",
}

MULTI_CHAR_OPERATORS = (">>>", "^^", "..", "=>", "==", "!=", "<=", ">=", "&&", "||", "<<", ">>")
SINGLE_CHAR_TOKENS = set("{}()[];:,.@+-*/%&|^~=!<>")
OPEN_TO_CLOSE = {"{": "}", "(": ")", "[": "]"}
CLOSE_TO_OPEN = {v: k for k, v in OPEN_TO_CLOSE.items()}
DECLARATION_KEYWORDS = {"class", "interface", "data", "enum"}
MODIFIERS = {"public", "private", "protected", "static", "async", "gc", "owned", "borrow", "shared"}

# LSP SymbolKind constants.
SYMBOL_KIND_FILE = 1
SYMBOL_KIND_CLASS = 5
SYMBOL_KIND_METHOD = 6
SYMBOL_KIND_FIELD = 8
SYMBOL_KIND_ENUM = 10
SYMBOL_KIND_INTERFACE = 11
SYMBOL_KIND_FUNCTION = 12
SYMBOL_KIND_STRUCT = 23


@dataclass(frozen=True)
class Position:
    line: int
    character: int

    def lsp(self) -> Dict[str, int]:
        return {"line": self.line - 1, "character": self.character - 1}


@dataclass(frozen=True)
class Token:
    kind: str
    lexeme: str
    start: Position
    end: Position
    start_offset: int
    end_offset: int


@dataclass(frozen=True)
class Diagnostic:
    message: str
    start: Position
    end: Position
    severity: int = 1  # LSP: Error
    source: str = "zl-lsp"

    def lsp(self) -> Dict[str, Any]:
        end = self.end
        if end.line < self.start.line or (end.line == self.start.line and end.character <= self.start.character):
            end = Position(self.start.line, self.start.character + 1)
        return {
            "range": {"start": self.start.lsp(), "end": end.lsp()},
            "severity": self.severity,
            "source": self.source,
            "message": self.message,
        }

    def cli(self, path: str) -> str:
        label = "error" if self.severity == 1 else "warning"
        return f"{path}:{self.start.line}:{self.start.character}: {label}: {self.message}"


@dataclass
class DocumentSymbol:
    name: str
    kind: int
    start: Position
    end: Position
    selection_start: Position
    selection_end: Position
    children: List["DocumentSymbol"]

    def lsp(self) -> Dict[str, Any]:
        if self.end.line < self.start.line or (self.end.line == self.start.line and self.end.character <= self.start.character):
            end = Position(self.start.line, self.start.character + max(1, len(self.name)))
        else:
            end = self.end
        return {
            "name": self.name,
            "kind": self.kind,
            "range": {"start": self.start.lsp(), "end": end.lsp()},
            "selectionRange": {"start": self.selection_start.lsp(), "end": self.selection_end.lsp()},
            "children": [child.lsp() for child in self.children],
        }


class ZlScanner:
    def __init__(self, source: str) -> None:
        self.source = source
        self.pos = 0
        self.line = 1
        self.col = 1
        self.tokens: List[Token] = []
        self.diagnostics: List[Diagnostic] = []

    def at_end(self) -> bool:
        return self.pos >= len(self.source)

    def peek(self, distance: int = 0) -> str:
        idx = self.pos + distance
        if idx >= len(self.source):
            return "\0"
        return self.source[idx]

    def advance(self) -> str:
        ch = self.source[self.pos]
        self.pos += 1
        if ch == "\n":
            self.line += 1
            self.col = 1
        else:
            self.col += 1
        return ch

    def position(self) -> Position:
        return Position(self.line, self.col)

    def add_token(self, kind: str, lexeme: str, start: Position, start_offset: int) -> None:
        self.tokens.append(Token(kind, lexeme, start, self.position(), start_offset, self.pos))

    def scan(self) -> Tuple[List[Token], List[Diagnostic]]:
        while not self.at_end():
            ch = self.peek()
            if ch in " \t\r\n":
                self.advance()
                continue
            if ch == "/" and self.peek(1) == "/":
                self.scan_line_comment()
                continue
            if ch == '"':
                self.scan_string()
                continue
            if ch == "#":
                self.scan_regex_literal()
                continue
            if ch.isalpha() or ch == "_":
                self.scan_identifier()
                continue
            if ch.isdigit() or (ch == "." and self.peek(1).isdigit()):
                self.scan_number()
                continue
            if self.scan_operator_or_punctuation():
                continue

            start = self.position()
            start_offset = self.pos
            bad = self.advance()
            self.tokens.append(Token("unknown", bad, start, self.position(), start_offset, self.pos))
            self.diagnostics.append(Diagnostic(f"unexpected character {bad!r}", start, self.position()))
        eof = self.position()
        self.tokens.append(Token("eof", "", eof, eof, self.pos, self.pos))
        return self.tokens, self.diagnostics

    def scan_line_comment(self) -> None:
        while not self.at_end() and self.peek() != "\n":
            self.advance()

    def scan_identifier(self) -> None:
        start = self.position()
        start_offset = self.pos
        text = []
        while not self.at_end() and (self.peek().isalnum() or self.peek() == "_"):
            text.append(self.advance())
        lexeme = "".join(text)
        self.add_token("keyword" if lexeme in KEYWORDS else "identifier", lexeme, start, start_offset)

    def scan_number(self) -> None:
        start = self.position()
        start_offset = self.pos
        text = []
        if self.peek() == ".":
            text.append(self.advance())
        while not self.at_end() and self.peek().isdigit():
            text.append(self.advance())
        if self.peek() == "." and self.peek(1).isdigit():
            text.append(self.advance())
            while not self.at_end() and self.peek().isdigit():
                text.append(self.advance())
        self.add_token("number", "".join(text), start, start_offset)

    def scan_string(self) -> None:
        start = self.position()
        start_offset = self.pos
        text = [self.advance()]
        while not self.at_end():
            ch = self.advance()
            text.append(ch)
            if ch == "\\" and not self.at_end():
                text.append(self.advance())
                continue
            if ch == '"':
                self.add_token("string", "".join(text), start, start_offset)
                return
        self.tokens.append(Token("string", "".join(text), start, self.position(), start_offset, self.pos))
        self.diagnostics.append(Diagnostic("unterminated string literal", start, self.position()))

    def scan_regex_literal(self) -> None:
        start = self.position()
        start_offset = self.pos
        text = [self.advance()]
        while not self.at_end():
            ch = self.advance()
            text.append(ch)
            if ch == "\\" and not self.at_end():
                text.append(self.advance())
                continue
            if ch == "#":
                self.add_token("regex", "".join(text), start, start_offset)
                return
        self.tokens.append(Token("regex", "".join(text), start, self.position(), start_offset, self.pos))
        self.diagnostics.append(Diagnostic("unterminated regex literal", start, self.position()))

    def scan_operator_or_punctuation(self) -> bool:
        start = self.position()
        start_offset = self.pos
        for op in MULTI_CHAR_OPERATORS:
            if self.source.startswith(op, self.pos):
                for _ in op:
                    self.advance()
                self.add_token("operator", op, start, start_offset)
                return True
        ch = self.peek()
        if ch in SINGLE_CHAR_TOKENS:
            self.advance()
            kind = "punctuation" if ch in "{}()[];:,.@" else "operator"
            self.add_token(kind, ch, start, start_offset)
            return True
        return False


def token_text(tokens: Sequence[Token], index: int) -> str:
    if 0 <= index < len(tokens):
        return tokens[index].lexeme
    return ""


def matching_brace_index(tokens: Sequence[Token], open_index: int) -> Optional[int]:
    if open_index >= len(tokens) or tokens[open_index].lexeme not in OPEN_TO_CLOSE:
        return None
    want = OPEN_TO_CLOSE[tokens[open_index].lexeme]
    depth = 0
    for i in range(open_index, len(tokens)):
        lexeme = tokens[i].lexeme
        if lexeme == tokens[open_index].lexeme:
            depth += 1
        elif lexeme == want:
            depth -= 1
            if depth == 0:
                return i
    return None


def declaration_name_after(tokens: Sequence[Token], index: int) -> Optional[int]:
    j = index + 1
    while token_text(tokens, j) in ("<", ">"):
        j += 1
    if j < len(tokens) and tokens[j].kind == "identifier":
        return j
    return None


def find_body_open(tokens: Sequence[Token], start: int, stop_at: Optional[int] = None) -> Optional[int]:
    limit = stop_at if stop_at is not None else len(tokens)
    paren = bracket = angle = 0
    for i in range(start, limit):
        lexeme = tokens[i].lexeme
        if lexeme == "(":
            paren += 1
        elif lexeme == ")":
            paren = max(0, paren - 1)
        elif lexeme == "[":
            bracket += 1
        elif lexeme == "]":
            bracket = max(0, bracket - 1)
        elif lexeme == "<":
            angle += 1
        elif lexeme == ">":
            angle = max(0, angle - 1)
        elif lexeme == "{" and paren == 0 and bracket == 0 and angle == 0:
            return i
    return None


def next_top_level_token(tokens: Sequence[Token], start: int, end: int) -> Iterable[int]:
    depth = 0
    i = start
    while i < end:
        lexeme = tokens[i].lexeme
        if depth == 0:
            yield i
        if lexeme in OPEN_TO_CLOSE:
            depth += 1
        elif lexeme in CLOSE_TO_OPEN:
            depth = max(0, depth - 1)
        i += 1


def function_name_at(tokens: Sequence[Token], index: int) -> Optional[int]:
    if token_text(tokens, index) == "operator":
        return index + 1 if index + 1 < len(tokens) else None
    if token_text(tokens, index) != "func":
        return None
    j = index + 1
    return j if j < len(tokens) and tokens[j].kind == "identifier" else None


def parse_member_symbols(tokens: Sequence[Token], body_start: int, body_end: int) -> List[DocumentSymbol]:
    children: List[DocumentSymbol] = []
    i = body_start + 1
    member_depth = 0
    while i < body_end:
        lexeme = tokens[i].lexeme
        if lexeme in OPEN_TO_CLOSE:
            member_depth += 1
            i += 1
            continue
        if lexeme in CLOSE_TO_OPEN:
            member_depth = max(0, member_depth - 1)
            i += 1
            continue
        if member_depth != 0:
            i += 1
            continue

        j = i
        while token_text(tokens, j) in MODIFIERS:
            j += 1
        if token_text(tokens, j) == "async" and token_text(tokens, j + 1) == "func":
            j += 1
        if token_text(tokens, j) in {"func", "operator"}:
            name_index = function_name_at(tokens, j)
            if name_index is not None and name_index < body_end:
                open_index = find_body_open(tokens, name_index + 1, body_end)
                close_index = matching_brace_index(tokens, open_index) if open_index is not None else None
                end_pos = tokens[close_index].end if close_index is not None else tokens[name_index].end
                name = tokens[name_index].lexeme
                if token_text(tokens, j) == "operator":
                    name = "operator " + name
                children.append(
                    DocumentSymbol(
                        name=name,
                        kind=SYMBOL_KIND_METHOD,
                        start=tokens[i].start,
                        end=end_pos,
                        selection_start=tokens[name_index].start,
                        selection_end=tokens[name_index].end,
                        children=[],
                    )
                )
                i = (close_index + 1) if close_index is not None else name_index + 1
                continue
        if token_text(tokens, j) in {"var", "let"} and j + 1 < body_end and tokens[j + 1].kind == "identifier":
            name_index = j + 1
            children.append(
                DocumentSymbol(
                    name=tokens[name_index].lexeme,
                    kind=SYMBOL_KIND_FIELD,
                    start=tokens[i].start,
                    end=tokens[name_index].end,
                    selection_start=tokens[name_index].start,
                    selection_end=tokens[name_index].end,
                    children=[],
                )
            )
            i = name_index + 1
            continue
        # Data declarations commonly have bare `name: Type` fields.
        if i + 1 < body_end and tokens[i].kind == "identifier" and token_text(tokens, i + 1) == ":":
            children.append(
                DocumentSymbol(
                    name=tokens[i].lexeme,
                    kind=SYMBOL_KIND_FIELD,
                    start=tokens[i].start,
                    end=tokens[i].end,
                    selection_start=tokens[i].start,
                    selection_end=tokens[i].end,
                    children=[],
                )
            )
        i += 1
    return children


def document_symbols(tokens: Sequence[Token]) -> List[DocumentSymbol]:
    result: List[DocumentSymbol] = []
    i = 0
    depth = 0
    while i < len(tokens):
        lexeme = tokens[i].lexeme
        if lexeme in OPEN_TO_CLOSE:
            depth += 1
            i += 1
            continue
        if lexeme in CLOSE_TO_OPEN:
            depth = max(0, depth - 1)
            i += 1
            continue
        if depth == 0 and lexeme in DECLARATION_KEYWORDS:
            name_index = declaration_name_after(tokens, i)
            if name_index is None:
                i += 1
                continue
            body_open = find_body_open(tokens, name_index + 1)
            body_close = matching_brace_index(tokens, body_open) if body_open is not None else None
            end = tokens[body_close].end if body_close is not None else tokens[name_index].end
            kind = {
                "class": SYMBOL_KIND_CLASS,
                "interface": SYMBOL_KIND_INTERFACE,
                "data": SYMBOL_KIND_STRUCT,
                "enum": SYMBOL_KIND_ENUM,
            }[lexeme]
            children = parse_member_symbols(tokens, body_open, body_close) if body_open is not None and body_close is not None else []
            result.append(
                DocumentSymbol(
                    name=tokens[name_index].lexeme,
                    kind=kind,
                    start=tokens[i].start,
                    end=end,
                    selection_start=tokens[name_index].start,
                    selection_end=tokens[name_index].end,
                    children=children,
                )
            )
            i = (body_close + 1) if body_close is not None else name_index + 1
            continue
        i += 1
    return result


def structural_diagnostics(tokens: Sequence[Token], path: Optional[str]) -> List[Diagnostic]:
    diagnostics: List[Diagnostic] = []
    stack: List[Token] = []
    seen_declaration = False
    classes: List[str] = []

    for index, token in enumerate(tokens):
        lexeme = token.lexeme
        if lexeme == "":
            continue
        if lexeme == "import":
            if seen_declaration:
                diagnostics.append(Diagnostic("imports must appear before declarations", token.start, token.end))
        elif lexeme in DECLARATION_KEYWORDS:
            seen_declaration = True
        if lexeme == "class":
            name_index = declaration_name_after(tokens, index)
            if name_index is not None:
                classes.append(tokens[name_index].lexeme)
        if lexeme in OPEN_TO_CLOSE:
            stack.append(token)
        elif lexeme in CLOSE_TO_OPEN:
            if not stack:
                diagnostics.append(Diagnostic(f"unmatched closing '{lexeme}'", token.start, token.end))
            else:
                opening = stack.pop()
                expected = OPEN_TO_CLOSE[opening.lexeme]
                if lexeme != expected:
                    diagnostics.append(
                        Diagnostic(
                            f"mismatched closing '{lexeme}', expected '{expected}' for '{opening.lexeme}' opened at {opening.start.line}:{opening.start.character}",
                            token.start,
                            token.end,
                        )
                    )

    for opening in reversed(stack):
        diagnostics.append(Diagnostic(f"unclosed '{opening.lexeme}'", opening.start, opening.end))

    if path and path.endswith(".zl"):
        stem = Path(path).stem
        if stem and stem not in classes:
            diagnostics.append(
                Diagnostic(
                    f"file '{Path(path).name}' must declare class '{stem}'",
                    Position(1, 1),
                    Position(1, 1),
                )
            )
    return diagnostics


def analyze(source: str, path: Optional[str] = None) -> Tuple[List[Token], List[Diagnostic], List[DocumentSymbol]]:
    scanner = ZlScanner(source)
    tokens, diagnostics = scanner.scan()
    diagnostics.extend(structural_diagnostics(tokens, path))
    return tokens, diagnostics, document_symbols(tokens)


def path_from_uri(uri: str) -> str:
    parsed = urlparse(uri)
    if parsed.scheme != "file":
        return uri
    return unquote(parsed.path)


def read_text(path: str) -> str:
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


class JsonRpcConnection:
    def __init__(self) -> None:
        self.reader = sys.stdin.buffer
        self.writer = sys.stdout.buffer

    def read_message(self) -> Optional[Dict[str, Any]]:
        headers: Dict[str, str] = {}
        while True:
            line = self.reader.readline()
            if line == b"":
                return None
            line = line.decode("ascii", errors="replace").strip()
            if line == "":
                break
            key, _, value = line.partition(":")
            headers[key.lower()] = value.strip()
        length = int(headers.get("content-length", "0"))
        if length <= 0:
            return None
        payload = self.reader.read(length)
        return json.loads(payload.decode("utf-8"))

    def send(self, message: Dict[str, Any]) -> None:
        payload = json.dumps(message, separators=(",", ":")).encode("utf-8")
        self.writer.write(f"Content-Length: {len(payload)}\r\n\r\n".encode("ascii"))
        self.writer.write(payload)
        self.writer.flush()

    def respond(self, request_id: Any, result: Any) -> None:
        self.send({"jsonrpc": "2.0", "id": request_id, "result": result})

    def error(self, request_id: Any, code: int, message: str) -> None:
        self.send({"jsonrpc": "2.0", "id": request_id, "error": {"code": code, "message": message}})

    def notify(self, method: str, params: Dict[str, Any]) -> None:
        self.send({"jsonrpc": "2.0", "method": method, "params": params})


class ZlLanguageServer:
    def __init__(self) -> None:
        self.rpc = JsonRpcConnection()
        self.documents: Dict[str, str] = {}
        self.shutdown_requested = False

    def run(self) -> int:
        while True:
            message = self.rpc.read_message()
            if message is None:
                return 0
            method = message.get("method")
            request_id = message.get("id")
            try:
                if method == "initialize":
                    self.handle_initialize(request_id)
                elif method == "shutdown":
                    self.shutdown_requested = True
                    self.rpc.respond(request_id, None)
                elif method == "exit":
                    return 0 if self.shutdown_requested else 1
                elif method == "textDocument/didOpen":
                    self.handle_did_open(message.get("params", {}))
                elif method == "textDocument/didChange":
                    self.handle_did_change(message.get("params", {}))
                elif method == "textDocument/didSave":
                    self.handle_did_save(message.get("params", {}))
                elif method == "textDocument/didClose":
                    self.handle_did_close(message.get("params", {}))
                elif method == "textDocument/documentSymbol":
                    self.handle_document_symbol(request_id, message.get("params", {}))
                elif request_id is not None:
                    self.rpc.error(request_id, -32601, f"method not found: {method}")
            except Exception as exc:  # Keep the editor session alive after bad input.
                if request_id is not None:
                    self.rpc.error(request_id, -32603, str(exc))
        return 0

    def handle_initialize(self, request_id: Any) -> None:
        self.rpc.respond(
            request_id,
            {
                "serverInfo": {"name": "zl-lsp", "version": "0.1.0"},
                "capabilities": {
                    "textDocumentSync": {"openClose": True, "change": 1, "save": True},
                    "documentSymbolProvider": True,
                },
            },
        )

    def document_text(self, uri: str) -> Tuple[str, str]:
        path = path_from_uri(uri)
        if uri in self.documents:
            return self.documents[uri], path
        return read_text(path), path

    def publish_diagnostics(self, uri: str) -> None:
        source, path = self.document_text(uri)
        _, diagnostics, _ = analyze(source, path)
        self.rpc.notify("textDocument/publishDiagnostics", {"uri": uri, "diagnostics": [d.lsp() for d in diagnostics]})

    def handle_did_open(self, params: Dict[str, Any]) -> None:
        doc = params.get("textDocument", {})
        uri = doc.get("uri")
        if not uri:
            return
        self.documents[uri] = doc.get("text", "")
        self.publish_diagnostics(uri)

    def handle_did_change(self, params: Dict[str, Any]) -> None:
        doc = params.get("textDocument", {})
        uri = doc.get("uri")
        changes = params.get("contentChanges", [])
        if not uri or not changes:
            return
        # The server advertises full-document sync for now.
        self.documents[uri] = changes[-1].get("text", self.documents.get(uri, ""))
        self.publish_diagnostics(uri)

    def handle_did_save(self, params: Dict[str, Any]) -> None:
        doc = params.get("textDocument", {})
        uri = doc.get("uri")
        if uri:
            self.publish_diagnostics(uri)

    def handle_did_close(self, params: Dict[str, Any]) -> None:
        doc = params.get("textDocument", {})
        uri = doc.get("uri")
        if uri:
            self.documents.pop(uri, None)
            self.rpc.notify("textDocument/publishDiagnostics", {"uri": uri, "diagnostics": []})

    def handle_document_symbol(self, request_id: Any, params: Dict[str, Any]) -> None:
        uri = params.get("textDocument", {}).get("uri")
        if not uri:
            self.rpc.respond(request_id, [])
            return
        source, path = self.document_text(uri)
        _, _, symbols = analyze(source, path)
        self.rpc.respond(request_id, [symbol.lsp() for symbol in symbols])


def check_files(paths: Sequence[str], json_output: bool) -> int:
    all_results: Dict[str, List[Dict[str, Any]]] = {}
    had_error = False
    for path in paths:
        try:
            source = read_text(path)
            _, diagnostics, _ = analyze(source, path)
        except OSError as exc:
            diagnostics = [Diagnostic(str(exc), Position(1, 1), Position(1, 1))]
        if diagnostics:
            had_error = True
        if json_output:
            all_results[path] = [diag.lsp() for diag in diagnostics]
        else:
            for diag in diagnostics:
                print(diag.cli(path), file=sys.stderr)
    if json_output:
        print(json.dumps(all_results, indent=2))
    return 1 if had_error else 0


def print_symbols(path: str, json_output: bool) -> int:
    source = read_text(path)
    _, _, symbols = analyze(source, path)
    if json_output:
        print(json.dumps([symbol.lsp() for symbol in symbols], indent=2))
    else:
        def walk(items: Sequence[DocumentSymbol], indent: int = 0) -> None:
            for item in items:
                print(f"{'  ' * indent}{item.name} ({item.start.line}:{item.start.character})")
                walk(item.children, indent + 1)
        walk(symbols)
    return 0


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="ZL editor tooling and language server")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--stdio", action="store_true", help="run as a Language Server Protocol server over stdio")
    mode.add_argument("--check", nargs="+", metavar="FILE", help="run fast editor diagnostics for one or more .zl files")
    mode.add_argument("--symbols", metavar="FILE", help="print document symbols for a .zl file")
    parser.add_argument("--json", action="store_true", help="emit JSON for --check or --symbols")
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_arg_parser().parse_args(argv)
    if args.check:
        return check_files(args.check, args.json)
    if args.symbols:
        return print_symbols(args.symbols, args.json)
    return ZlLanguageServer().run()


if __name__ == "__main__":
    raise SystemExit(main())
