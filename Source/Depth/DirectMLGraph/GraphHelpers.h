#pragma once
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace gsl {
using std::span;
template<class = void> using czstring = const char*;
template<class T, class U> constexpr T narrow_cast(U value) { return static_cast<T>(value); }
template<class T> constexpr std::span<T> make_span(T* ptr, std::size_t count) { return {ptr, count}; }
}

class BucketAllocator {
    std::pmr::monotonic_buffer_resource storage;
public:
    template<class T> T* Allocate(std::size_t count = 1) {
        return static_cast<T*>(storage.allocate(sizeof(T) * count, alignof(T)));
    }
};

#define THROW_HR(hr) Fail((hr))
#define THROW_IF_FAILED(hr) Check((hr))
#include "ApiTraits.h"
#include "ApiHelpers.h"
#include "DirectMLSchema.h"
#include "AbstractOperatorDesc.h"
#include "GeneratedSchemaTypes.h"
inline AbstractOperatorDesc::AbstractOperatorDesc() = default;
inline AbstractOperatorDesc::AbstractOperatorDesc(const DML_OPERATOR_SCHEMA* schema,
                                                  std::vector<OperatorField>&& fields)
    : schema(schema), fields(std::move(fields)) {}
inline AbstractOperatorDesc::~AbstractOperatorDesc() = default;
inline AbstractOperatorDesc::AbstractOperatorDesc(const AbstractOperatorDesc&) = default;
inline AbstractOperatorDesc::AbstractOperatorDesc(AbstractOperatorDesc&&) noexcept = default;
inline AbstractOperatorDesc& AbstractOperatorDesc::operator=(const AbstractOperatorDesc&) = default;
inline AbstractOperatorDesc& AbstractOperatorDesc::operator=(AbstractOperatorDesc&&) noexcept = default;
#include "SchemaHelpers.h"
#include "GeneratedSchemaHelpers.h"
#include "AbstractOperatorDescImpl.h"
#include "ApiTraits.inc"
#include "DmlGraphDeserialization.inc"
#include "DmlGraphHelper.h"
#undef THROW_IF_FAILED
#undef THROW_HR
