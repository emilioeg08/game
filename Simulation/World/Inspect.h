#pragma once

#include "Engine/Core/Types.h"
#include "Engine/Math/Vec3.h"
#include "Engine/Time/SimTime.h"
#include "Simulation/World/EntityRegistry.h"

#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Read-only reflection for debug tools (entity inspector, save inspector), built on the same `io` members
// that serialization uses, so a component describes its fields exactly once. Components that name their
// fields (`ar.io("hull", hull)`) get labelled output; unnamed fields are shown by position.
namespace gx {

class FieldVisitor {
public:
    virtual ~FieldVisitor() = default;
    virtual void beginGroup(std::string_view name) = 0;
    virtual void endGroup() = 0;
    virtual void field(std::string_view name, std::string_view formattedValue) = 0;
    // Entity references get their own callback so tools can make them clickable.
    virtual void entityField(std::string_view name, EntityId entity) = 0;
};

namespace detail {
template <typename T>
struct IsInspectVector : std::false_type {};
template <typename T, typename Allocator>
struct IsInspectVector<std::vector<T, Allocator>> : std::true_type {};
} // namespace detail

// Archive adapter feeding a FieldVisitor.
class InspectArchive {
public:
    static constexpr bool kIsReading = false;

    explicit InspectArchive(FieldVisitor& visitor) : m_visitor(visitor) {}

    template <typename T>
    void io(const T& value) {
        io(std::format("#{}", m_unnamedIndex++), value);
    }

    template <typename T>
    void io(std::string_view name, const T& value) {
        if constexpr (std::is_same_v<T, bool>) {
            m_visitor.field(name, value ? "true" : "false");
        } else if constexpr (std::is_enum_v<T>) {
            if constexpr (requires { toString(value); }) { // enums with a toString() overload show their name
                m_visitor.field(name, std::format("{} ({})", toString(value), static_cast<i64>(value)));
            } else {
                m_visitor.field(name, std::format("{}", static_cast<i64>(value)));
            }
        } else if constexpr (std::is_arithmetic_v<T>) {
            m_visitor.field(name, std::format("{}", value));
        } else if constexpr (std::is_same_v<T, std::string>) {
            m_visitor.field(name, value);
        } else if constexpr (std::is_same_v<T, Vec3d>) {
            m_visitor.field(name, std::format("({:.6g}, {:.6g}, {:.6g})", value.x, value.y, value.z));
        } else if constexpr (std::is_same_v<T, SimTime>) {
            m_visitor.field(name, formatSimTime(value));
        } else if constexpr (std::is_same_v<T, SimDuration>) {
            m_visitor.field(name, formatDuration(value));
        } else if constexpr (std::is_same_v<T, EntityId>) {
            m_visitor.entityField(name, value);
        } else if constexpr (std::is_same_v<T, std::vector<std::byte>>) {
            m_visitor.field(name, std::format("<{} bytes>", value.size()));
        } else if constexpr (detail::IsInspectVector<T>::value) {
            m_visitor.beginGroup(std::format("{} [{}]", name, value.size()));
            for (usize i = 0; i < value.size(); ++i) {
                io(std::format("[{}]", i), value[i]);
            }
            m_visitor.endGroup();
        } else {
            m_visitor.beginGroup(name);
            const u32 savedIndex = m_unnamedIndex;
            m_unnamedIndex = 0;
            const_cast<T&>(value).io(*this);
            m_unnamedIndex = savedIndex;
            m_visitor.endGroup();
        }
    }

private:
    FieldVisitor& m_visitor;
    u32 m_unnamedIndex = 0;
};

} // namespace gx
