#!/usr/bin/env python3
"""Offline adapter for the focal-free DepthPro2 ONNX export; not a runtime dependency."""
import argparse
from collections import Counter
from copy import deepcopy
import json
import math
from pathlib import Path

MODEL_SIZE = (192, 352)  # (H, W)

def shape(tensor):
    tt = tensor.type.tensor_type
    dims = tuple(d.dim_value if d.HasField('dim_value') else None for d in tt.shape.dim)
    return tt.elem_type, dims

def metadata(model):
    props = {x.key: x.value for x in model.metadata_props}
    raw = props.get('depthpro2_onnx')
    return json.loads(raw) if raw else {}

def inspect(model, onnx):
    inputs = {t.name: shape(t) for t in model.graph.input}
    outputs = {t.name: shape(t) for t in model.graph.output}
    states = {}
    for name, (dtype, dims) in inputs.items():
        if not name.startswith('past_'):
            continue
        out = 'present_' + name[5:]
        if outputs.get(out) != (dtype, dims):
            raise ValueError(f'Mismatched state input/output: {name} / {out}')
        if any(d is None or d <= 0 for d in dims):
            raise ValueError(f'Non-fixed cache shape: {name}')
        import math
        sizes = {onnx.TensorProto.FLOAT: 4, onnx.TensorProto.FLOAT16: 2}
        if dtype not in sizes:
            raise ValueError(f'Unsupported cache dtype: {name}: {dtype}')
        states[name] = {'output':out, 'shape':dims, 'dtype':onnx.TensorProto.DataType.Name(dtype), 'bytes':math.prod(dims)*sizes[dtype]}
    if {n for n in outputs if n.startswith('present_')} != {s['output'] for s in states.values()}:
        raise ValueError('Unexpected unmatched present-state output')
    return {'metadata':metadata(model), 'inputs':inputs, 'outputs':outputs,
            'operators':dict(Counter(f'{n.domain or "ai.onnx"}::{n.op_type}' for n in model.graph.node)),
            'states':states, 'state_bytes':sum(s['bytes'] for s in states.values()),
            'ping_pong_state_bytes':2*sum(s['bytes'] for s in states.values())}

