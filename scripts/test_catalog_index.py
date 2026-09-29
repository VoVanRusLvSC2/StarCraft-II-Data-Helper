import hashlib
import tempfile
import unittest
import os
import json
from pathlib import Path
from lxml import etree
from generate_catalog_index import generate

class CatalogIndexTests(unittest.TestCase):
    xsd = Path(os.environ.get('SC2DH_XSD_PATH', str(Path(__file__).resolve().parents[1] / 'resources/catalogsData.xsd')))

    def test_anonymous_array_scope_and_carrier(self):
        data = generate(self.xsd)
        types = data['runtimeTypes']
        fields = types['CEffectSet']['fields']
        effect = next(f for f in fields if f['name'] == 'EffectArray')
        self.assertEqual(effect['maxOccurs'], 'unbounded')
        self.assertFalse(any(f['name'] in ('index', 'removed') for f in fields))
        inline = types[effect['type']]
        self.assertEqual(inline['base'], 'CEffectLink')
        self.assertEqual({f['name'] for f in inline['fields']}, {'index', 'removed'})
        self.assertEqual(types['CEffectLink']['fields'][0]['name'], 'value')
        # Check independently in the source schema, without generating expected output.
        source = etree.parse(str(self.xsd))
        ns = {'x': 'http://www.w3.org/2001/XMLSchema'}
        self.assertEqual(source.xpath('string(//x:complexType[@name="CEffectSet"]//x:element[@name="EffectArray"]/x:complexType/x:complexContent/x:extension/@base)', namespaces=ns), 'CEffectLink')

    def test_determinism_and_hash(self):
        self.assertEqual(generate(self.xsd), generate(self.xsd))
        self.assertEqual(generate(self.xsd)['sourceSha256'], hashlib.sha256(self.xsd.read_bytes()).hexdigest())

    def test_catalog_identity_is_from_explicit_source_constraints(self):
        data = generate(self.xsd)
        self.assertEqual(data['catalogTypes']['CRequirement'], 'Requirement')
        self.assertEqual(data['catalogTypes']['CRequirementAnd'], 'RequirementNode')
        source = etree.parse(str(self.xsd))
        ns = {'x': 'http://www.w3.org/2001/XMLSchema'}
        selector = source.xpath('string(//x:element[@name="Catalog"]/x:unique[@name="RequirementNodeID"]/x:selector/@xpath)', namespaces=ns)
        self.assertIn('CRequirementAnd', selector.split('|'))

    def test_bundled_index_matches_current_schema(self):
        bundled = self.xsd.parent / 'catalog_type_index.json'
        self.assertEqual(json.loads(bundled.read_text(encoding='utf-8')), generate(self.xsd))

    def test_namespace_and_nested_choice(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/'test.xsd'
            path.write_text('<s:schema xmlns:s="http://www.w3.org/2001/XMLSchema" xmlns:t="urn:test" targetNamespace="urn:test"><s:complexType name="Outer"><s:choice><s:sequence><s:element name="Items" maxOccurs="unbounded"><s:complexType><s:complexContent><s:extension base="t:Base"><s:attribute name="index" type="s:integer"/></s:extension></s:complexContent></s:complexType></s:element></s:sequence></s:choice></s:complexType></s:schema>')
            data = generate(path)
            owner = data['runtimeTypes']['{urn:test}Outer']
            self.assertEqual(len(owner['fields']), 1)
            inline = data['runtimeTypes'][owner['fields'][0]['type']]
            self.assertEqual(inline['base'], '{urn:test}Base')
            self.assertEqual(inline['fields'][0]['type'], '{http://www.w3.org/2001/XMLSchema}integer')
            self.assertEqual(data['types']['{urn:test}Outer']['children'][0]['kind'], 'choice')

if __name__ == '__main__': unittest.main()
