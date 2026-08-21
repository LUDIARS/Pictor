#include "pictor/visus/visus_metadata.h"

namespace pictor {

// ---- VisusValue ------------------------------------------------------------

namespace {
const std::string&        empty_string() { static const std::string s; return s; }
const VisusValue::Array&  empty_array()  { static const VisusValue::Array a; return a; }
const VisusMetadata&      empty_object() { static const VisusMetadata o; return o; }
} // namespace

VisusValue::VisusValue() = default;
VisusValue::VisusValue(bool v)     : type_(Type::BOOL),   bool_(v) {}
VisusValue::VisusValue(double v)   : type_(Type::NUMBER), number_(v) {}
VisusValue::VisusValue(int v)      : type_(Type::NUMBER), number_(static_cast<double>(v)) {}
VisusValue::VisusValue(unsigned v) : type_(Type::NUMBER), number_(static_cast<double>(v)) {}
VisusValue::VisusValue(int64_t v)  : type_(Type::NUMBER), number_(static_cast<double>(v)) {}
VisusValue::VisusValue(uint64_t v) : type_(Type::NUMBER), number_(static_cast<double>(v)) {}
VisusValue::VisusValue(const char* v)      : type_(Type::STRING), string_(v ? v : "") {}
VisusValue::VisusValue(std::string_view v) : type_(Type::STRING), string_(v) {}
VisusValue::VisusValue(std::string v)      : type_(Type::STRING), string_(std::move(v)) {}
VisusValue::VisusValue(Array v)
    : type_(Type::ARRAY), array_(std::make_unique<Array>(std::move(v))) {}
VisusValue::VisusValue(VisusMetadata v)
    : type_(Type::OBJECT), object_(std::make_unique<VisusMetadata>(std::move(v))) {}

VisusValue::VisusValue(const VisusValue& other) { copy_from_(other); }

// Move leaves the source as a null value. A defaulted move would keep `type_`
// while the payload pointer moves away, so a moved-from value would still
// answer `is_array()` / `is_object()` with true while holding nothing.
VisusValue::VisusValue(VisusValue&& other) noexcept
    : type_(other.type_),
      bool_(other.bool_),
      number_(other.number_),
      string_(std::move(other.string_)),
      array_(std::move(other.array_)),
      object_(std::move(other.object_)) {
    other.reset_();
}

VisusValue& VisusValue::operator=(const VisusValue& other) {
    if (this != &other) { reset_(); copy_from_(other); }
    return *this;
}

VisusValue& VisusValue::operator=(VisusValue&& other) noexcept {
    if (this != &other) {
        type_   = other.type_;
        bool_   = other.bool_;
        number_ = other.number_;
        string_ = std::move(other.string_);
        array_  = std::move(other.array_);
        object_ = std::move(other.object_);
        other.reset_();
    }
    return *this;
}

VisusValue::~VisusValue() = default;

void VisusValue::reset_() {
    type_ = Type::NUL;
    bool_ = false;
    number_ = 0.0;
    string_.clear();
    array_.reset();
    object_.reset();
}

void VisusValue::copy_from_(const VisusValue& other) {
    type_   = other.type_;
    bool_   = other.bool_;
    number_ = other.number_;
    string_ = other.string_;
    if (other.array_)  array_  = std::make_unique<Array>(*other.array_);
    if (other.object_) object_ = std::make_unique<VisusMetadata>(*other.object_);
}

bool VisusValue::as_bool() const { return type_ == Type::BOOL ? bool_ : false; }
double VisusValue::as_number() const { return type_ == Type::NUMBER ? number_ : 0.0; }
const std::string& VisusValue::as_string() const {
    return type_ == Type::STRING ? string_ : empty_string();
}
const VisusValue::Array& VisusValue::as_array() const {
    return (type_ == Type::ARRAY && array_) ? *array_ : empty_array();
}
const VisusMetadata& VisusValue::as_object() const {
    return (type_ == Type::OBJECT && object_) ? *object_ : empty_object();
}

VisusValue::Array& VisusValue::mutable_array() {
    if (type_ != Type::ARRAY || !array_) {
        reset_();
        type_  = Type::ARRAY;
        array_ = std::make_unique<Array>();
    }
    return *array_;
}

VisusMetadata& VisusValue::mutable_object() {
    if (type_ != Type::OBJECT || !object_) {
        reset_();
        type_   = Type::OBJECT;
        object_ = std::make_unique<VisusMetadata>();
    }
    return *object_;
}

bool VisusValue::operator==(const VisusValue& other) const {
    if (type_ != other.type_) return false;
    switch (type_) {
        case Type::NUL:    return true;
        case Type::BOOL:   return bool_ == other.bool_;
        case Type::NUMBER: return number_ == other.number_;
        case Type::STRING: return string_ == other.string_;
        case Type::ARRAY:  return as_array() == other.as_array();
        case Type::OBJECT: return as_object() == other.as_object();
    }
    return false;
}

// ---- VisusMetadata ---------------------------------------------------------

const VisusValue* VisusMetadata::find(std::string_view key) const {
    for (const Entry& e : entries_) {
        if (e.first == key) return &e.second;
    }
    return nullptr;
}

std::optional<std::string> VisusMetadata::get_string(std::string_view key) const {
    const VisusValue* v = find(key);
    if (!v || !v->is_string()) return std::nullopt;
    return v->as_string();
}

std::optional<double> VisusMetadata::get_number(std::string_view key) const {
    const VisusValue* v = find(key);
    if (!v || !v->is_number()) return std::nullopt;
    return v->as_number();
}

std::optional<bool> VisusMetadata::get_bool(std::string_view key) const {
    const VisusValue* v = find(key);
    if (!v || !v->is_bool()) return std::nullopt;
    return v->as_bool();
}

const VisusValue::Array* VisusMetadata::get_array(std::string_view key) const {
    const VisusValue* v = find(key);
    if (!v || !v->is_array()) return nullptr;
    return &v->as_array();
}

const VisusMetadata* VisusMetadata::get_object(std::string_view key) const {
    const VisusValue* v = find(key);
    if (!v || !v->is_object()) return nullptr;
    return &v->as_object();
}

void VisusMetadata::set(std::string key, VisusValue value) {
    for (Entry& e : entries_) {
        if (e.first == key) { e.second = std::move(value); return; }
    }
    entries_.emplace_back(std::move(key), std::move(value));
}

bool VisusMetadata::erase(std::string_view key) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->first == key) { entries_.erase(it); return true; }
    }
    return false;
}

bool VisusMetadata::operator==(const VisusMetadata& other) const {
    return entries_ == other.entries_;
}

} // namespace pictor
