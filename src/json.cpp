// JSON codec: 1 MiB input and nesting depth 64. Exceptions are translated into protocol errors by HTTP handlers.
#include "json.hpp"
#include <cmath>
#include <cctype>

namespace sm
{
namespace
{
std::string escaped(const std::string& s)
{
    std::string out = "\"";
    const char* digits = "0123456789abcdef";
    for (unsigned char c : s)
    {
        switch (c)
        {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 32)
                {
                    out += "\\u00";
                    out += digits[c >> 4];
                    out += digits[c & 15];
                }
                else
                {
                    out += char(c);
                }
        }
    }
    return out + "\"";
}

struct Parser
{
    const std::string& s;
    size_t p = 0;

    [[noreturn]] void fail() const
    {
        throw std::runtime_error("invalid JSON at byte " + std::to_string(p));
    }

    void space()
    {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n'))
        {
            ++p;
        }
    }

    bool take(char c)
    {
        space();
        if (p < s.size() && s[p] == c)
        {
            ++p;
            return true;
        }
        return false;
    }

    unsigned code()
    {
        unsigned v = 0;
        for (int i = 0; i < 4; ++i)
        {
            if (p == s.size())
            {
                fail();
            }
            char c = s[p++];
            v *= 16;
            if (c >= '0' && c <= '9')
            {
                v += c - '0';
            }
            else if (c >= 'a' && c <= 'f')
            {
                v += c - 'a' + 10;
            }
            else if (c >= 'A' && c <= 'F')
            {
                v += c - 'A' + 10;
            }
            else
            {
                fail();
            }
        }
        return v;
    }

    std::string string()
    {
        if (!take('"'))
        {
            fail();
        }
        std::string out;
        while (p < s.size())
        {
            unsigned char c = s[p++];
            if (c == '"')
            {
                if (!out.empty())
                {
                    widen(out);
                }
                return out;
            }
            if (c < 32)
            {
                fail();
            }
            if (c != '\\')
            {
                out += char(c);
                continue;
            }
            if (p == s.size())
            {
                fail();
            }
            c = s[p++];
            switch (c)
            {
                case '"':
                    out += '"';
                    break;
                case '\\':
                    out += '\\';
                    break;
                case '/':
                    out += '/';
                    break;
                case 'b':
                    out += '\b';
                    break;
                case 'f':
                    out += '\f';
                    break;
                case 'n':
                    out += '\n';
                    break;
                case 'r':
                    out += '\r';
                    break;
                case 't':
                    out += '\t';
                    break;
                case 'u':
                {
                    unsigned v = code();
                    std::wstring w;
                    if (v >= 0xd800 && v <= 0xdbff)
                    {
                        if (p + 2 > s.size() || s[p++] != '\\' || s[p++] != 'u')
                        {
                            fail();
                        }
                        unsigned low = code();
                        if (low < 0xdc00 || low > 0xdfff)
                        {
                            fail();
                        }
                        w += wchar_t(v);
                        w += wchar_t(low);
                    }
                    else
                    {
                        if (v >= 0xdc00 && v <= 0xdfff)
                        {
                            fail();
                        }
                        w += wchar_t(v);
                    }
                    out += utf8(w);
                    break;
                }
                default:
                    fail();
            }
        }
        fail();
    }

    Json value(unsigned depth = 0)
    {
        if (depth > 64)
        {
            fail();
        }
        space();
        if (p == s.size())
        {
            fail();
        }
        char c = s[p];
        if (c == '"')
        {
            return string();
        }
        if (c == '{')
        {
            ++p;
            Json::Object o;
            if (take('}'))
            {
                return o;
            }
            do
            {
                auto k = string();
                if (!take(':') || o.count(k))
                {
                    fail();
                }
                o.emplace(std::move(k), value(depth + 1));
            } while (take(','));
            if (!take('}'))
            {
                fail();
            }
            return o;
        }
        if (c == '[')
        {
            ++p;
            Json::Array a;
            if (take(']'))
            {
                return a;
            }
            do
            {
                a.push_back(value(depth + 1));
            } while (take(','));
            if (!take(']'))
            {
                fail();
            }
            return a;
        }
        for (auto item : {std::pair{"true", Json(true)},
                 std::pair{"false", Json(false)},
                 std::pair{"null", Json()}})
        {
            std::string word = item.first;
            if (s.compare(p, word.size(), word) == 0)
            {
                p += word.size();
                return item.second;
            }
        }
        size_t start = p;
        if (c == '-')
        {
            ++p;
        }
        if (p == s.size())
        {
            fail();
        }
        if (s[p] == '0')
        {
            ++p;
        }
        else
        {
            if (s[p] < '1' || s[p] > '9')
            {
                fail();
            }
            while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p])))
            {
                ++p;
            }
        }
        if (p < s.size() && s[p] == '.')
        {
            ++p;
            size_t q = p;
            while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p])))
            {
                ++p;
            }
            if (q == p)
            {
                fail();
            }
        }
        if (p < s.size() && (s[p] == 'e' || s[p] == 'E'))
        {
            ++p;
            if (p < s.size() && (s[p] == '+' || s[p] == '-'))
            {
                ++p;
            }
            size_t q = p;
            while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p])))
            {
                ++p;
            }
            if (q == p)
            {
                fail();
            }
        }
        Json j;
        j.value = Json::Number{s.substr(start, p - start)};
        return j;
    }
};
} // namespace

std::string Json::dump() const
{
    if (std::holds_alternative<std::nullptr_t>(value))
    {
        return "null";
    }
    if (auto p = std::get_if<bool>(&value))
    {
        return *p ? "true" : "false";
    }
    if (auto p = std::get_if<Number>(&value))
    {
        return p->text;
    }
    if (auto p = std::get_if<std::string>(&value))
    {
        return escaped(*p);
    }
    std::string out;
    bool first = true;
    if (auto p = std::get_if<Array>(&value))
    {
        out = "[";
        for (auto& v : *p)
        {
            if (!first)
            {
                out += ",";
            }
            first = false;
            out += v.dump();
        }
        return out + "]";
    }
    out = "{";
    for (auto& [k, v] : std::get<Object>(value))
    {
        if (!first)
        {
            out += ",";
        }
        first = false;
        out += escaped(k) + ":" + v.dump();
    }
    return out + "}";
}

Json Json::parse(const std::string& text)
{
    if (text.size() > 1024 * 1024)
    {
        throw std::runtime_error("JSON too large");
    }
    Parser p{text};
    auto result = p.value();
    p.space();
    if (p.p != text.size())
    {
        p.fail();
    }
    return result;
}
} // namespace sm
