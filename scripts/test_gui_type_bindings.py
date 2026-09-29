import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from generate_gui_type_bindings import generate


class GuiBindingTests(unittest.TestCase):
    def fixture(self, xml):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / 'test.triggerlib'
        path.write_bytes(xml.encode('utf-8'))
        return path

    def test_library_scope_and_function_parameter_identity(self):
        source = self.fixture('''<TriggerData><Standard Id="Ntve"/>
          <Element Type="ParamDef" Id="U"><ParameterType><Type Value="gamelink"/><GameType Value="Unit"/></ParameterType></Element>
          <Element Type="FunctionDef" Id="F"><Parameter Type="ParamDef" Id="U"/><Parameter Type="ParamDef" Library="Local" Id="S"/></Element>
          <Library Id="Local"><Element Type="ParamDef" Id="U"><ParameterType><Type Value="string"/></ParameterType></Element></Library>
        </TriggerData>''')
        result = generate(source, 97563)
        self.assertEqual(result['parameters'], {'Ntve|U': {'kind': 'gamelink', 'gameType': 'Unit'},
                                                'Local|U': {'kind': 'string', 'gameType': ''}})
        self.assertEqual(result['functions'], {'Ntve|F': ['Local|S', 'Ntve|U']})
        self.assertEqual(result['sourceSha256'], hashlib.sha256(source.read_bytes()).hexdigest())
        self.assertEqual(generate(source, 97563), result)

    def test_conflicting_definitions_rejected(self):
        source = self.fixture('''<TriggerData><Element Type="ParamDef" Id="X"><ParameterType><Type Value="string"/></ParameterType></Element>
          <Element Type="ParamDef" Id="X"><ParameterType><Type Value="gamelink"/><GameType Value="Unit"/></ParameterType></Element></TriggerData>''')
        with self.assertRaisesRegex(ValueError, 'Conflicting'):
            generate(source, 1)

    def test_missing_type_not_inferred(self):
        source = self.fixture('<TriggerData><Element Type="ParamDef" Id="UnitName"><ParameterType><SameAs Id="Unknown"/></ParameterType></Element></TriggerData>')
        self.assertEqual(generate(source, 1)['parameters']['|UnitName'], {'kind': '', 'gameType': ''})

    def test_dtd_and_namespace_rejected(self):
        for xml in ('<!DOCTYPE TriggerData [<!ENTITY ext SYSTEM "file:///missing">]><TriggerData/>',
                    '<TriggerData xmlns="urn:unsupported"/>'):
            source = self.fixture(xml)
            with self.assertRaises(ValueError):
                generate(source, 1)

    def test_bundled_independently_verified_native_signature(self):
        path = Path(__file__).resolve().parents[1] / 'resources/gui_type_bindings.json'
        raw = path.read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), 'c58ea0734694b55153dba68f96d80801fc9b717ad758e23fb2a96bd7d571fdb5')
        data = json.loads(raw)
        self.assertEqual(data['build'], 97563)
        self.assertEqual(len(data['parameters']), 6554)
        self.assertEqual(len(data['functions']), 3196)
        self.assertEqual(data['parameters']['Ntve|EF0CF6FF'], {'kind': 'gamelink', 'gameType': 'Unit'})
        self.assertIn('Ntve|EF0CF6FF', data['functions']['Ntve|6C39A0DF'])


if __name__ == '__main__':
    unittest.main()
