"""Windows integration test: real converter + serialized DML graph + ten native frames.
Requires onnx and a built converter beside DirectML.dll. No model downloads.
Run explicitly: python test_native_converter.py PATH_TO_DEPTH_CONVERT.EXE
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def create_fixture(path,height=192,width=352):
    import onnx
    from onnx import helper as h, TensorProto as T
    # Deliberately reverse declaration ordering; name pairing, not index, is required.
    shape=[1,1,height,width]
    inputs=[h.make_tensor_value_info('past_a',T.FLOAT,shape),
            h.make_tensor_value_info('image',T.FLOAT,[1,3,height,width]),
            h.make_tensor_value_info('past_b',T.FLOAT,shape)]
    outputs=[h.make_tensor_value_info('present_b',T.FLOAT,shape),
             h.make_tensor_value_info('depth',T.FLOAT,shape),
             h.make_tensor_value_info('present_a',T.FLOAT,shape)]
    init=[h.make_tensor('idx',T.INT64,[1],[0]),
          h.make_tensor('half',T.FLOAT,[],[0.5]), h.make_tensor('red_scale',T.FLOAT16,[],[0.75]), h.make_tensor('quarter',T.FLOAT,[],[0.25])]
    nodes=[h.make_node('Gather',['image','idx'],['red_input'],axis=1),
           h.make_node('Cast',['red_input'],['red_fp16'],to=T.FLOAT16),
           h.make_node('Mul',['red_fp16','red_scale'],['red_scaled']),
           h.make_node('Cast',['red_scaled'],['red'],to=T.FLOAT),
           h.make_node('Mul',['past_a','half'],['a_half']),
           h.make_node('Mul',['past_b','quarter'],['b_quarter']),
           h.make_node('Add',['red','a_half'],['present_a']),
           h.make_node('Add',['present_a','b_quarter'],['present_b']),
           h.make_node('Add',['present_b','red'],['depth'])]
    model=h.make_model(h.make_graph(nodes,'DepthStreamFixture',inputs,outputs,init),opset_imports=[h.make_opsetid('',18)])
    model.ir_version=10
    onnx.checker.check_model(model)
    onnx.save(model,str(path))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('converter',type=Path)
    a=p.parse_args()
    exe=a.converter.resolve()
    if not exe.is_file() or not exe.with_name('DirectML.dll').is_file():
        p.error('Converter and DirectML.dll must exist in the same folder')
    with tempfile.TemporaryDirectory() as tmp:
        for height,width in [(192,352),(224,384)]:
            graph=Path(tmp)/f'stream-{width}x{height}.onnx'; pack=graph.with_suffix('.dmlpack')
            create_fixture(graph,height,width)
            completed = subprocess.run([str(exe),str(graph),str(pack)],check=False)
            if (height,width) != (192,352):
                if completed.returncode == 0 or pack.exists():
                    raise RuntimeError('Converter accepted an unsupported image size')
                continue
            completed.check_returncode()
            if not pack.is_file(): raise RuntimeError('Converter did not publish a pack')
    print('Synthetic stream converter + native 10-frame parity PASS')

if __name__=='__main__': main()
