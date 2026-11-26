// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../Source/Depth/DepthPack.h"
#include "../../Source/Depth/DepthStreamContract.h"
#include <json.hpp>
#include <bit>
#include <cstring>
#include <set>

namespace {
using namespace DepthPack;
using View = std::span<const std::uint8_t>;
using Shape = std::vector<std::int64_t>;

// Edit only the known fields of ONNX Model/Graph/Node/Tensor/ValueInfo.
// Other wire fields are carried verbatim. No generic protobuf dependency.
std::uint64_t Var(View& bytes) {
    std::uint64_t value = 0;
    for (unsigned bit = 0; bit <= 63 && !bytes.empty(); bit += 7) {
        const auto byte = bytes.front();
        bytes = bytes.subspan(1);
        if (bit == 63 && byte > 1) break;
        value |= std::uint64_t(byte & 127) << bit;
        if (!(byte & 128)) return value;
    }
    throw std::runtime_error("Invalid ONNX varint");
}
struct Field { unsigned number, wire; View encoded, payload; std::uint64_t value = 0; };
std::vector<Field> Fields(View remaining) {
    std::vector<Field> result;
    while (!remaining.empty()) {
        const auto original = remaining;
        const auto key = Var(remaining);
        if (key < 8 || key >> 3 >= (1u << 29)) throw std::runtime_error("Invalid ONNX field");
        Field field{unsigned(key >> 3), unsigned(key & 7), {}, {}};
        if (field.wire == 0) field.value = Var(remaining);
        else {
            std::uint64_t size;
            switch (field.wire) {
            case 1: size = 8; break;
            case 2: size = Var(remaining); break;
            case 5: size = 4; break;
            default: throw std::runtime_error("Unsupported ONNX wire field");
            }
            if (size > remaining.size()) throw std::runtime_error("Truncated ONNX field");
            field.payload = remaining.first(size_t(size));
            remaining = remaining.subspan(size_t(size));
        }
        field.encoded = original.first(original.size() - remaining.size());
        result.push_back(field);
    }
    return result;
}
std::string_view Text(View data) {
    return data.empty() ? std::string_view{} : std::string_view(reinterpret_cast<const char*>(data.data()), data.size());
}
Field Find(const std::vector<Field>& fields, unsigned number, unsigned wire = 2) {
    for (const auto& f : fields) if (f.number == number && f.wire == wire) return f;
    return {};
}
void Append(Bytes& out, View bytes) { out.insert(out.end(), bytes.begin(), bytes.end()); }
void Var(Bytes& out, std::uint64_t value) {
    while (value >= 128) { out.push_back(std::uint8_t(value) | 128); value >>= 7; }
    out.push_back(std::uint8_t(value));
}
void Integer(Bytes& out, unsigned number, std::uint64_t value) {
    Var(out, std::uint64_t(number) << 3);
    Var(out, value);
}
void Blob(Bytes& out, unsigned number, View bytes) {
    Var(out, (std::uint64_t(number) << 3) | 2);
    Var(out, bytes.size());
    Append(out, bytes);
}
void Text(Bytes& out, unsigned number, std::string_view value) {
    Blob(out, number, {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}
void RepeatedInts(Shape& shape, const Field& field) {
    if (field.wire == 0) shape.push_back(std::bit_cast<std::int64_t>(field.value));
    else if (field.wire == 2) {
        auto bytes = field.payload;
        while (!bytes.empty()) shape.push_back(std::bit_cast<std::int64_t>(Var(bytes)));
    }
}
Shape IntVector(const std::vector<Field>& tensor) {
    Shape dims, values;
    for (const auto& field : tensor) if (field.number == 1) RepeatedInts(dims, field);
    if (Find(tensor, 2, 0).value != 7 || dims.size() != 1) return {};
    for (const auto& field : tensor) {
        if (field.number == 7) RepeatedInts(values, field);
        if (field.number == 9 && field.wire == 2) {
            static_assert(std::endian::native == std::endian::little);
            if (field.payload.size() % sizeof(std::int64_t)) throw std::runtime_error("Invalid INT64 ONNX initializer");
            values.resize(field.payload.size() / sizeof(std::int64_t));
            if (!values.empty()) std::memcpy(values.data(), field.payload.data(), field.payload.size());
        }
    }
    return dims.front() == std::int64_t(values.size()) ? values : Shape{};
}
struct TensorType {
    unsigned type;
    Shape shape;
    bool operator==(const TensorType&) const = default;
};
TensorType TypeOf(const std::vector<Field>& info) {
    const auto tensor = Fields(Find(Fields(Find(info, 2).payload), 1).payload);
    TensorType result{unsigned(Find(tensor, 1, 0).value), {}};
    for (const auto& dimension : Fields(Find(tensor, 2).payload)) {
        if (dimension.number == 1)
            result.shape.push_back(std::int64_t(Find(Fields(dimension.payload), 1, 0).value));
    }
    return result;
}
Bytes RenameValue(const std::vector<Field>& value, std::string_view name) {
    Bytes result;
    for (const auto& field : value) {
        if (field.number == 1) Text(result, 1, name);
        else Append(result, field.encoded);
    }
    return result;
}
Bytes DepthOutput() {
    Bytes shape, tensor, type, value;
    for (const auto extent : {1u, 1u, Height, Width}) {
        Bytes dim;
        Integer(dim, 1, extent);
        Blob(shape, 1, dim);
    }
    Integer(tensor, 1, 1);
    Blob(tensor, 2, shape);
    Blob(type, 1, tensor);
    Text(value, 1, "depth");
    Blob(value, 2, type);
    return value;
}
Bytes IntTensor(std::string_view name, const Shape& shape) {
    Bytes tensor, dims, values;
    Var(dims, shape.size());
    Blob(tensor, 1, dims);
    Integer(tensor, 2, 7);
    for (const auto d : shape) Var(values, std::uint64_t(d));
    Blob(tensor, 7, values);
    Text(tensor, 8, name);
    return tensor;
}
Bytes FocalTensor() {
    Bytes tensor, dims, value;
    Var(dims, 1);
    Blob(tensor, 1, dims);
    Integer(tensor, 2, 1);
    const auto bits = std::bit_cast<std::uint32_t>(float(Width));
    for (unsigned bit = 0; bit < 32; bit += 8) value.push_back(std::uint8_t(bits >> bit));
    Blob(tensor, 4, value);
    Text(tensor, 8, "f_px");
    return tensor;
}
std::uint64_t Elements(const Shape& shape) {
    std::uint64_t result = 1;
    for (auto d : shape) {
        if (d <= 0 || std::uint64_t(d) > UINT64_MAX / result)
            throw std::runtime_error("Invalid ONNX tensor shape");
        result *= std::uint64_t(d);
    }
    return result;
}
Bytes Node(std::string_view op, std::string_view name, std::string_view output,
    std::initializer_list<std::string_view> inputs) {
    Bytes node;
    for (auto input : inputs) Text(node, 1, input);
    Text(node, 2, output);
    Text(node, 3, name);
    Text(node, 4, op);
    return node;
}

Bytes AdaptGraph(View source) {
    auto fields = Fields(source);
    std::map<std::string_view, TensorType> types, inputs, outputs;
    std::map<std::string_view, Shape> constants;
    std::set<std::string, std::less<>> names;
    for (const auto& field : fields) {
        if (field.number == 5) {
            const auto tensor = Fields(field.payload);
            const auto name = Text(Find(tensor, 8).payload);
            if (name == "f_px" || Find(tensor, 13).number || Find(tensor, 14, 0).value)
                throw std::runtime_error("Expected self-contained upstream ONNX");
            names.emplace(name);
            auto ints = IntVector(tensor);
            if (!ints.empty()) constants.emplace(name, std::move(ints));
        } else if (field.number == 11 || field.number == 12 || field.number == 13) {
            const auto value = Fields(field.payload);
            const auto name = Text(Find(value, 1).payload);
            const auto type = TypeOf(value);
            names.emplace(name);
            types.emplace(name, type);
            if (field.number == 11) inputs.emplace(name, type);
            if (field.number == 12) outputs.emplace(name, type);
        } else if (field.number == 1) {
            const auto node = Fields(field.payload);
            const auto domain = Text(Find(node, 7).payload);
            const bool constant = (domain.empty() || domain == "ai.onnx") && Text(Find(node, 4).payload) == "Constant";
            for (const auto& f : node) {
                if (f.number == 1 || f.number == 2) names.emplace(Text(f.payload));
                if (f.number != 5) continue;
                const auto attr = Fields(f.payload);
                if (Find(attr, 6).number || Find(attr, 11).number)
                    throw std::runtime_error("Unexpected nested ONNX graph");
                if (!constant) continue;
                const auto attrName = Text(Find(attr, 1).payload);
                auto list = attrName == "value" ? IntVector(Fields(Find(attr, 5).payload)) : Shape{};
                if (attrName == "value_ints") for (const auto& v : attr) if (v.number == 8) RepeatedInts(list, v);
                if (!list.empty()) constants.emplace(Text(Find(node, 2).payload), std::move(list));
            }
        }
    }
    if (inputs.at("pixel_values") != TensorType{1, {1, 3, Height, Width}} ||
        inputs.at("f_px") != TensorType{1, {1}} || outputs.at("depth_meters") != TensorType{1, {1, Height, Width}} ||
        inputs.size() != 15 || outputs.size() != 14)
        throw std::runtime_error("Unexpected focal-free W4 stream contract");
    std::uint64_t stateBytes = 0;
    for (const auto& [name, type] : inputs) {
        if (name == "pixel_values" || name == "f_px") continue;
        const auto elements = Elements(type.shape);
        const unsigned bytes = type.type == 10 ? 2 : 4;
        if (!depthstream::IsPast(name) || outputs.at(depthstream::Present(name)) != type ||
            (type.type != 1 && type.type != 10) || elements > (depthstream::MaxStateBytes - stateBytes) / bytes)
            throw std::runtime_error("Invalid W4 state");
        stateBytes += elements * bytes;
        constants.erase(name);
    }
    for (auto name : {"image", "depth", "edition_dp2_inverse", "edition_dp2_axes"})
        if (names.contains(name)) throw std::runtime_error("Reserved ONNX tensor collision");

    Bytes result;
    std::vector<Bytes> fixedShapes;
    unsigned index = 0;
    for (const auto& field : fields) {
        if (field.number == 11 || field.number == 12 || field.number == 13) {
            const auto value = Fields(field.payload);
            const auto name = Text(Find(value, 1).payload);
            if (field.number == 11 && name == "f_px") continue;
            if (field.number == 12 && name == "depth_meters") Blob(result, 12, DepthOutput());
            else if (name == "pixel_values") Blob(result, field.number, RenameValue(value, "image"));
            else Append(result, field.encoded);
        } else if (field.number == 1) {
            const auto node = Fields(field.payload);
            const auto domain = Text(Find(node, 7).payload);
            bool allowzero = false;
            std::string replacement;
            std::vector<View> args;
            for (const auto& f : node) if (f.number == 1) args.push_back(f.payload);
            if ((domain.empty() || domain == "ai.onnx") && Text(Find(node, 4).payload) == "Reshape" && args.size() == 2) {
                for (const auto& f : node) if (f.number == 5) {
                    const auto attr = Fields(f.payload);
                    allowzero |= Text(Find(attr, 1).payload) == "allowzero" && Find(attr, 3, 0).value == 1;
                }
                if (allowzero) {
                    const auto target = constants.find(Text(args[1]));
                    allowzero = target != constants.end() && std::ranges::count(target->second, -1) == 1 &&
                        std::ranges::all_of(target->second, [](auto d) { return d == -1 || d > 0; });
                    const auto input = types.find(Text(args[0]));
                    if (allowzero && input != types.end() && !input->second.shape.empty() &&
                        std::ranges::all_of(input->second.shape, [](auto d) { return d > 0; })) {
                        auto dims = target->second;
                        auto unknown = std::ranges::find(dims, -1);
                        *unknown = 1;
                        const auto total = Elements(input->second.shape), known = Elements(dims);
                        if (total % known) throw std::runtime_error("Reshape element count mismatch");
                        *unknown = std::int64_t(total / known);
                        replacement = "edition_dp2_reshape_" + std::to_string(index);
                        if (!names.emplace(replacement).second) throw std::runtime_error("Reshape initializer name collision");
                        fixedShapes.push_back(IntTensor(replacement, dims));
                    }
                }
            }
            Bytes updated;
            unsigned arg = 0;
            for (const auto& f : node) {
                if (f.number == 1) {
                    ++arg;
                    if (arg == 2 && !replacement.empty()) { Text(updated, 1, replacement); continue; }
                    if (Text(f.payload) == "pixel_values") { Text(updated, 1, "image"); continue; }
                } else if (f.number == 5 && allowzero && Text(Find(Fields(f.payload), 1).payload) == "allowzero") continue;
                Append(updated, f.encoded);
            }
            Blob(result, 1, updated);
            ++index;
        } else Append(result, field.encoded);
    }
    Blob(result, 1, Node("Reciprocal", "EditionDP2Inverse", "edition_dp2_inverse", {"depth_meters"}));
    Blob(result, 1, Node("Unsqueeze", "EditionDP2NCHW", "depth", {"edition_dp2_inverse", "edition_dp2_axes"}));
    Blob(result, 5, FocalTensor());
    Blob(result, 5, IntTensor("edition_dp2_axes", {1}));
    for (const auto& tensor : fixedShapes) Blob(result, 5, tensor);
    return result;
}
}

void AdaptDepthOnnx(const std::filesystem::path& input, const std::filesystem::path& output) {
    const auto bytes = DepthPack::ReadFile(input);
    const auto model = Fields(bytes);
    bool valid = false, opset = false;
    for (const auto& f : model) {
        if (f.number == 8) {
            const auto version = Fields(f.payload);
            const auto domain = Text(Find(version, 1).payload);
            opset |= (domain.empty() || domain == "ai.onnx") && Find(version, 2, 0).value >= 13;
        } else if (f.number == 14) {
            const auto property = Fields(f.payload);
            if (Text(Find(property, 1).payload) != "depthpro2_onnx") continue;
            const auto metadata = nlohmann::json::parse(Text(Find(property, 2).payload));
            valid = metadata.at("mode") == "stream" && metadata.at("precision") == "fp16" &&
                metadata.at("window") == 4 && metadata.at("focal_estimator_removed") == true &&
                metadata.at("width") == DepthPack::Width && metadata.at("height") == DepthPack::Height;
        }
    }
    if (!valid || !opset || std::ranges::count(model, 7u, &Field::number) != 1)
        throw std::runtime_error("Expected the upstream 352x192 W4 FP16 ONNX export");
    Bytes result;
    for (const auto& f : model) {
        if (f.number == 7) Blob(result, 7, AdaptGraph(f.payload));
        else Append(result, f.encoded);
    }
    Bytes metadata;
    Text(metadata, 1, "edition_depth_adapter");
    Text(metadata, 2, "inverse-depth-stream-v1; focal fixed to width; research only");
    Blob(result, 14, metadata);
    std::ofstream out(output, std::ios::binary);
    out.write(reinterpret_cast<const char*>(result.data()), std::streamsize(result.size()));
    out.close();
    if (!out) throw std::runtime_error("Cannot write adapted ONNX");
}
