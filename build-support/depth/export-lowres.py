#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Resolve a complete official checkpoint before invoking the unmodified exporter.

The Hub mixin can suppress an HTTP error for config.json and then construct a
model with missing arguments. Fetch both files explicitly, pin one revision,
and run the original exporter against the complete local Hub cache.
"""
import argparse
import importlib
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import time

REPO = 'apple/DepthPro2-S'
EXPORTER_PIN = '30e647174773e3bdf5ab5dcc2a815c38cec5fdd8'
PIN = '903a1c1ade918831d138adcc4aedbcc8f4da0078'
REQUIRED = ('backbone_preset', 'reassembled_blocks', 'cross_frame_blocks',
            'dims_reassemble', 'dim_decoder')


def validate_config(path):
    config = json.loads(Path(path).read_text(encoding='utf-8'))
    if not isinstance(config, dict) or any(k not in config for k in REQUIRED):
        raise ValueError('Official DepthPro2-S config.json is missing model settings; refusing to invent them.')
    if config.get('backbone_preset') != 'vit_small_patch16_dinov3' or config.get('fov') is not False:
        raise ValueError('Official checkpoint is not the focal-free S variant expected by the exporter.')
    for key in REQUIRED[1:-1]:
        if not isinstance(config[key], list) or not config[key] or any(type(v) is not int for v in config[key]):
            raise ValueError(f'Invalid official DepthPro2-S configuration field: {key}')
    if type(config['dim_decoder']) is not int or config['dim_decoder'] <= 0:
        raise ValueError('Invalid official DepthPro2-S decoder dimension.')
    return config


def retry_errors():
    from huggingface_hub.errors import HfHubHTTPError, LocalEntryNotFoundError
    # Hub 2.x uses httpx2; earlier releases use httpx or requests. Only
    # include classes from installed transports, never install an extra client.
    errors = [HfHubHTTPError, LocalEntryNotFoundError]
    for name, attributes in (('httpx2', ('TransportError',)),
                             ('httpx', ('TransportError',)),
                             ('requests', ('exceptions.ConnectionError', 'exceptions.Timeout'))):
        try:
            module = importlib.import_module(name)
        except ModuleNotFoundError as exc:
            if exc.name != name:
                raise  # A broken installed backend is not an absent backend.
            continue
        for attribute in attributes:
            cls = module
            for part in attribute.split('.'):
                cls = getattr(cls, part, None)
            if isinstance(cls, type) and issubclass(cls, Exception):
                errors.append(cls)
    return tuple(dict.fromkeys(errors))


def retry(call, sleep=time.sleep):
    errors = retry_errors()
    for i in range(3):
        try:
            return call()
        except errors as exc:
            status = getattr(getattr(exc, 'response', None), 'status_code', None)
            # Only retry transient HTTP statuses or transport/cache misses.
            # Never bypass access restrictions or retry a definitive 404.
            if (status is not None and status not in (408, 429) and not 500 <= status <= 599) or i == 2:
                raise
            print(f'[WARN] Checkpoint request failed ({type(exc).__name__}); retry {i+1}/2.', flush=True)
            sleep(2 ** (i + 1))


def prepare_checkpoint(download, retry_call=retry):
    revision = PIN
    config_path = retry_call(lambda: download(REPO, 'config.json', revision=revision))
    validate_config(config_path)
    print(f'[INFO] Verified official S config at {revision}; preparing matching safetensors.', flush=True)
    weights = retry_call(lambda: download(REPO, 'model.safetensors', revision=revision))
    if not Path(weights).is_file() or Path(weights).stat().st_size <= 8:
        raise ValueError('Official S weight file is empty or incomplete.')
    # Cache-only check now, while the acquisition errors are still explicit.
    local_config = download(REPO, 'config.json', revision=revision, local_files_only=True)
    local_weights = download(REPO, 'model.safetensors', revision=revision, local_files_only=True)
    if Path(local_config).resolve() != Path(config_path).resolve() or Path(local_weights).resolve() != Path(weights).resolve():
        raise RuntimeError('Checkpoint cache changed during preparation.')
    # Verify safetensors structure without loading a second full model.
    from safetensors import safe_open
    with safe_open(str(local_weights), framework='pt', device='cpu') as f:
        if not f.keys():
            raise ValueError('Official S weight file has no tensors.')
    return revision


def offline_export(args):
    # Fresh process: HF_HUB_OFFLINE is already set before any Hub imports.
    if args.revision != PIN:
        raise ValueError('Invalid checkpoint revision.')
    checkout = args.checkout.resolve()
    sys.path.insert(0, str(checkout))
    from depthpro2_onnx import model
    if model.CHECKPOINTS['S'] != (REPO, EXPORTER_PIN):
        raise RuntimeError('Unexpected upstream checkpoint mapping; refusing mismatched exporter.')
    # Only the resource revision may change (verified official revision). The
    # original exporter, load_model, precision and graph code stay untouched.
    model.CHECKPOINTS['S'] = (REPO, args.revision)
    script = checkout / 'export_onnx.py'
    sys.argv = [str(script), '--size', 'S', '--width', str(args.width),
                '--height', str(args.height), '--stream', '4', '--only',
                'model_stream4_fp16', '--out', str(args.out.resolve())]
    runpy.run_path(str(script), run_name='__main__')
    # Resource provenance only; do not modify the exported ONNX.
    (args.out.resolve() / 'checkpoint.json').write_text(
        json.dumps({'repo': REPO, 'revision': args.revision}), encoding='utf-8')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--checkout', type=Path, required=True)
    parser.add_argument('--width', type=int, required=True)
    parser.add_argument('--height', type=int, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--revision', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.revision:
        offline_export(args)
        return
    from huggingface_hub import hf_hub_download
    revision = prepare_checkpoint(hf_hub_download)
    print(f'[INFO] Complete official S checkpoint cached; exporting offline at {args.width}x{args.height}.', flush=True)
    env = dict(os.environ, HF_HUB_OFFLINE='1')
    subprocess.run([sys.executable, str(Path(__file__).resolve()), *sys.argv[1:],
                    '--revision', revision], env=env, check=True)


if __name__ == '__main__':
    main()