# ORT DirectML ReshapeHelper mishandles allowzero=1 together with -1.
# ONNX's allowzero only changes the meaning of literal zeros. For a constant
# target with no zero it is always safe to remove the attribute; when the
# input shape is known we also resolve -1 explicitly. Do not change dynamic
# or zero-sized targets, or mutate shared initializer tensors.
def reshape_target(target, input_dims=None):
    target = tuple(int(d) for d in target)
    if not target or target.count(-1) != 1 or any(d == 0 or d < -1 for d in target):
        return None
    if input_dims and all(isinstance(d, int) and d > 0 for d in input_dims):
        total = math.prod(input_dims)
        known = math.prod(d for d in target if d != -1)
        if total % known:
            raise ValueError(f'Invalid Reshape element count: {input_dims} -> {target}')
        return tuple(total // known if d == -1 else d for d in target)
    return target


def normalize_dml_reshape(model, onnx):
    from onnx import helper, numpy_helper, TensorProto
    g = model.graph
    # Only proven compile-time constants. Preserve all shared consumers.
    runtime_inputs = {v.name for v in g.input}
    constants = {t.name:t for t in g.initializer if t.name not in runtime_inputs}
    integer_lists = {}
    for n in g.node:
        if n.domain not in ('','ai.onnx') or n.op_type != 'Constant' or len(n.output) != 1:
            continue
        for a in n.attribute:
            if a.name == 'value' and a.type == onnx.AttributeProto.TENSOR:
                constants[n.output[0]] = a.t
            elif a.name == 'value_ints' and a.type == onnx.AttributeProto.INTS:
                integer_lists[n.output[0]] = tuple(a.ints)
    shapes = {v.name:shape(v)[1] for v in list(g.input)+list(g.output)+list(g.value_info)
              if v.type.HasField('tensor_type') and v.type.tensor_type.HasField('shape')}
    shapes.update({t.name:tuple(t.dims) for t in g.initializer})
    names = {v.name for v in list(g.input)+list(g.output)+list(g.value_info)+list(g.initializer)}
    names.update(x for n in g.node for x in list(n.input)+list(n.output) if x)
    changed, unresolved = [], []
    for i,n in enumerate(g.node):
        if n.domain not in ('','ai.onnx') or n.op_type != 'Reshape' or len(n.input) != 2:
            continue
        a = next((a for a in n.attribute if a.name == 'allowzero' and a.i == 1), None)
        if a is None:
            continue
        key = n.input[1]
        target = integer_lists.get(key)
        if target is None and key in constants:
            t = constants[key]
            if t.data_type == TensorProto.INT64 and len(t.dims) == 1:
                target = tuple(int(x) for x in numpy_helper.to_array(t))
        if target is None:
            unresolved.append(n.name or n.output[0]); continue
        fixed = reshape_target(target, shapes.get(n.input[0]))
        if fixed is None:
            continue
        # Removing allowzero is sufficient even if -1 must remain. Prefer an
        # explicit shape when the input has a known, positive element count.
        if fixed != target:
            name = f'edition_dp2_reshape_{i}'
            while name in names: name += '_'
            names.add(name)
            g.initializer.append(helper.make_tensor(name, TensorProto.INT64, [len(fixed)], fixed))
            n.input[1] = name
        n.attribute.remove(a)
        changed.append({'node':n.name or n.output[0], 'from':target, 'to':fixed})
    print(json.dumps({'dml_reshape_fix':len(changed), 'rewrites':changed,
                      'unresolved_allowzero_shapes':unresolved}, indent=2))
    return changed


def adapt(model, onnx):
    from onnx import helper, TensorProto
    graph = model.graph
    report = inspect(model, onnx)
    ins, outs = report['inputs'], report['outputs']
    state_inputs = set(report['states'])
    state_outputs = {x['output'] for x in report['states'].values()}
    if set(ins) != {'pixel_values','f_px'} | state_inputs or set(outs) != {'depth_meters'} | state_outputs:
        raise ValueError('Requires the focal-free streaming export.')
    if not state_inputs:
        raise ValueError('The fixed W4 export must expose temporal state.')
    if len(state_inputs) > 32 or report['state_bytes'] > 64 * 1024 * 1024:
        raise ValueError('State exceeds native depth limits')
    dtype, dims = ins['pixel_values']
    if dtype != TensorProto.FLOAT or len(dims) != 4 or dims[:2] != (1,3) or dims[2:] != MODEL_SIZE:
        raise ValueError(f'Unsupported input: {dims}; expected fixed FP32 NCHW at 352x192.')
    h,w = dims[2:]
    if ins['f_px'] != (TensorProto.FLOAT,(1,)) or outs['depth_meters'] != (TensorProto.FLOAT,(1,h,w)):
        raise ValueError('Unexpected focal or metric depth tensor contract')
    meta = report['metadata']
    if (meta.get('mode'), meta.get('window'), meta.get('precision')) != ('stream', 4, 'fp16'):
        raise ValueError('Expected the upstream W4 mixed FP16 stream metadata')
    if meta.get('focal_estimator_removed') is not True or (meta.get('height'), meta.get('width')) != MODEL_SIZE:
        raise ValueError('Expected focal-free 352x192 model metadata')
    if not any(o.domain in ('','ai.onnx') and o.version >= 13 for o in model.opset_import):
        raise ValueError('ONNX opset >= 13 required')
    if any(a.type in (onnx.AttributeProto.GRAPH, onnx.AttributeProto.GRAPHS) for n in graph.node for a in n.attribute):
        raise ValueError('Nested subgraphs are not supported by this adapter')
    names = {v.name for v in list(graph.input)+list(graph.output)+list(graph.initializer)+list(graph.value_info)}
    names.update(x for n in graph.node for x in list(n.input)+list(n.output) if x)
    reserved = {'image','depth','edition_dp2_inverse','edition_dp2_axes'}
    if names & reserved:
        raise ValueError(f'Reserved tensor name collision: {names & reserved}')
    if 'f_px' in {t.name for t in graph.initializer}:
        raise ValueError('Focal length already has an initializer; unexpected export')
    # Source contract: depth_meters = f_px / (width * inverse_depth).
    # Fix f_px to width, then reciprocate metric depth to recover the original
    # near-high inverse depth, including 1/inf == 0 for sky. No focal inference.
    # Preserve the original graph rather than guessing its internal node names.
    for n in graph.node:
        for i, name in enumerate(n.input):
            if name == 'pixel_values': n.input[i] = 'image'
    for v in graph.value_info:
        if v.name == 'pixel_values': v.name = 'image'
    state_in = [deepcopy(v) for v in graph.input if v.name in state_inputs]
    state_out = [deepcopy(v) for v in graph.output if v.name in state_outputs]
    del graph.input[:]
    graph.input.extend([helper.make_tensor_value_info('image',TensorProto.FLOAT,[1,3,h,w]), *state_in])
    graph.initializer.extend([helper.make_tensor('f_px',TensorProto.FLOAT,[1],[float(w)]),
                              helper.make_tensor('edition_dp2_axes',TensorProto.INT64,[1],[1])])
    graph.node.extend([helper.make_node('Reciprocal',['depth_meters'],['edition_dp2_inverse'],name='EditionDP2Inverse'),
                       helper.make_node('Unsqueeze',['edition_dp2_inverse','edition_dp2_axes'],['depth'],name='EditionDP2NCHW')])
    del graph.output[:]
    graph.output.extend([helper.make_tensor_value_info('depth',TensorProto.FLOAT,[1,1,h,w]), *state_out])
    p=model.metadata_props.add(); p.key='edition_depth_adapter'; p.value='inverse-depth-stream-v1; focal fixed to width; research only'
    normalize_dml_reshape(model, onnx)
    onnx.checker.check_model(model)
    return model

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input',type=Path)
    parser.add_argument('output',type=Path,nargs='?')
    parser.add_argument('--audit',action='store_true',help='Print tensor and operator inventory, including W4 state footprint; no conversion')
    args=parser.parse_args()
    if not args.audit and args.output is None: parser.error('output required unless --audit')
    if args.output is not None and args.input.resolve()==args.output.resolve(): parser.error('refusing to overwrite source')
    import onnx
    model=onnx.load(str(args.input),load_external_data=True)
    onnx.checker.check_model(model)
    print(json.dumps(inspect(model,onnx),indent=2))
    if args.audit: return
    adapt(model,onnx)
    onnx.external_data_helper.convert_model_from_external_data(model)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    if args.output.exists(): raise FileExistsError(args.output)
    onnx.save_model(model,str(args.output),save_as_external_data=False)
    print(f'Wrote {args.output}; run existing native DirectML converter to validate and pack.')

if __name__=='__main__': main()
