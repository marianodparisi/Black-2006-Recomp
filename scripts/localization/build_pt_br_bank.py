"""Build the Brazilian Portuguese MainUS bank from the verified English base."""

import argparse
import hashlib
import json
from pathlib import Path

from mainus_bank import MainUSBank


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source_bank", type=Path)
    parser.add_argument("translation_source", type=Path)
    parser.add_argument("output_bank", type=Path)
    args = parser.parse_args()

    source_bytes = args.source_bank.read_bytes()
    source_hash = hashlib.sha256(source_bytes).hexdigest().upper()
    source = MainUSBank.parse(source_bytes)
    translation_data = json.loads(args.translation_source.read_text(encoding="utf-8"))
    if translation_data.get("formatVersion") != 1:
        raise ValueError("Unsupported PT-BR translation source version")
    if translation_data.get("englishBaseSha256", "").upper() != source_hash:
        raise ValueError("PT-BR translations do not match this original English MainUS bank")

    translations = translation_data.get("translations")
    if not isinstance(translations, dict) or len(translations) != translation_data.get("translatedEntries"):
        raise ValueError("PT-BR translation count does not match its metadata")

    by_id = {record.record_id: record for record in source.records}
    localized = {}
    for key, value in translations.items():
        try:
            record_id = int(key, 16)
        except (TypeError, ValueError) as error:
            raise ValueError("Invalid PT-BR record ID: " + str(key)) from error
        if record_id not in by_id:
            raise ValueError("PT-BR source contains an unknown MainUS record ID: " + key)
        if not isinstance(value, str) or not value.strip():
            raise ValueError("PT-BR translation is empty or invalid for record ID: " + key)
        localized[record_id] = value

    # The catalog omits unchanged strings; preserve those from the verified base.
    values = {record.record_id: localized.get(record.record_id, record.text) for record in source.records}
    # Locale banks need every record in the hash index used by runtime lookup.
    # Retaining the smaller English index leaves remaining records unsearchable.
    output = source.rebuild(values, index_all=True)
    rebuilt = MainUSBank.parse(output)
    if rebuilt.indexed_count != len(rebuilt.records):
        raise ValueError("Rebuilt PT-BR MainUS search index does not include every record")
    if [record.record_id for record in rebuilt.records] != [record.record_id for record in source.records]:
        raise ValueError("Rebuilt PT-BR MainUS IDs or ordering differ from the English source")
    if [record.text for record in rebuilt.records] != [values[record.record_id] for record in source.records]:
        raise ValueError("Rebuilt PT-BR MainUS text does not match the translation source")

    args.output_bank.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output_bank.with_suffix(args.output_bank.suffix + ".tmp")
    try:
        temporary.write_bytes(output)
        temporary.replace(args.output_bank)
    finally:
        if temporary.exists():
            temporary.unlink()
    print(
        f"Built {len(rebuilt.records)} records ({len(localized)} translated, "
        f"{len(rebuilt.records) - len(localized)} English fallbacks): {args.output_bank}"
    )


if __name__ == "__main__":
    main()
