// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <objbase.h>
#include <DirectXPackedVector.h>
#include <json.hpp>

#include "SafetensorsDepth.h"
#include "ConverterResources.h"
#include "../../Source/Depth/DepthSplitPack.h"
#include "../../Source/Depth/DirectMLGraph/DmlGraphDesc_generated.h"

#include <bit>
#include <cstring>
#include <iostream>
#include <memory>
#include <set>
#include <streambuf>

namespace depthrecipe {
namespace {
namespace fs = std::filesystem;
using namespace DepthPack;
using View = std::span<const std::uint8_t>;
using Json = nlohmann::ordered_json;
using DirectX::PackedVector::XMConvertFloatToHalf;
using DirectX::PackedVector::XMConvertHalfToFloat;
constexpr char Metadata[] = "recipe.bin";
constexpr std::string_view GraphConstants[]{
    "mean.bin", "new_zeros.bin", "split_split_0.bin", "split_split_1.bin", "std.bin", "val_10.bin"
};

template<class Sink>
class OutputBuffer : public std::streambuf {
    Sink sink;
    std::streamsize xsputn(const char* data, std::streamsize size) override {
        const auto bytes = View(reinterpret_cast<const std::uint8_t*>(data), static_cast<size_t>(size));
        return sink(bytes) ? size : 0;
    }
    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) return traits_type::not_eof(value);
        const char byte = traits_type::to_char_type(value);
        return xsputn(&byte, 1) == 1 ? value : traits_type::eof();
    }
public:
    explicit OutputBuffer(Sink write) : sink(std::move(write)) {}
};

class Sha256 {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    template<class AddBytes>
    std::string Compute(AddBytes addBytes) const {
        std::array<UCHAR, 32> digest{};
        BCRYPT_HASH_HANDLE hash = nullptr;
        if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
            throw std::runtime_error("SHA-256 failed");
        const auto close = [](void* handle) { BCryptDestroyHash(handle); };
        const std::unique_ptr<void, decltype(close)> handle(hash, close);
        const auto update = [hash](View bytes) {
            return bytes.size() <= Limit &&
                BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) >= 0;
        };
        if (!addBytes(update) || BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
            throw std::runtime_error("SHA-256 failed");
        constexpr char hex[] = "0123456789abcdef";
        std::string result;
        result.reserve(digest.size() * 2);
        for (auto b : digest) {
            result += hex[b >> 4];
            result += hex[b & 15];
        }
        return result;
    }
public:
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            throw std::runtime_error("Cannot open Windows SHA-256 provider");
    }
    ~Sha256() { BCryptCloseAlgorithmProvider(algorithm, 0); }
    Sha256(const Sha256&) = delete;
    std::string operator()(View bytes) const {
        return Compute([bytes](auto update) { return update(bytes); });
    }
    std::string Pack(const Entries& entries) const {
        return Compute([&](auto update) {
            OutputBuffer buffer(update);
            std::ostream stream(&buffer);
            Write(stream, entries);
            return bool(stream);
        });
    }
};

struct Tensor {
    View bytes;
    std::vector<size_t> shape;
    size_t Count() const { return bytes.size() / sizeof(float); }
    float At(size_t index) const {
        float value;
        std::memcpy(&value, bytes.data() + index * sizeof(value), sizeof(value));
        return value;
    }
};

size_t ReadSize(const Json& value, size_t limit) {
    if (!value.is_number_unsigned() || value.get<std::uint64_t>() > limit)
        throw std::runtime_error("Invalid tensor extent");
    return value.get<size_t>();
}

