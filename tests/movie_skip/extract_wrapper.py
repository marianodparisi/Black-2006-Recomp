"""Derive authored movie wrapper snippets into the CMake build directory only."""
import argparse
from pathlib import Path
import re


def function_end(text: str, signature: str) -> int:
    matches = list(re.finditer(re.escape(signature) + r"\s*\{", text))
    if len(matches) != 1:
        raise ValueError(f"Expected exactly one definition of {signature}")
    pos = text.index("{", matches[0].start())
    depth = 0
    # Count code braces, ignoring comments and quoted literals.
    tokens = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)
    for token in tokens.finditer(text, pos):
        value = token.group()
        if value == "{":
            depth += 1
        elif value == "}":
            depth -= 1
            if depth == 0:
                return token.end()
    raise ValueError(f"Unterminated definition of {signature}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    text = args.source.read_text(encoding="utf-8")
    marker = "extern void sub_000C4570_gen(void);"
    if text.count(marker) != 1:
        raise ValueError("Expected exactly one authored movie wrapper declaration")
    wrapper = text[text.index(marker):function_end(text, "void sub_000C4570(void)")]
    font_signature = "static void black_movie_skip_font(void)"
    font_match = re.search(re.escape(font_signature) + r"\s*\{", text)
    if font_match is None:
        raise ValueError("Movie hint font helper is missing")
    font = text[font_match.start():function_end(text, font_signature)]
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for name, source in (("movie-wrapper-under-test.inc", wrapper), ("movie-font-under-test.inc", font)):
        (args.output_dir / name).write_text("/* Derived from the current authored production source. */\n" + source + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
