#!/usr/bin/env python3
"""Generate a deterministic structural SC2 XSD index; no game merge rules inferred."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
from lxml import etree

XS = 'http://www.w3.org/2001/XMLSchema'
Q = '{' + XS + '}'
SUPPORTED = {'schema', 'complexType', 'simpleType', 'complexContent', 'simpleContent',
             'extension', 'restriction', 'sequence', 'choice', 'element', 'attribute',
             'enumeration', 'minInclusive', 'maxInclusive', 'minExclusive', 'maxExclusive',
             'pattern', 'length', 'minLength', 'maxLength', 'whiteSpace', 'fractionDigits',
             'totalDigits', 'union', 'list'}

def generate(path: Path) -> dict:
    raw = path.read_bytes()
    root = etree.fromstring(raw, etree.XMLParser(resolve_entities=False, no_network=True))
    namespace = root.get('targetNamespace', '')
    def qname(node, value):
        if ':' in value:
            prefix, name = value.split(':', 1)
            return '{' + node.nsmap.get(prefix, '') + '}' + name
        return ('{' + namespace + '}' if namespace else '') + value
    def ast(node):
        kind = etree.QName(node).localname
        attrs = dict(sorted(node.attrib.items()))
        for key in ('type', 'base', 'ref', 'itemType'):
            if key in attrs: attrs[key] = qname(node, attrs[key])
        if 'memberTypes' in attrs:
            attrs['memberTypes'] = [qname(node, v) for v in attrs['memberTypes'].split()]
        return {'kind': kind, 'attributes': attrs, 'supported': kind in SUPPORTED,
                'children': [ast(child) for child in node if isinstance(child.tag, str) and child.tag != Q+'annotation']}
    types = {}
    runtime = {}
    def register(node, name):
        types[name] = ast(node)
        fields = []
        base = ''
        def visit(current):
            nonlocal base
            for child in current:
                if child.tag == Q+'annotation': continue
                kind = etree.QName(child).localname
                if kind in ('extension', 'restriction') and child.get('base'):
                    base = qname(child, child.get('base'))
                if kind in ('element', 'attribute'):
                    field = dict(child.attrib)
                    field['kind'] = kind
                    field['name'] = child.get('name', child.get('ref', ''))
                    field['type'] = qname(child, child.get('type', '')) if child.get('type') else ''
                    inline = child.find(Q+'complexType')
                    if inline is None: inline = child.find(Q+'simpleType')
                    if inline is not None:
                        inline_name = name + '/' + kind + ':' + field['name']
                        register(inline, inline_name)
                        field['type'] = inline_name
                    fields.append(field)
                    # Do not descend into a field's inline type in the owner's scope.
                elif kind in ('complexContent', 'simpleContent', 'extension', 'restriction', 'choice', 'sequence'):
                    visit(child)
        visit(node)
        runtime[name] = {'base': base, 'fields': fields}
    for node in root:
        if node.tag in (Q+'complexType', Q+'simpleType') and node.get('name'):
            register(node, qname(node, node.get('name')))
    catalog_types = {}
    for constraint in root.xpath('.//xs:element[@name="Catalog"]/xs:unique', namespaces={'xs': XS}):
        name = constraint.get('name', '')
        selector = constraint.find(Q+'selector')
        if not name.endswith('ID') or selector is None:
            continue
        for class_name in selector.get('xpath', '').split('|'):
            # Only the schema's explicit direct-class identity selectors are supported.
            if class_name and all(c.isalnum() or c in '_:' for c in class_name):
                catalog_types[qname(selector, class_name)] = name[:-2]
    return {'format': 'sc2dh.catalog-index.v1', 'sourceSha256': hashlib.sha256(raw).hexdigest(),
            'targetNamespace': namespace, 'schemaAttributes': dict(root.attrib),
            'catalogTypes': dict(sorted(catalog_types.items())),
            'types': dict(sorted(types.items())), 'runtimeTypes': dict(sorted(runtime.items())),
            'topLevel': [ast(n) for n in root if isinstance(n.tag, str) and n.tag not in (Q+'annotation', Q+'complexType', Q+'simpleType')]}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xsd', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    data = generate(args.xsd)
    args.output.write_text(json.dumps(data, ensure_ascii=False, sort_keys=True, separators=(',', ':'))+'\n', encoding='utf-8')
    print('types=', len(data['types']), 'sha256=', data['sourceSha256'])

if __name__ == '__main__': main()
