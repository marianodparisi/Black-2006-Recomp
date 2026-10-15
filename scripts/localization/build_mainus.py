"""Build a locale MainUS bank from a complete ID-keyed JSONL catalog."""

import argparse
import json
from pathlib import Path

from mainus_bank import MainUSBank


def load_catalog(path: Path):
    rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    values = {}
    for row in rows:
        record_id = int(row["id"], 16)
        if not row["text"].strip():
            raise ValueError(f"empty translation for ID {row['id']}")
        if record_id in values:
            raise ValueError(f"duplicate translation ID {row['id']}")
        values[record_id] = row["text"]
    return values


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source_bank", type=Path)
    parser.add_argument("catalog", type=Path)
    parser.add_argument("output_bank", type=Path)
    args = parser.parse_args()
    source = MainUSBank.parse(args.source_bank.read_bytes())
    values = load_catalog(args.catalog)
    expected = {record.record_id for record in source.records}
    if set(values) != expected:
        raise ValueError(f"catalog ID coverage differs: missing={len(expected-set(values))}, extra={len(set(values)-expected)}")
    output = source.rebuild(values, index_all=True)
    rebuilt = MainUSBank.parse(output)
    if rebuilt.indexed_count != len(rebuilt.records):
        raise ValueError("Rebuilt locale MainUS search index does not include every record")
    if [record.record_id for record in rebuilt.records] != [record.record_id for record in source.records]:
        raise ValueError("rebuilt MainUS IDs or ordering do not match the source")
    if [record.text for record in rebuilt.records] != [values[record.record_id] for record in source.records]:
        raise ValueError("rebuilt MainUS text does not match the locale catalog")
    args.output_bank.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output_bank.with_suffix(args.output_bank.suffix + ".tmp")
    temporary.write_bytes(output)
    temporary.replace(args.output_bank)
    print(f"Built {len(rebuilt.records)} records ({rebuilt.indexed_count} indexed): {args.output_bank}")


if __name__ == "__main__":
    main()
