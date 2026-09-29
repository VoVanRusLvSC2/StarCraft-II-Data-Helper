"""Offline generator: preserve type evidence, never infer a missing type."""
import argparse
import hashlib
import json
from pathlib import Path

from lxml import etree


def generate(source: Path, build: int):
    raw = source.read_bytes()
    if b"<!DOCTYPE" in raw.upper():
        raise ValueError("DOCTYPE is not allowed in GUI binding evidence")
    root = etree.fromstring(raw, etree.XMLParser(
        resolve_entities=False, no_network=True, load_dtd=False))
    if root.getroottree().docinfo.doctype:
        raise ValueError("DOCTYPE is not allowed in GUI binding evidence")
    if root.tag != "TriggerData":
        raise ValueError("Expected an unqualified TriggerData root")
    standard = root.find("Standard")
    containers = [(root, standard.get("Id", "") if standard is not None else "")]
    parameters, functions = {}, {}

    def identity(node, library):
        return node.get("Library", library) + "|" + node.get("Id", "")

    def insert(mapping, key, value):
        if key in mapping and mapping[key] != value:
            raise ValueError("Conflicting source definitions: " + key)
        mapping[key] = value

    for container, library in containers:
        for node in container:
            if node.tag == "Library":
                containers.append((node, node.get("Id", "")))
            elif node.tag == "Element" and node.get("Type") == "ParamDef":
                definition = node.find("ParameterType")
                kind = definition.find("Type") if definition is not None else None
                game = definition.find("GameType") if definition is not None else None
                insert(parameters, library + "|" + node.get("Id", ""), {
                    "kind": kind.get("Value", "") if kind is not None else "",
                    "gameType": game.get("Value", "") if game is not None else "",
                })
            elif node.tag == "Element" and node.get("Type") == "FunctionDef":
                insert(functions, library + "|" + node.get("Id", ""), sorted({
                    identity(parameter, library) for parameter in node.findall("Parameter")
                    if parameter.get("Type") == "ParamDef"
                }))
    return {
        "formatVersion": 1, "build": build, "source": source.name,
        "sourceSha256": hashlib.sha256(raw).hexdigest(),
        "parameters": parameters, "functions": functions,
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--build", type=int, required=True)
    args = parser.parse_args()
    # Canonical CRLF retains the shipped Windows artifact byte-for-byte on every host.
    args.output.write_bytes((json.dumps(generate(args.source, args.build), sort_keys=True, indent=2)
                            + "\n").replace("\n", "\r\n").encode("utf-8"))
