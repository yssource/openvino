// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "ngraph/compatibility.hpp"
#include "openvino/core/core_visibility.hpp"

namespace ov {

/// Supports three functions, ov::is_type<Type>, ov::as_type<Type>, and ov::as_type_ptr<Type> for type-safe
/// dynamic conversions via static_cast/static_ptr_cast without using C++ RTTI.
/// Type must have a static type_info member and a virtual get_type_info() member that
/// returns a reference to its type_info member.

/// Type information for a type system without inheritance; instances have exactly one type not
/// related to any other type.
struct OPENVINO_API DiscreteTypeInfo {
    const char* name;
    OPENVINO_DEPRECATED("This member was deprecated. Please use version_id instead.")
    uint64_t version;
    const char* version_id;
    // A pointer to a parent type info; used for casting and inheritance traversal, not for
    // exact type identification
    const DiscreteTypeInfo* parent;

    DiscreteTypeInfo() = default;
    OPENVINO_SUPPRESS_DEPRECATED_START
    DiscreteTypeInfo(const DiscreteTypeInfo&) = default;
    DiscreteTypeInfo(DiscreteTypeInfo&&) = default;
    DiscreteTypeInfo& operator=(const DiscreteTypeInfo&) = default;

    explicit constexpr DiscreteTypeInfo(const char* _name,
                                        const char* _version_id,
                                        const DiscreteTypeInfo* _parent = nullptr)
        : name(_name),
          version(0),
          version_id(_version_id),
          parent(_parent),
          hash_value(0) {}

    constexpr DiscreteTypeInfo(const char* _name, uint64_t _version, const DiscreteTypeInfo* _parent = nullptr)
        : name(_name),
          version(_version),
          version_id(nullptr),
          parent(_parent),
          hash_value(0) {}

    constexpr DiscreteTypeInfo(const char* _name,
                               uint64_t _version,
                               const char* _version_id,
                               const DiscreteTypeInfo* _parent = nullptr)
        : name(_name),
          version(_version),
          version_id(_version_id),
          parent(_parent),
          hash_value(0) {}
    OPENVINO_SUPPRESS_DEPRECATED_END

    bool is_castable(const DiscreteTypeInfo& target_type) const;

    std::string get_version() const;

    // For use as a key
    bool operator<(const DiscreteTypeInfo& b) const;
    bool operator<=(const DiscreteTypeInfo& b) const;
    bool operator>(const DiscreteTypeInfo& b) const;
    bool operator>=(const DiscreteTypeInfo& b) const;
    bool operator==(const DiscreteTypeInfo& b) const;
    bool operator!=(const DiscreteTypeInfo& b) const;

    operator std::string() const;

    size_t hash() const;
    size_t hash();

private:
    size_t hash_value;
};

OPENVINO_API
std::ostream& operator<<(std::ostream& s, const DiscreteTypeInfo& info);

/// \brief Tests if value is a pointer/shared_ptr that can be statically cast to a
/// Type*/shared_ptr<Type>
OPENVINO_SUPPRESS_DEPRECATED_START
template <typename Type, typename Value>
typename std::enable_if<
    ngraph::HasTypeInfoMember<Type>::value &&
        std::is_convertible<decltype(std::declval<Value>()->get_type_info().is_castable(Type::type_info)), bool>::value,
    bool>::type
is_type(Value value) {
    return value->get_type_info().is_castable(Type::type_info);
}

template <typename Type, typename Value>
typename std::enable_if<
    !ngraph::HasTypeInfoMember<Type>::value &&
        std::is_convertible<decltype(std::declval<Value>()->get_type_info().is_castable(Type::get_type_info_static())),
                            bool>::value,
    bool>::type
is_type(Value value) {
    return value->get_type_info().is_castable(Type::get_type_info_static());
}
OPENVINO_SUPPRESS_DEPRECATED_END

/// Casts a Value* to a Type* if it is of type Type, nullptr otherwise
template <typename Type, typename Value>
typename std::enable_if<std::is_convertible<decltype(static_cast<Type*>(std::declval<Value>())), Type*>::value,
                        Type*>::type
as_type(Value value) {
    return ov::is_type<Type>(value) ? static_cast<Type*>(value) : nullptr;
}

namespace util {
template <typename T>
struct AsTypePtr;
/// Casts a std::shared_ptr<Value> to a std::shared_ptr<Type> if it is of type
/// Type, nullptr otherwise
template <typename In>
struct AsTypePtr<std::shared_ptr<In>> {
    template <typename Type>
    static std::shared_ptr<Type> call(const std::shared_ptr<In>& value) {
        return ov::is_type<Type>(value) ? std::static_pointer_cast<Type>(value) : std::shared_ptr<Type>();
    }
};
}  // namespace util

/// Casts a std::shared_ptr<Value> to a std::shared_ptr<Type> if it is of type
/// Type, nullptr otherwise
template <typename T, typename U>
auto as_type_ptr(const U& value) -> decltype(::ov::util::AsTypePtr<U>::template call<T>(value)) {
    OPENVINO_SUPPRESS_DEPRECATED_START
    return ::ov::util::AsTypePtr<U>::template call<T>(value);
    OPENVINO_SUPPRESS_DEPRECATED_END
}
}  // namespace ov

namespace std {
template <>
struct OPENVINO_API hash<ov::DiscreteTypeInfo> {
    size_t operator()(const ov::DiscreteTypeInfo& k) const;
};
}  // namespace std