class Checkpoint {
    Bytes bytes;
public:
    std::map<std::string, Tensor, std::less<>> tensors;
    std::string digest;
    Checkpoint(const fs::path& path, const Sha256& sha) : bytes(ReadFile(path)), digest(sha(bytes)) {}
    void ReadTensors() {
        if (bytes.size() < 9 || bytes[8] != '{') throw std::runtime_error("Invalid Safetensors header");
        std::uint64_t count;
        static_assert(std::endian::native == std::endian::little);
        std::memcpy(&count, bytes.data(), sizeof(count));
        if (count > 1024 * 1024 || count > bytes.size() - 8)
            throw std::runtime_error("Invalid Safetensors header length");
        const auto parts = Json::parse(bytes.begin() + 8, bytes.begin() + 8 + size_t(count));
        const auto payload = View(bytes).subspan(8 + size_t(count));
        if (!parts.is_object() || parts.size() > MaxEntries)
            throw std::runtime_error("Invalid Safetensors table");
        std::vector<std::pair<size_t, size_t>> ranges;
        for (const auto& [name, field] : parts.items()) {
            if (name == "__metadata__") continue;
            if (field.at("dtype") != "F32") throw std::runtime_error("Expected upstream F32 tensors");
            const auto& dimensions = field.at("shape");
            const auto& offsets = field.at("data_offsets");
            if (!dimensions.is_array() || dimensions.size() > 8 || !offsets.is_array() || offsets.size() != 2)
                throw std::runtime_error("Invalid Safetensors tensor");
            const size_t begin = ReadSize(offsets[0], payload.size()), end = ReadSize(offsets[1], payload.size());
            std::vector<size_t> shape;
            size_t elements = 1;
            for (const auto& d : dimensions) {
                const auto extent = ReadSize(d, Limit / sizeof(float));
                if (!extent || elements > Limit / sizeof(float) / extent)
                    throw std::runtime_error("Oversized Safetensors tensor");
                elements *= extent;
                shape.push_back(extent);
            }
            if (end < begin || end - begin != elements * sizeof(float))
                throw std::runtime_error("Safetensors data size does not match shape");
            ranges.emplace_back(begin, end);
            tensors.emplace(name, Tensor{payload.subspan(begin, end - begin), std::move(shape)});
        }
        std::ranges::sort(ranges);
        size_t position = 0;
        for (const auto& [begin, end] : ranges) {
            if (begin != position) throw std::runtime_error("Overlapping or non-contiguous tensor data");
            position = end;
        }
        if (tensors.empty() || position != payload.size()) throw std::runtime_error("Incomplete Safetensors payload");
    }
};

struct Rule {
    std::string source, operation, argument;
    bool fp32 = false;
    unsigned part = 0;
};

void Append(Bytes& out, const auto value) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), data, data + sizeof(value));
}

Bytes Transform(const Checkpoint& checkpoint, const Rule& rule) {
    const auto& input = checkpoint.tensors.at(rule.source);
    const bool transpose = rule.operation == "transpose" || rule.operation == "slice-transpose";
    const bool slice = rule.operation == "slice-transpose";
    const bool concatenate = rule.operation == "concat";
    const bool scale = rule.operation == "scale";
    const bool round = rule.operation == "round";
    if (!transpose && !concatenate && !scale && !round && rule.operation != "copy")
        throw std::runtime_error("Unknown checkpoint tensor transform");
    if (transpose && input.shape.size() != 2)
        throw std::runtime_error("Expected a matrix to transpose");
    const size_t rows = transpose ? input.shape[0] / (slice ? 3 : 1) : 0;
    const size_t columns = transpose ? input.shape[1] : 0;
    if (slice && (input.shape[0] % 3 || rule.part >= 3)) throw std::runtime_error("Invalid QKV slice");
    const auto* other = concatenate || scale ? &checkpoint.tensors.at(rule.argument) : nullptr;
    if (scale && (input.shape.empty() || other->Count() != input.shape[0]))
        throw std::runtime_error("Mismatched channel scale");
    const size_t elements = slice ? input.Count() / 3 : input.Count();
    const size_t rowSize = scale ? input.Count() / input.shape[0] : 0;
    Bytes result;
    result.reserve((elements + (concatenate ? other->Count() : 0)) * (rule.fp32 ? 4 : 2));
    for (size_t dst = 0; dst < elements; ++dst) {
        const size_t src = transpose ? (dst % rows + rule.part * rows) * columns + dst / rows : dst;
        const float value = input.At(src);
        if (rule.fp32) {
            // The exporter first calls model.half(), then promotes selected
            // branches. Preserve those widened values and FP32 binding types.
            Append(result, round || transpose ? XMConvertHalfToFloat(XMConvertFloatToHalf(value)) : value);
        } else if (scale) {
            const float a = XMConvertHalfToFloat(XMConvertFloatToHalf(value));
            const float b = XMConvertHalfToFloat(XMConvertFloatToHalf(other->At(src / rowSize)));
            Append(result, XMConvertFloatToHalf(a * b));
        } else Append(result, XMConvertFloatToHalf(value));
    }
    if (concatenate) {
        for (size_t i = 0; i < other->Count(); ++i) Append(result, XMConvertFloatToHalf(other->At(i)));
    }
    return result;
}

