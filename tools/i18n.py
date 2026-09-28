#!/usr/bin/env python3
"""Translation catalogs (ADR-036). Spanish is the source language: every text in the code is its own key.

    python tools/i18n.py extract            # data/lang/messages.pot from the sources
    python tools/i18n.py update data/lang/en.po   # add new texts (untranslated), drop removed ones
    python tools/i18n.py check data/lang/en.po    # CI: every text translated, placeholders compatible

Texts are found where the code marks them: tr("..."), trf("..."), msg("..."), term("...") and GX_TEXT("..."),
with adjacent string literals concatenated as C++ does. The placeholder rules mirror compatiblePatterns() in
Engine/Text/Localization.cpp: a translation that breaks them is rejected by the game at load time.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE_DIRS = ["Apps", "Game", "Space", "Simulation", "Engine"]
SKIP_FILES = {os.path.join("Engine", "Text", "Localization.h"), os.path.join("Engine", "Text", "Localization.cpp")}
MARKER = re.compile(r'\b(tr|trf|msg|term|GX_TEXT)\(\s*"')
PRINTF = re.compile(r"%[-+ #0]*[\d*]*(?:\.[\d*]+)?(?:hh|h|ll|l|j|z|t|L)?([diouxXeEfFgGaAcsp%])")
ESCAPES = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\", "'": "'"}


def read_literal(text, i):
    """C++ string literal starting at text[i] == '"'. Returns (value, index after it)."""
    value = []
    j = i + 1
    while text[j] != '"':
        if text[j] == "\\":
            value.append(ESCAPES.get(text[j + 1], text[j + 1]))
            j += 2
        else:
            value.append(text[j])
            j += 1
    return "".join(value), j + 1


def extract():
    found = {}  # text -> set of "file:line"
    for directory in SOURCE_DIRS:
        for base, _, files in os.walk(os.path.join(ROOT, directory)):
            for name in sorted(files):
                if not name.endswith((".cpp", ".h")):
                    continue
                path = os.path.join(base, name)
                relative = os.path.relpath(path, ROOT)
                if relative in SKIP_FILES:
                    continue
                text = open(path, encoding="utf-8").read()
                for match in MARKER.finditer(text):
                    i = match.end() - 1
                    parts = []
                    while True:
                        value, i = read_literal(text, i)
                        parts.append(value)
                        j = i
                        while j < len(text) and text[j] in " \t\r\n":
                            j += 1
                        if j < len(text) and text[j] == '"':
                            i = j
                            continue
                        break
                    source = "".join(parts)
                    line = text.count("\n", 0, match.start()) + 1
                    found.setdefault(source, set()).add(f"{relative.replace(os.sep, '/')}:{line}")
    return found


def printf_conversions(text):
    return [m.group(0) for m in PRINTF.finditer(text) if m.group(1) != "%"]


def brace_placeholders(text):
    """Sorted (index, spec) list, or None if malformed or mixing numbering (as the game decides)."""
    out, automatic, manual, i = [], False, False, 0
    while i < len(text):
        c = text[i]
        if c == "}":
            if text[i + 1:i + 2] == "}":
                i += 2
                continue
            return None
        if c != "{":
            i += 1
            continue
        if text[i + 1:i + 2] == "{":
            i += 2
            continue
        close = text.find("}", i)
        if close < 0:
            return None
        inside = text[i + 1:close]
        ident, _, spec = inside.partition(":")
        spec = (":" + spec) if ":" in inside else ""
        if ident == "":
            automatic = True
            out.append([-1, spec])
        elif ident.isdigit():
            manual = True
            out.append([int(ident), spec])
        else:
            return None
        i = close + 1
    if automatic and manual:
        return None
    if automatic:
        for k, item in enumerate(out):
            item[0] = k
    return sorted(tuple(item) for item in out)


def compatible(source, translation):
    if printf_conversions(source) != printf_conversions(translation):
        return False
    a = brace_placeholders(source)
    b = brace_placeholders(translation)
    return a is None or (b is not None and a == b)


def po_quote(text):
    escaped = text.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t").replace("\r", "\\r")
    lines = escaped.split("\n")
    if len(lines) == 1:
        return f'"{lines[0]}"'
    parts = [line + "\\n" for line in lines[:-1]] + ([lines[-1]] if lines[-1] else [])
    return '""\n' + "\n".join(f'"{p}"' for p in parts)


def po_unquote(text):
    value, _ = read_literal(text, 0)
    return value


def read_po(path):
    """{msgid: (msgstr, fuzzy)} and the header msgstr."""
    entries, header = {}, ""
    msgid = msgstr = None
    field = None
    fuzzy = False

    def flush():
        nonlocal msgid, msgstr, fuzzy, header
        if msgid is not None:
            if msgid == "":
                header = msgstr or ""
            else:
                entries[msgid] = (msgstr or "", fuzzy)
        msgid = msgstr = None
        fuzzy = False

    for raw in open(path, encoding="utf-8-sig").read().split("\n"):
        line = raw.strip()
        if not line:
            continue
        if line.startswith("#,") and "fuzzy" in line:
            flush()
            fuzzy = True
            continue
        if line.startswith("#"):
            continue
        if line.startswith("msgid "):
            keep = fuzzy and msgid is None
            flush()
            fuzzy = keep
            msgid, field = po_unquote(line[6:].strip()), "id"
        elif line.startswith("msgstr "):
            msgstr, field = po_unquote(line[7:].strip()), "str"
        elif line.startswith('"'):
            if field == "id":
                msgid += po_unquote(line)
            else:
                msgstr += po_unquote(line)
        else:
            raise SystemExit(f"{path}: cannot parse: {raw}")
    flush()
    return entries, header


def write_po(path, header, entries, found):
    out = ['msgid ""', "msgstr " + po_quote(header), ""]
    def first_use(source):
        file, _, line = min(found[source], key=lambda w: (w.rsplit(":", 1)[0], int(w.rsplit(":", 1)[1])))\
            .rpartition(":")
        return (file, int(line), source)

    for source in sorted(found, key=first_use):
        translation, fuzzy = entries.get(source, ("", False))
        out.append("#: " + " ".join(sorted(found[source])[:3]))
        if fuzzy:
            out.append("#, fuzzy")
        out.append("msgid " + po_quote(source))
        out.append("msgstr " + po_quote(translation))
        out.append("")
    open(path, "w", encoding="utf-8", newline="\n").write("\n".join(out))


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    command = sys.argv[1]
    found = extract()
    if command == "extract":
        header = "Content-Type: text/plain; charset=UTF-8\nLanguage: es\n"
        write_po(os.path.join(ROOT, "data", "lang", "messages.pot"), header, {}, found)
        print(f"{len(found)} texts")
        return
    path = sys.argv[2]
    entries, header = read_po(path)
    if command == "update":
        write_po(path, header, entries, found)
        missing = sum(1 for s in found if not entries.get(s, ("", False))[0])
        print(f"{len(found)} texts, {missing} untranslated, {len(set(entries) - set(found))} dropped")
        return
    if command == "check":
        problems = []
        for source in sorted(found):
            translation, fuzzy = entries.get(source, ("", False))
            where = sorted(found[source])[0]
            if not translation or fuzzy:
                problems.append(f"{where}: untranslated: {source!r}")
            elif not compatible(source, translation):
                problems.append(f"{where}: placeholders differ: {source!r} -> {translation!r}")
        for source in sorted(set(entries) - set(found)):
            problems.append(f"{path}: not in the code any more: {source!r}")
        for problem in problems:
            print(problem)
        print(f"{os.path.relpath(path, ROOT)}: {len(found)} texts, {len(problems)} problems")
        sys.exit(1 if problems else 0)
    raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
