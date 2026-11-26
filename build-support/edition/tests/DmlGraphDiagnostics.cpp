// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the actual converter diagnostic with a serialized graph fixture.
#include "../../depth/DmlGraphDiagnostics.h"
#include <cassert>
#include <iostream>
#include <vector>

using namespace dml::ir;
enum class Attribute { UntypedNull, TypedNull, Present, RequiredNull };
std::vector<uint8_t> Fixture(Attribute kind = Attribute::UntypedNull) {
    flatbuffers::FlatBufferBuilder f;
    auto type = f.CreateString("DML_OPERATOR_ELEMENT_WISE_IDENTITY");
    auto dtype = f.CreateString("FLOAT32");
    auto dims = f.CreateVector(std::vector<uint32_t>{1, 4});
    auto tensor = CreateDmlBufferTensorDesc(f, dtype, dims, 0, 16);
    auto inputs = f.CreateVector(std::vector{tensor});
    auto outputs = f.CreateVector(std::vector{tensor});
    auto attrName = f.CreateString("ScaleBias");
    using namespace operatorFieldTypes;
    flatbuffers::Offset<AttributeDesc> attr;
    switch (kind) {
    case Attribute::UntypedNull:
        attr = CreateAttributeDesc(f, attrName);
        break;
    case Attribute::TypedNull:
        attr = CreateAttributeDesc(f, 0, AttributeFieldVariant_ScaleBias, 0);
        break;
    case Attribute::Present: {
        auto value = f.CreateStruct(ScaleBias(1.25f, -0.5f));
        attr = CreateAttributeDesc(f, attrName, AttributeFieldVariant_ScaleBias, value.Union());
        break;
    }
    case Attribute::RequiredNull:
        attr = CreateAttributeDesc(f, attrName, AttributeFieldVariant_UInt32, 0);
        break;
    }
    auto attrs = f.CreateVector(std::vector{attr});
    auto op = CreateOperatorNodeDesc(f, type, inputs, outputs, attrs);
    auto name = f.CreateString("test_node");
    auto inputName = f.CreateString("image");
    auto outputName = f.CreateString("depth");
    auto inNames = f.CreateVector(std::vector{inputName});
    auto outNames = f.CreateVector(std::vector{outputName});
    auto node = CreateDmlGraphNode(f, NodeDesc_OperatorNodeDesc, op.Union(), name, inNames, outNames);
    auto nodes = f.CreateVector(std::vector{node});
    auto root = CreateDmlGraphDesc(f, nodes, inNames, outNames);
    FinishDmlGraphDescBuffer(f, root);
    return {f.GetBufferPointer(), f.GetBufferPointer() + f.GetSize()};
}
bool Valid(const std::vector<uint8_t>& bytes) {
    flatbuffers::Verifier v(bytes.data(), bytes.size());
    return VerifyDmlGraphDescBuffer(v);
}
void BreakString(std::vector<uint8_t>& bytes, const flatbuffers::String* str) {
    auto offset = reinterpret_cast<const uint8_t*>(str->c_str()) - bytes.data();
    bytes.at(offset + str->size()) = 'X';
}
void Expect(const std::vector<uint8_t>& bytes, const std::string& needle) {
    assert(!Valid(bytes));
    auto result = dmlgraphdiag::DescribeFailure(bytes);
    std::cout << result << '\n';
    assert(result.find(needle) != std::string::npos);
}
int main() {
    auto bytes = Fixture();
    auto typedNull = Fixture(Attribute::TypedNull);
    std::cout << "Typed-null ScaleBias: " << Valid(typedNull) << std::endl;
    assert(Valid(typedNull));
    const auto* missing = GetDmlGraphDesc(typedNull.data())->nodes()->Get(0)->desc_as_OperatorNodeDesc()->attributes()->Get(0);
    assert(missing->val_type() == operatorFieldTypes::AttributeFieldVariant_ScaleBias);
    assert(missing->val() == nullptr && missing->val_as_ScaleBias() == nullptr);
    auto present = Fixture(Attribute::Present);
    assert(Valid(present));
    const auto* attr = GetDmlGraphDesc(present.data())->nodes()->Get(0)->desc_as_OperatorNodeDesc()->attributes()->Get(0);
    assert(attr->val_as_ScaleBias()->scale() == 1.25f && attr->val_as_ScaleBias()->bias() == -0.5f);
    // An actual offset field is not optional merely because the union's type is ScaleBias.
    auto corrupt = present;
    auto* table = reinterpret_cast<const flatbuffers::Table*>(GetDmlGraphDesc(corrupt.data())->nodes()->Get(0)->desc_as_OperatorNodeDesc()->attributes()->Get(0));
    const auto off = table->GetOptionalFieldOffset(operatorFieldTypes::AttributeDesc::VT_VAL);
    assert(off);
    const auto pos = reinterpret_cast<const uint8_t*>(table) - corrupt.data() + off;
    const auto validOffset = flatbuffers::ReadScalar<flatbuffers::uoffset_t>(corrupt.data() + pos);
    flatbuffers::WriteScalar<flatbuffers::uoffset_t>(corrupt.data() + pos, validOffset + 1);
    assert(!Valid(corrupt));
    flatbuffers::WriteScalar<flatbuffers::uoffset_t>(corrupt.data() + pos, 0);
    assert(!Valid(corrupt));
    flatbuffers::WriteScalar<flatbuffers::uoffset_t>(corrupt.data() + pos, 0x7ffffffc);
    assert(!Valid(corrupt));
    auto requiredNull = Fixture(Attribute::RequiredNull);
    assert(!Valid(requiredNull));
    assert(dmlgraphdiag::DescribeFailure(requiredNull).find("tag=3 UInt32, payload=absent") != std::string::npos);
    assert(Valid(bytes));
    assert(dmlgraphdiag::DescribeFailure(bytes).starts_with("individual probes passed"));
    auto b = bytes;
    BreakString(b, GetDmlGraphDesc(b.data())->nodes()->Get(0)->desc_as_OperatorNodeDesc()->type());
    Expect(b, ".operator.type");
    b = bytes;
    BreakString(b, GetDmlGraphDesc(b.data())->nodes()->Get(0)->desc_as_OperatorNodeDesc()->attributes()->Get(0)->name());
    Expect(b, ".attributes[0].name");
    b = bytes;
    BreakString(b, GetDmlGraphDesc(b.data())->nodes()->Get(0)->desc_as_OperatorNodeDesc()->inputs()->Get(0)->dataType());
    Expect(b, ".inputs[0]");
    b = bytes;
    // Corrupt an element offset inside a graph-name vector, but not any other data.
    const auto* names = GetDmlGraphDesc(b.data())->graphInputNames();
    auto element = names->Data() - b.data();
    flatbuffers::WriteScalar<flatbuffers::uoffset_t>(b.data()+element, 0);
    assert(dmlgraphdiag::DescribeFailure(b).find("inputNames[0]: invalid vector element offset") != std::string::npos);
    b = bytes;
    flatbuffers::WriteScalar<flatbuffers::uoffset_t>(b.data(), 0);
    Expect(b, "root: invalid FlatBuffers root offset");
    std::cout << "DML graph diagnostic fixtures PASS\n";
}