std::set<std::string> GraphBindings(View graph) {
    flatbuffers::Verifier verifier(graph.data(), graph.size());
    if (!dml::ir::VerifyDmlGraphDescBuffer(verifier))
        throw std::runtime_error("Invalid DirectML graph in recipe");
    const auto* model = dml::ir::GetDmlGraphDesc(graph.data());
    if (!model->nodes()) throw std::runtime_error("Expected DirectML graph nodes");
    std::set<std::string> names;
    size_t inlineCount = 0, inlineBytes = 0;
    for (const auto* node : *model->nodes()) {
        const auto* constant = node->desc_as_ConstantNodeDesc();
        if (!constant) continue;
        if (const auto* binding = constant->data_as_ConstantName(); binding && binding->name()) {
            auto name = binding->name()->str() + ".bin";
            if (!ValidName(name) || name == Metadata || name == GraphFile)
                throw std::runtime_error("Invalid DirectML binding name");
            names.insert(std::move(name));
        } else if (const auto* raw = constant->data_as_ConstantRawData(); raw && raw->data()) {
            if (raw->data()->size() > 8)
                throw std::runtime_error("Unexpected inline tensor; audit exporter changes before embedding");
            ++inlineCount;
            inlineBytes += raw->data()->size();
        } else throw std::runtime_error("Invalid DirectML constant");
    }
    // The validated export externalizes all learned tensors. Report inline
    // auxiliary data so a future exporter change is visible during regeneration.
    std::cout << "Graph: " << names.size() << " external bindings, "
        << inlineCount << " inline constants (" << inlineBytes << " bytes).\n";
    return names;
}

std::string ScaleName(const std::string& name) {
    if (const auto p = name.find(".attn.proj."); p != std::string::npos) return name.substr(0, p) + ".gamma_1";
    if (const auto p = name.find(".mlp.fc2."); p != std::string::npos) return name.substr(0, p) + ".gamma_2";
    return {};
}

Json AddMappings(const Checkpoint& checkpoint, const Entries& expected, const Sha256& sha) {
    std::map<std::string, std::string> targetByHash;
    for (const auto& [name, bytes] : expected) {
        if (!targetByHash.emplace(sha(bytes), name).second)
            throw std::runtime_error("Ambiguous reference tensors; audit exporter changes");
    }
    std::map<std::string, Json> mapped;
    const auto tryRule = [&](const Rule& rule) {
        const auto bytes = Transform(checkpoint, rule);
        const auto digest = sha(bytes);
        const auto target = targetByHash.find(digest);
        if (target == targetByHash.end()) return;
        if (!std::ranges::equal(expected.at(target->second), bytes))
            throw std::runtime_error("Unexpected tensor digest collision");
        mapped.try_emplace(target->second, Json{{"output", target->second}, {"op", rule.operation}, {"source", rule.source},
            {"arg", rule.argument}, {"fp32", rule.fp32}, {"part", rule.part}, {"sha256", digest}});
        targetByHash.erase(target);
    };
    for (const auto& [name, tensor] : checkpoint.tensors) {
        tryRule({name, "copy", {}, false});
        tryRule({name, "copy", {}, true});
        tryRule({name, "round", {}, true});
        if (tensor.shape.size() == 2) {
            tryRule({name, "transpose", {}, false});
            tryRule({name, "transpose", {}, true});
            if (name.ends_with(".qkv.weight") && tensor.shape[0] % 3 == 0) {
                for (unsigned part = 0; part < 3; ++part) {
                    tryRule({name, "slice-transpose", {}, false, part});
                    tryRule({name, "slice-transpose", {}, true, part});
                }
            }
        }
        if (const auto scale = ScaleName(name); !scale.empty() && checkpoint.tensors.contains(scale))
            tryRule({name, "scale", scale});
    }
    if (checkpoint.tensors.contains("encoder.cls_token") && checkpoint.tensors.contains("encoder.reg_token"))
        tryRule({"encoder.cls_token", "concat", "encoder.reg_token"});

    if (!targetByHash.empty()) {
        for (const auto& [digest, name] : targetByHash) std::cerr << "Unmapped: " << name << '\n';
        throw std::runtime_error("Incomplete Safetensors mapping; no recipe written");
    }
    Json records = Json::array();
    for (auto& [name, record] : mapped) records.push_back(std::move(record));
    return records;
}

View EmbeddedRecipe() {
    const auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(DEPTH_SAFETENSORS_RECIPE), RT_RCDATA);
    if (!resource) throw std::runtime_error("This converter was built without the depth recipe");
    const auto handle = LoadResource(nullptr, resource);
    const auto* data = static_cast<const std::uint8_t*>(LockResource(handle));
    if (!data) throw std::runtime_error("Cannot read the embedded depth recipe");
    return {data, SizeofResource(nullptr, resource)};
}

void WriteNew(const fs::path& path, const Entries& entries, const depthsplit::PairId* id = nullptr, bool weights = false) {
    const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Output must be a new file in an existing directory");
    try {
        OutputBuffer buffer([file](View data) {
            DWORD written = 0;
            const auto size = static_cast<DWORD>(data.size());
            return WriteFile(file, data.data(), size, &written, nullptr) && written == size;
        });
        std::ostream stream(&buffer);
        if (id) depthsplit::WritePart(stream, entries, *id, weights);
        else Write(stream, entries);
        if (!stream || !FlushFileBuffers(file)) throw std::runtime_error("Cannot write new depth resource");
    } catch (...) {
        CloseHandle(file);
        DeleteFileW(path.c_str());
        throw;
    }
    CloseHandle(file);
}
}

