// FBX Document -- Level 1 (Raw Document)
//
// Uninterpreted FBX node tree. Parses both binary FBX (7.x, 32/64-bit
// node headers) and ASCII FBX into a unified node/property/children
// structure. No typed object interpretation at this layer; that is
// delegated to the upper FBXScene (Level 2+3).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pictor {

/// FBX binary property type code (1-byte identifier).
/// ASCII properties are also mapped to these types during parsing.
enum class FBXPropertyType : uint8_t {
    UNKNOWN = 0,
    BOOL    = 'C',
    INT16   = 'Y',
    INT32   = 'I',
    INT64   = 'L',
    FLOAT   = 'F',
    DOUBLE  = 'D',
    STRING  = 'S',
    RAW     = 'R',
    ARR_BOOL    = 'b',
    ARR_INT32   = 'i',
    ARR_INT64   = 'l',
    ARR_FLOAT   = 'f',
    ARR_DOUBLE  = 'd',
};

/// FBX property (scalar, array, string, or raw bytes).
struct FBXProperty {
    FBXPropertyType type = FBXPropertyType::UNKNOWN;

    int64_t  i64 = 0;
    double   f64 = 0.0;

    std::string str;

    std::vector<int32_t> arr_i32;
    std::vector<int64_t> arr_i64;
    std::vector<float>   arr_f32;
    std::vector<double>  arr_f64;

    // -- Lenient accessors (attempt type coercion) --
    bool        as_bool()   const noexcept;
    int64_t     as_int()    const noexcept;
    double      as_double() const noexcept;
    std::string as_string() const;

    std::vector<double>  as_double_array() const;
    std::vector<int64_t> as_int_array() const;

    bool is_scalar() const noexcept;
    bool is_array()  const noexcept;
    bool is_string() const noexcept;
    size_t array_size() const noexcept;
};

/// FBX node: name + properties[] + children[] (recursive).
struct FBXNode {
    std::string              name;
    std::vector<FBXProperty> properties;
    std::vector<FBXNode>     children;

    /// Find first child with matching name. Returns nullptr if not found.
    const FBXNode* find_child(std::string_view name) const noexcept;
    /// Find all children with matching name.
    std::vector<const FBXNode*> find_children(std::string_view name) const;
    /// Traverse a "/" delimited path (e.g. "Objects/Geometry"). Leading "/" is ignored.
    const FBXNode* find_descendant(std::string_view path) const noexcept;

    /// Return property[index] of child named child_name. nullptr if missing.
    const FBXProperty* child_property(std::string_view child_name,
                                      size_t index = 0) const noexcept;
    /// Return this node's property[index]. nullptr if out of range.
    const FBXProperty* property(size_t index) const noexcept;

    bool empty() const noexcept { return name.empty() && properties.empty() && children.empty(); }
};

/// FBX document (binary / ASCII abstraction).
struct FBXDocument {
    bool        is_ascii = false;
    uint32_t    version  = 0;   ///< e.g. 7400, 7500. May be 0 for ASCII.
    FBXNode     root;

    bool parse(const uint8_t* data, size_t size, std::string* error = nullptr);
    bool parse_binary(const uint8_t* data, size_t size, std::string* error = nullptr);
    bool parse_ascii(const uint8_t* data, size_t size, std::string* error = nullptr);
    bool load_file(const std::string& path, std::string* error = nullptr);

    static bool looks_binary(const uint8_t* data, size_t size) noexcept;
};

/// Inflate a zlib (RFC 1950) stream. Self-contained, no external dependency.
/// When expected_out_size is known (FBX array: length * elem_size), pass it
/// for faster allocation.
bool fbx_zlib_decompress(const uint8_t* in, size_t in_size,
                         std::vector<uint8_t>& out,
                         size_t expected_out_size = 0,
                         std::string* error = nullptr);

} // namespace pictor
