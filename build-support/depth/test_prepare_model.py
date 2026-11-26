import json
import tempfile
import unittest
from pathlib import Path
try:
    import onnx
    from onnx import helper, TensorProto
except ImportError:
    onnx=None
from prepare_model import adapt, inspect, reshape_target, normalize_dml_reshape

@unittest.skipUnless(onnx is not None,'requires offline onnx package')
class ContractTests(unittest.TestCase):
    def make(self,h=192,w=352,stream=True):
        # Synthetic metric model: red-channel inverse depth -> metric depth.
        F=TensorProto.FLOAT; I=TensorProto.INT64
        inputs=[helper.make_tensor_value_info('pixel_values',F,[1,3,h,w]),helper.make_tensor_value_info('f_px',F,[1])]
        outputs=[helper.make_tensor_value_info('depth_meters',F,[1,h,w])]
        inits=[helper.make_tensor('idx',I,[],[0]),helper.make_tensor('width',F,[],[w])]
        nodes=[helper.make_node('Gather',['pixel_values','idx'],['inverse'],axis=1),helper.make_node('Mul',['inverse','width'],['denom']),helper.make_node('Div',['f_px','denom'],['depth_meters'])]
        if stream:
            inputs.append(helper.make_tensor_value_info('past_filled',F,[4]));outputs.append(helper.make_tensor_value_info('present_filled',F,[4]));nodes.append(helper.make_node('Identity',['past_filled'],['present_filled']))
        model=helper.make_model(helper.make_graph(nodes,'test',inputs,outputs,inits),opset_imports=[helper.make_opsetid('',18)])
        helper.set_model_props(model, {'depthpro2_onnx':json.dumps(dict(
            mode='stream',window=4,precision='fp16',width=w,height=h,focal_estimator_removed=True))})
        return model
    def test_stateless_or_other_window_rejected(self):
        with self.assertRaises(ValueError): adapt(self.make(stream=False),onnx)
        model=self.make()
        config=json.loads(model.metadata_props[0].value)
        config['window']=8
        model.metadata_props[0].value=json.dumps(config)
        with self.assertRaises(ValueError): adapt(model,onnx)
    def test_stream_adapter_preserves_state_and_depth(self):
        m=adapt(self.make(stream=True),onnx)
        r=inspect(m,onnx)
        self.assertEqual(r['states']['past_filled']['output'],'present_filled')
        self.assertEqual(r['state_bytes'],16)
        self.assertEqual(r['outputs']['depth'],(TensorProto.FLOAT,(1,1,192,352)))
        import numpy as np
        from onnx.reference import ReferenceEvaluator
        state=np.array([0,1,2,3],dtype=np.float32)
        rgb=np.random.default_rng(4).uniform(0.01,1,(1,3,192,352)).astype(np.float32)
        rgb[0,0,0,0]=0
        with np.errstate(divide='ignore'):
            values=ReferenceEvaluator(m).run(None,{'image':rgb,'past_filled':state})
        np.testing.assert_allclose(values[0],rgb[:,:1],rtol=3e-6,atol=1e-7)
        np.testing.assert_array_equal(values[1],state)
    def test_stream_pair_mismatch_rejected(self):
        m=self.make(stream=True)
        m.graph.output[1].type.tensor_type.shape.dim[0].dim_value=5
        with self.assertRaises(ValueError): adapt(m,onnx)
    def test_dml_reshape_preserves_shared_constant_and_results(self):
        import numpy as np
        from onnx.reference import ReferenceEvaluator
        F=TensorProto.FLOAT; I=TensorProto.INT64
        inputs=[helper.make_tensor_value_info('x',F,[2,3,4]),helper.make_tensor_value_info('z',F,[3,3,4])]
        outputs=[helper.make_tensor_value_info('a',F,[2,12]),helper.make_tensor_value_info('b',F,[3,12])]
        init=[helper.make_tensor('target',I,[2],[-1,12])]
        nodes=[helper.make_node('Reshape',['x','target'],['a'],allowzero=1,name='fix'),helper.make_node('Reshape',['z','target'],['b'],name='shared')]
        model=helper.make_model(helper.make_graph(nodes,'reshape',inputs,outputs,init),opset_imports=[helper.make_opsetid('',18)])
        data={'x':np.arange(24,dtype=np.float32).reshape(2,3,4),'z':np.arange(36,dtype=np.float32).reshape(3,3,4)}
        expected=ReferenceEvaluator(model).run(None,data)
        self.assertEqual(len(normalize_dml_reshape(model,onnx)),1)
        onnx.checker.check_model(model)
        self.assertEqual(model.graph.node[1].input[1],'target')
        self.assertEqual(len(model.graph.node[0].attribute),0)
        self.assertNotEqual(model.graph.node[0].input[1],'target')
        for a,b in zip(expected,ReferenceEvaluator(model).run(None,data)):
            np.testing.assert_array_equal(a,b)
    def test_dynamic_or_zero_target_untouched(self):
        F=TensorProto.FLOAT; I=TensorProto.INT64
        inputs=[helper.make_tensor_value_info('x',F,[0,3]),helper.make_tensor_value_info('target',I,[2])]
        outputs=[helper.make_tensor_value_info('a',F,[0,3])]
        model=helper.make_model(helper.make_graph([helper.make_node('Reshape',['x','target'],['a'],allowzero=1)],'dynamic',inputs,outputs),opset_imports=[helper.make_opsetid('',18)])
        before=model.SerializeToString()
        self.assertEqual(normalize_dml_reshape(model,onnx),[])
        self.assertEqual(model.SerializeToString(),before)
    def test_size_rejected(self):
        with self.assertRaises(ValueError): adapt(self.make(480,672),onnx)

class ReshapeMathTests(unittest.TestCase):
    def test_explicit_inferred_dimension(self):
        for batch in range(1,33):
            for channels in range(1,17):
                self.assertEqual(reshape_target((-1,channels*7),(batch,channels,7)),(batch,channels*7))
                self.assertEqual(reshape_target((batch,-1),(batch,channels,7)),(batch,channels*7))
    def test_no_inferred_or_zero_untouched(self):
        for shape in ((2,12),(0,12),(0,-1),(-1,-1),(-2,12),()):
            self.assertIsNone(reshape_target(shape,(2,3,4)))
    def test_symbolic_and_invalid(self):
        self.assertEqual(reshape_target((-1,12),(None,3,4)),(-1,12))
        with self.assertRaises(ValueError): reshape_target((-1,7),(2,3,4))

if __name__=='__main__': unittest.main()
