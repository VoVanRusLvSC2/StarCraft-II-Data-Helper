#!/usr/bin/env python3
"""Validate SC2 GameData XML files against Blizzard's catalogsData.xsd."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Iterable

from lxml import etree


SC2_ARCHIVE_SUFFIXES = {".sc2map", ".sc2mod", ".sc2components", ".sc2campaign", ".sc2archive"}


def is_duplicate_identity_error(error: etree._LogEntry) -> bool:
    message = error.message or ""
    return "Duplicate key-sequence" in message and "unique identity-constraint" in message


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def default_xsd_path() -> Path:
    candidates = [
        repo_root() / "resources" / "catalogsData.xsd",
        Path.cwd() / "resources" / "catalogsData.xsd",
        Path(sys.argv[0]).resolve().parent / "resources" / "catalogsData.xsd",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return candidates[0]


def archive_entries(path: Path) -> Iterable[tuple[str, bytes]]:
    try:
        from mpyq import MPQArchive
    except ImportError as exc:
        raise RuntimeError("mpyq is required to validate SC2 archives") from exc

    files = MPQArchive(str(path)).extract()
    for key, value in files.items():
        name = key.decode("utf-8", errors="ignore") if isinstance(key, bytes) else str(key)
        data = value.encode("utf-8") if isinstance(value, str) else value
        normalized = name.replace("\\", "/").lower()
        if normalized.endswith(".xml") and "/gamedata/" in normalized:
            yield name, data


def is_sc2_archive_path(path: Path) -> bool:
    suffix = path.suffix.lower()
    if suffix in SC2_ARCHIVE_SUFFIXES:
        return True
    # The app validates pending files such as Map.SC2Map.sc2dh.pending before
    # atomically replacing the original archive.
    lowered_name = path.name.lower()
    return any(token in lowered_name for token in SC2_ARCHIVE_SUFFIXES)


def folder_entries(path: Path) -> Iterable[tuple[str, bytes]]:
    for xml_path in path.rglob("*.xml"):
        normalized = xml_path.as_posix().lower()
        if "/gamedata/" not in normalized and xml_path.name.lower() not in {
            "abildata.xml",
            "actordata.xml",
            "behaviordata.xml",
            "buttondata.xml",
            "datacollectiondata.xml",
            "effectdata.xml",
            "modeldata.xml",
            "requirementdata.xml",
            "unitdata.xml",
            "validatordata.xml",
            "weapondata.xml",
        }:
            continue
        yield str(xml_path), xml_path.read_bytes()


def input_entries(path: Path) -> Iterable[tuple[str, bytes]]:
    if path.is_dir():
        yield from folder_entries(path)
    elif is_sc2_archive_path(path):
        yield from archive_entries(path)
    else:
        yield str(path), path.read_bytes()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", help="SC2 archive, folder, or XML file")
    parser.add_argument("--xsd", default=str(default_xsd_path()), help="Path to catalogsData.xsd")
    parser.add_argument("--max-errors", type=int, default=20, help="Maximum errors printed per input")
    args = parser.parse_args()

    xsd_path = Path(args.xsd)
    if not xsd_path.exists():
        print(f"xsd_missing={xsd_path}", file=sys.stderr)
        return 2

    schema = etree.XMLSchema(etree.parse(str(xsd_path)))
    overall_ok = True
    for raw_input in args.inputs:
        input_path = Path(raw_input)
        if not input_path.exists():
            print(f"input_missing={input_path}", file=sys.stderr)
            overall_ok = False
            continue

        total = 0
        bad = 0
        duplicate_identity_warnings = 0
        printed = 0
        for name, data in input_entries(input_path):
            total += 1
            try:
                document = etree.fromstring(data)
            except etree.XMLSyntaxError as exc:
                bad += 1
                overall_ok = False
                if printed < args.max_errors:
                    print(f"{input_path}: {name}: XML syntax error: {exc}")
                    printed += 1
                continue

            if schema.validate(document):
                continue
            errors = list(schema.error_log)
            fatal_errors = [error for error in errors if not is_duplicate_identity_error(error)]
            duplicate_errors = [error for error in errors if is_duplicate_identity_error(error)]
            if not fatal_errors and duplicate_errors:
                duplicate_identity_warnings += len(duplicate_errors)
                if printed < args.max_errors:
                    print(f"{input_path}: {name}: XSD duplicate identity warning (non-blocking)")
                    for error in duplicate_errors[: max(1, min(5, args.max_errors - printed))]:
                        print(f"  line={error.line} column={error.column} {error.message}")
                        printed += 1
                        if printed >= args.max_errors:
                            break
                continue

            bad += 1
            overall_ok = False
            if printed >= args.max_errors:
                continue
            print(f"{input_path}: {name}: XSD validation failed")
            for error in fatal_errors[: max(1, min(5, args.max_errors - printed))]:
                print(f"  line={error.line} column={error.column} {error.message}")
                printed += 1
                if printed >= args.max_errors:
                    break

        status = "ok" if bad == 0 and duplicate_identity_warnings == 0 else "warning" if bad == 0 else "failed"
        print(
            f"validation_status={status} input={input_path} xml_checked={total} "
            f"xml_failed={bad} duplicate_identity_warnings={duplicate_identity_warnings}"
        )

    return 0 if overall_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
