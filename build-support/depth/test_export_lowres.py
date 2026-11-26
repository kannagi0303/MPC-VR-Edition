import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('export_lowres', Path(__file__).with_name('export-lowres.py'))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


class Missing(Exception): pass
class Denied(Exception): pass


class CheckpointTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.config = self.root / 'config.json'
        self.weights = self.root / 'model.safetensors'
        self.config.write_text(json.dumps(dict(backbone_preset='vit_small_patch16_dinov3',
            reassembled_blocks=[2,5,8,11], cross_frame_blocks=[5,7,9,11],
            dims_reassemble=[48,96,192,384], dim_decoder=64, fov=False)))
        # A real, structurally valid tiny safetensors file; not a DP2 model.
        from safetensors.numpy import save_file
        import numpy as np
        save_file({'test': np.ones((2,), dtype=np.float32)}, str(self.weights))
        self.calls=[]
        self.rev=m.PIN
    def download(self, repo, file, revision, local_files_only=False):
        self.calls.append((repo,file,revision,local_files_only))
        return str(self.config if file=='config.json' else self.weights)
    def prepare(self, download=None):
        return m.prepare_checkpoint(download or self.download, lambda f:f())
    def test_pin_and_cache(self):
        self.assertEqual(self.prepare(),m.PIN)
        self.assertEqual(self.calls,[(m.REPO,'config.json',m.PIN,False),
            (m.REPO,'model.safetensors',m.PIN,False),(m.REPO,'config.json',m.PIN,True),
            (m.REPO,'model.safetensors',m.PIN,True)])
    def test_missing_pin_is_reported(self):
        def get(*args, **kwargs):
            raise Missing()
        with self.assertRaises(Missing): self.prepare(get)
    def test_auth_error_never_falls_back(self):
        def get(*a,**k): raise Denied()
        with self.assertRaises(Denied): self.prepare(get)
        self.assertEqual(self.calls,[])
    def test_invalid_config_prevents_weight_fetch(self):
        self.config.write_text('{}')
        with self.assertRaises(ValueError): self.prepare()
        self.assertEqual(len(self.calls),1)
    def test_invalid_weights_prevent_export(self):
        self.weights.write_bytes(b'not a safetensors file')
        with self.assertRaises(Exception): self.prepare()
    def test_offline_original_exporter_args_and_provenance(self):
        package=self.root/'depthpro2_onnx';package.mkdir()
        (package/'__init__.py').write_text('')
        (package/'model.py').write_text(f'CHECKPOINTS={{"S":({m.REPO!r},{m.EXPORTER_PIN!r})}}')
        (self.root/'export_onnx.py').write_text('''import json,sys,os
from pathlib import Path
from depthpro2_onnx.model import CHECKPOINTS
assert os.environ['HF_HUB_OFFLINE']=='1'
assert sys.argv[1:-2]==['--size','S','--width','352','--height','192','--stream','4','--only','model_stream4_fp16']
p=Path(sys.argv[-1]);p.mkdir();(p/'invocation.json').write_text(json.dumps(CHECKPOINTS['S']))
''')
        out=self.root/'out'
        import os
        subprocess.run([sys.executable,str(Path(m.__file__).resolve()),'--checkout',str(self.root),
            '--width','352','--height','192','--out',str(out),'--revision',self.rev],
            env=dict(os.environ,HF_HUB_OFFLINE='1'),check=True)
        self.assertEqual(json.loads((out/'invocation.json').read_text()),[m.REPO,self.rev])
        self.assertEqual(json.loads((out/'checkpoint.json').read_text()),dict(repo=m.REPO,revision=self.rev))


class RetryTransportTests(unittest.TestCase):
    # Exercise the actual retry function with only the pinned Hub 2.x
    # transport available. This is an API fixture, not a real HTTP request.
    def test_httpx2_only(self):
        class TransportError(Exception): pass
        real_import = m.importlib.import_module
        def import_backend(name, *args, **kwargs):
            if name == 'httpx2':
                return types.SimpleNamespace(TransportError=TransportError)
            if name in ('httpx', 'requests'):
                raise ModuleNotFoundError(name=name)
            return real_import(name, *args, **kwargs)
        calls=[]; delays=[]
        def download():
            calls.append(1)
            if len(calls) < 3: raise TransportError('temporary')
            return 'success'
        with patch.object(m.importlib, 'import_module', side_effect=import_backend):
            self.assertEqual(m.retry(download, delays.append), 'success')
        self.assertEqual(len(calls), 3)
        self.assertEqual(delays, [2, 4])

    def test_no_optional_transport_import(self):
        def absent(name): raise ModuleNotFoundError(name=name)
        with patch.object(m.importlib, 'import_module', side_effect=absent):
            self.assertEqual(m.retry(lambda: 'cached'), 'cached')

    def test_retry_statuses(self):
        class TransportError(Exception):
            def __init__(self, status):
                self.response = types.SimpleNamespace(status_code=status)
        for status, expected in ((400,1),(401,1),(403,1),(404,1),(408,3),(429,3),(500,3),(503,3)):
            calls=[]; delays=[]
            def download():
                calls.append(1)
                raise TransportError(status)
            with self.subTest(status=status), patch.object(m, 'retry_errors', return_value=(TransportError,)):
                with self.assertRaises(TransportError): m.retry(download, delays.append)
                self.assertEqual(len(calls), expected)
                self.assertEqual(len(delays), expected-1)

    def test_programming_error_not_retried(self):
        def download(): raise TypeError('invalid argument')
        with self.assertRaises(TypeError): m.retry(download, lambda _: self.fail('unexpected retry'))

if __name__=='__main__': unittest.main()
