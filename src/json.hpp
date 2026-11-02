// Size/depth-limited JSON values and parser for MCP, with no script execution or implicit type coercion.
// Strings use UTF-8 and Unicode escapes validate surrogate pairs. Numbers retain text to preserve request IDs.
#pragma once
#include "common.hpp"

namespace sm
{
struct Json
{
    struct Number
    {
        std::string text;
    };

    using Object = std::map<std::string, Json>;
    using Array = std::vector<Json>;
    std::variant<std::nullptr_t, bool, Number, std::string, Array, Object> value = nullptr;
    Json() = default;

    Json(std::nullptr_t)
    {
    }

    Json(bool v)
        : value(v)
    {
    }

    Json(int v)
        : value(Number{std::to_string(v)})
    {
    }

    Json(int64_t v)
        : value(Number{std::to_string(v)})
    {
    }

    Json(const char* v)
        : value(std::string(v))
    {
    }

    Json(std::string v)
        : value(std::move(v))
    {
    }

    Json(Array v)
        : value(std::move(v))
    {
    }

    Json(Object v)
        : value(std::move(v))
    {
    }

    static Json object(std::initializer_list<Object::value_type> v)
    {
        return Object(v);
    }

    static Json array(std::initializer_list<Json> v)
    {
        return Array(v);
    }

    bool is_object() const
    {
        return std::holds_alternative<Object>(value);
    }

    bool is_array() const
    {
        return std::holds_alternative<Array>(value);
    }

    bool is_string() const
    {
        return std::holds_alternative<std::string>(value);
    }

    bool has(const std::string& key) const
    {
        return is_object() && std::get<Object>(value).count(key);
    }

    const Json& at(const std::string& key) const
    {
        static Json empty;
        if (!is_object())
        {
            return empty;
        }
        auto& o = std::get<Object>(value);
        auto i = o.find(key);
        return i == o.end() ? empty : i->second;
    }

    std::string text(std::string fallback = "") const
    {
        auto p = std::get_if<std::string>(&value);
        return p ? *p : fallback;
    }

    int64_t integer(int64_t fallback = 0) const
    {
        auto p = std::get_if<Number>(&value);
        if (!p)
        {
            return fallback;
        }
        try
        {
            size_t n;
            auto v = std::stoll(p->text, &n);
            return n == p->text.size() ? v : fallback;
        }
        catch (...)
        {
            return fallback;
        }
    }

    bool boolean(bool fallback = false) const
    {
        auto p = std::get_if<bool>(&value);
        return p ? *p : fallback;
    }

    std::string dump() const;
    static Json parse(const std::string& text);
};
} // namespace sm