void Generate(const fs::path& checkpointPath, const fs::path& config, const fs::path& pack, const fs::path& recipe) {
    const Sha256 sha;
    Checkpoint checkpoint(checkpointPath, sha);
    checkpoint.ReadTensors();
    const auto packData = ReadFile(pack);
    const auto expected = Read(packData);
    const auto graph = expected.at(GraphFile);
    auto bindings = GraphBindings(graph);
    Entries structural{{GraphFile, graph}}, learned;
    for (const auto& [name, bytes] : expected) {
        if (name == GraphFile) continue;
        if (bindings.erase(name) != 1) throw std::runtime_error("Unexpected reference binding");
        (std::ranges::find(GraphConstants, name) != std::end(GraphConstants) ? structural : learned).emplace(name, bytes);
    }
    if (!bindings.empty() || structural.size() != std::size(GraphConstants) + 1)
        throw std::runtime_error("Reference constants or graph bindings changed");
    const auto rules = AddMappings(checkpoint, learned, sha);
    const auto metadata = Json{{"format", 1}, {"safetensors", checkpoint.digest}, {"config", sha(ReadFile(config, 65536))},
        {"reference_pack", sha(packData)}, {"weights", rules}}.dump(2);
    structural.emplace(Metadata, View(reinterpret_cast<const std::uint8_t*>(metadata.data()), metadata.size()));
    WriteNew(recipe, structural);
    std::cout << "Recipe generated: " << rules.size() << " reconstructed tensors.\n";
}

void Convert(const fs::path& checkpointPath, const fs::path& config, const fs::path& graph, const fs::path& weights) {
    if (fs::absolute(graph).parent_path() != fs::absolute(weights).parent_path() ||
        _wcsicmp(graph.extension().c_str(), L".dmlgraph") || _wcsicmp(weights.extension().c_str(), L".dmlweights"))
        throw std::runtime_error("Expected new .dmlgraph and .dmlweights files in one directory");
    const Sha256 sha;
    auto structural = Read(EmbeddedRecipe());
    const auto meta = structural.at(Metadata);
    const auto metadata = Json::parse(meta.begin(), meta.end());
    structural.erase(Metadata);
    const auto graphBytes = structural.at(GraphFile);
    auto bindings = GraphBindings(graphBytes);
    Checkpoint checkpoint(checkpointPath, sha);
    if (metadata.at("format").get<int>() != 1 || metadata.at("safetensors").get<std::string>() != checkpoint.digest ||
        metadata.at("config").get<std::string>() != sha(ReadFile(config, 65536)))
        throw std::runtime_error("Checkpoint/config SHA-256 does not match the pinned graph recipe");
    checkpoint.ReadTensors();
    std::map<std::string, Bytes> buffers;
    for (const auto& row : metadata.at("weights")) {
        const auto name = row.at("output").get<std::string>();
        const Rule rule{row.at("source"), row.at("op"), row.at("arg"), row.at("fp32"), row.at("part")};
        auto bytes = Transform(checkpoint, rule);
        if (sha(bytes) != row.at("sha256").get<std::string>() || !buffers.emplace(name, std::move(bytes)).second)
            throw std::runtime_error("Reconstructed tensor differs from the reference: " + name);
    }
    Entries all = structural;
    for (const auto& [name, bytes] : buffers) {
        if (!all.emplace(name, bytes).second) throw std::runtime_error("Duplicate depth tensor");
    }
    for (const auto& [name, bytes] : all) {
        if (name != GraphFile && bindings.erase(name) != 1) throw std::runtime_error("Unexpected depth tensor");
    }
    // Same serializer as the validated ORT workflow. This byte-for-byte gate
    // includes graph constants, tensor types/layouts and every learned byte.
    if (!bindings.empty() || sha.Pack(all) != metadata.at("reference_pack").get<std::string>())
        throw std::runtime_error("Native pack differs from the validated DirectML reference");
    const auto parts = depthsplit::Partition(all);
    depthsplit::PairId id{};
    const auto digest = sha(EmbeddedRecipe());
    for (size_t i = 0; i < id.size(); ++i) id[i] = static_cast<std::uint8_t>(std::stoi(digest.substr(i * 2, 2), nullptr, 16));
    WriteNew(graph, parts.graph, &id);
    try { WriteNew(weights, parts.weights, &id, true); }
    catch (...) { DeleteFileW(graph.c_str()); throw; }
    std::cout << "[OK] Graph and " << buffers.size() << " reconstructed tensors match the validated DirectML reference.\n";
}
}
