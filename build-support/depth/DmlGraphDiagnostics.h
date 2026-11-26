// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../../Source/Depth/DirectMLGraph/DmlGraphDesc_generated.h"
#include <span>
#include <string>

// Diagnostics only, for a converter-exported graph which the stock verifier
// has already rejected. Never substitute these probes for the full verifier.
namespace dmlgraphdiag {
class Probe {
    using U = flatbuffers::uoffset_t;
    using V = flatbuffers::voffset_t;
    using Table = flatbuffers::Table;
    std::span<const uint8_t> bytes;
    flatbuffers::Verifier verifier;
    std::string error;

    bool Fail(const std::string& path, const char* why) {
        error = path + ": " + why;
        return false;
    }
    bool Begin(const void* obj, const std::string& path) {
        if (!obj) return Fail(path, "missing table");
        auto* table = reinterpret_cast<const Table*>(obj);
        if (!table->VerifyTableStart(verifier)) return Fail(path, "invalid table/vtable or verifier complexity limit");
        return true;
    }
    template<class P, class T>
    bool Pointer(const T* obj, V field, P& result, const std::string& path) {
        auto* table = reinterpret_cast<const Table*>(obj);
        if (!table->VerifyOffset(verifier, field)) return Fail(path, "invalid field offset");
        result = table->template GetPointer<P>(field);
        return true;
    }
    template<class T>
    bool String(const T* obj, V field, const std::string& path, std::string* value = nullptr) {
        const flatbuffers::String* s = nullptr;
        if (!Pointer(obj, field, s, path)) return false;
        if (!verifier.VerifyString(s)) return Fail(path, "invalid string or terminator");
        if (value && s) *value = s->str();
        return true;
    }
    template<class T, class Owner, class Check>
    bool Vector(const Owner* obj, V field, const std::string& path, Check check) {
        const flatbuffers::Vector<flatbuffers::Offset<T>>* v = nullptr;
        if (!Pointer(obj, field, v, path)) return false;
        if (!verifier.VerifyVector(v)) return Fail(path, "invalid vector length/range");
        if (!v) return true;
        for (U i = 0; i < v->size(); ++i) {
            const auto element = static_cast<size_t>(v->Data() - bytes.data()) + i * sizeof(U);
            const auto p = path + "[" + std::to_string(i) + "]";
            if (!verifier.VerifyOffset(element)) return Fail(p, "invalid vector element offset");
            if (!check(v->Get(i), p)) return false;
        }
        return true;
    }
    template<class T>
    bool Strings(const T* obj, V field, const std::string& path) {
        return Vector<flatbuffers::String>(obj, field, path, [this](auto* s, const auto& p) {
            return verifier.VerifyString(s) || Fail(p, "invalid string or terminator");
        });
    }
    template<class T>
    bool StockTable(const T* obj, const std::string& path) {
        flatbuffers::Verifier sub(bytes.data(), bytes.size());
        return obj->Verify(sub) || Fail(path, "table rejected by schema verifier");
    }
    bool Operator(const dml::ir::OperatorNodeDesc* op, std::string path) {
        using O = dml::ir::OperatorNodeDesc;
        using A = dml::ir::operatorFieldTypes::AttributeDesc;
        using T = dml::ir::DmlBufferTensorDesc;
        if (!Begin(op, path)) return false;
        std::string type;
        if (!String(op, O::VT_TYPE, path + ".type", &type)) return false;
        if (!type.empty()) path += " (" + type + ")";
        auto tensor = [this](const T* t, const std::string& p) { return StockTable(t, p); };
        if (!Vector<T>(op, O::VT_INPUTS, path + ".inputs", tensor) ||
            !Vector<T>(op, O::VT_OUTPUTS, path + ".outputs", tensor) ||
            !Vector<A>(op, O::VT_ATTRIBUTES, path + ".attributes", [this](const A* a, std::string p) {
                if (!Begin(a, p)) return false;
                std::string name;
                if (!String(a, A::VT_NAME, p + ".name", &name)) return false;
                if (!name.empty()) p += " (" + name + ")";
                const auto* table = reinterpret_cast<const Table*>(a);
                if (!table->VerifyField<uint8_t>(verifier, A::VT_VAL_TYPE, 1))
                    return Fail(p + ".val_type", "invalid discriminator");
                const void* value = nullptr;
                if (!Pointer(a, A::VT_VAL, value, p + ".val")) return false;
                p += " [tag=" + std::to_string(static_cast<unsigned>(a->val_type())) +
                    " " + dml::ir::operatorFieldTypes::EnumNameAttributeFieldVariant(a->val_type()) +
                    ", payload=" + (value ? "present" : "absent") + "]";
                if (!StockTable(a, p)) return false;
                return verifier.EndTable();
            })) return false;
        return verifier.EndTable();
    }
    bool Node(const dml::ir::DmlGraphNode* node, std::string path) {
        using N = dml::ir::DmlGraphNode;
        if (!Begin(node, path)) return false;
        std::string name;
        if (!String(node, N::VT_NAME, path + ".name", &name)) return false;
        if (!name.empty()) path += " (" + name + ")";
        auto* table = reinterpret_cast<const Table*>(node);
        if (!table->VerifyField<uint8_t>(verifier, N::VT_DESC_TYPE, 1)) return Fail(path + ".desc_type", "invalid scalar field");
        const void* desc = nullptr;
        if (!Pointer(node, N::VT_DESC, desc, path + ".desc")) return false;
        if (node->desc_type() == dml::ir::NodeDesc_OperatorNodeDesc) {
            if (!Operator(static_cast<const dml::ir::OperatorNodeDesc*>(desc), path + ".operator")) return false;
        } else if (node->desc_type() == dml::ir::NodeDesc_ConstantNodeDesc) {
            if (!desc) return Fail(path + ".constant", "missing table");
            if (!StockTable(static_cast<const dml::ir::ConstantNodeDesc*>(desc), path + ".constant")) return false;
        } else {
            // The generated union verifier permits unknown tags. Report them
            // rather than guessing a schema or accepting them here.
            return Fail(path + ".desc_type", "unknown or empty node union tag");
        }
        if (!Strings(node, N::VT_INPUTNAMES, path + ".inputNames") ||
            !Strings(node, N::VT_OUTPUTNAMES, path + ".outputNames")) return false;
        return verifier.EndTable();
    }
public:
    explicit Probe(std::span<const uint8_t> data) : bytes(data), verifier(data.data(), data.size()) {}
    std::string Describe() {
        using G = dml::ir::DmlGraphDesc;
        const auto offset = verifier.VerifyOffset(size_t{0});
        if (!offset) return "root: invalid FlatBuffers root offset";
        const auto* root = reinterpret_cast<const G*>(bytes.data() + offset);
        if (!Begin(root, "root") ||
            !Vector<dml::ir::DmlGraphNode>(root, G::VT_NODES, "nodes", [this](auto* n, const auto& p) { return Node(n, p); }) ||
            !Strings(root, G::VT_GRAPHINPUTNAMES, "graphInputNames") ||
            !Strings(root, G::VT_GRAPHOUTPUTNAMES, "graphOutputNames")) return error;
        return "individual probes passed; full schema verification rejected the graph (aggregate complexity or another nested constraint)";
    }
};

inline std::string DescribeFailure(std::span<const uint8_t> graph) {
    return Probe(graph).Describe();
}
} // namespace dmlgraphdiag
