#include "Json.h"

#include <cstring>
#include <cstdlib>

namespace mjson {
namespace {

// 嵌套层数上限，防止畸形输入把解析栈打爆
const int kMaxDepth = 32;

class Parser
{
public:
    Parser(const char* begin, const char* end)
        : m_p(begin), m_end(end) {}

    bool ParseValue(Value& v, int depth);
    void SkipWhitespace();
    bool AtEnd() const { return m_p >= m_end; }

private:
    bool ParseString(std::string& s);
    bool ParseNumber(Value& v);
    bool ParseObject(Value& v, int depth);
    bool ParseArray(Value& v, int depth);
    bool MatchLiteral(const char* literal);
    bool ParseHex4(unsigned& out);
    static void AppendUtf8(std::string& s, unsigned code_point);

    const char* m_p;
    const char* m_end;
};

void Parser::SkipWhitespace()
{
    while (m_p < m_end && (*m_p == ' ' || *m_p == '\t' || *m_p == '\n' || *m_p == '\r'))
        ++m_p;
}

bool Parser::MatchLiteral(const char* literal)
{
    const size_t len = std::strlen(literal);
    if (static_cast<size_t>(m_end - m_p) < len)
        return false;
    if (std::memcmp(m_p, literal, len) != 0)
        return false;
    m_p += len;
    return true;
}

bool Parser::ParseHex4(unsigned& out)
{
    if (m_end - m_p < 4)
        return false;
    unsigned value = 0;
    for (int i = 0; i < 4; ++i)
    {
        const char c = m_p[i];
        value <<= 4;
        if (c >= '0' && c <= '9')
            value |= static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f')
            value |= static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            value |= static_cast<unsigned>(c - 'A' + 10);
        else
            return false;
    }
    m_p += 4;
    out = value;
    return true;
}

void Parser::AppendUtf8(std::string& s, unsigned cp)
{
    if (cp < 0x80)
    {
        s.push_back(static_cast<char>(cp));
    }
    else if (cp < 0x800)
    {
        s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000)
    {
        s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else
    {
        s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool Parser::ParseString(std::string& s)
{
    if (AtEnd() || *m_p != '"')
        return false;
    ++m_p;
    s.clear();
    while (m_p < m_end)
    {
        const char c = *m_p;
        if (c == '"')
        {
            ++m_p;
            return true;
        }
        if (c == '\\')
        {
            ++m_p;
            if (AtEnd())
                return false;
            const char esc = *m_p++;
            switch (esc)
            {
            case '"':  s.push_back('"');  break;
            case '\\': s.push_back('\\'); break;
            case '/':  s.push_back('/');  break;
            case 'b':  s.push_back('\b'); break;
            case 'f':  s.push_back('\f'); break;
            case 'n':  s.push_back('\n'); break;
            case 'r':  s.push_back('\r'); break;
            case 't':  s.push_back('\t'); break;
            case 'u':
            {
                unsigned cp = 0;
                if (!ParseHex4(cp))
                    return false;
                // 高代理项：尝试与紧随其后的低代理项合成一个码点
                if (cp >= 0xD800 && cp <= 0xDBFF && m_end - m_p >= 6 &&
                    m_p[0] == '\\' && m_p[1] == 'u')
                {
                    const char* saved = m_p;
                    m_p += 2;
                    unsigned low = 0;
                    if (ParseHex4(low) && low >= 0xDC00 && low <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    else
                        m_p = saved;
                }
                AppendUtf8(s, cp);
                break;
            }
            default:
                return false;
            }
            continue;
        }
        s.push_back(c);
        ++m_p;
    }
    return false;   // 字符串未闭合
}

bool Parser::ParseNumber(Value& v)
{
    const char* start = m_p;
    if (m_p < m_end && (*m_p == '-' || *m_p == '+'))
        ++m_p;
    while (m_p < m_end && ((*m_p >= '0' && *m_p <= '9') || *m_p == '.' ||
                           *m_p == 'e' || *m_p == 'E' || *m_p == '-' || *m_p == '+'))
        ++m_p;
    if (m_p == start)
        return false;

    const std::string token(start, m_p);
    char* parse_end = nullptr;
    const double value = std::strtod(token.c_str(), &parse_end);
    if (parse_end == token.c_str())
        return false;

    v.type = Value::T_NUMBER;
    v.number = value;
    return true;
}

bool Parser::ParseArray(Value& v, int depth)
{
    ++m_p;   // 跳过 '['
    v.type = Value::T_ARRAY;
    SkipWhitespace();
    if (m_p < m_end && *m_p == ']')
    {
        ++m_p;
        return true;
    }
    for (;;)
    {
        Value element;
        if (!ParseValue(element, depth + 1))
            return false;
        v.array.push_back(std::move(element));
        SkipWhitespace();
        if (AtEnd())
            return false;
        if (*m_p == ',')
        {
            ++m_p;
            SkipWhitespace();
            continue;
        }
        if (*m_p == ']')
        {
            ++m_p;
            return true;
        }
        return false;
    }
}

bool Parser::ParseObject(Value& v, int depth)
{
    ++m_p;   // 跳过 '{'
    v.type = Value::T_OBJECT;
    SkipWhitespace();
    if (m_p < m_end && *m_p == '}')
    {
        ++m_p;
        return true;
    }
    for (;;)
    {
        SkipWhitespace();
        std::string key;
        if (!ParseString(key))
            return false;
        SkipWhitespace();
        if (AtEnd() || *m_p != ':')
            return false;
        ++m_p;
        SkipWhitespace();
        Value member;
        if (!ParseValue(member, depth + 1))
            return false;
        v.object[key] = std::move(member);
        SkipWhitespace();
        if (AtEnd())
            return false;
        if (*m_p == ',')
        {
            ++m_p;
            continue;
        }
        if (*m_p == '}')
        {
            ++m_p;
            return true;
        }
        return false;
    }
}

bool Parser::ParseValue(Value& v, int depth)
{
    if (depth > kMaxDepth)
        return false;
    SkipWhitespace();
    if (AtEnd())
        return false;

    switch (*m_p)
    {
    case '{':
        return ParseObject(v, depth);
    case '[':
        return ParseArray(v, depth);
    case '"':
        v.type = Value::T_STRING;
        return ParseString(v.string);
    case 't':
        if (!MatchLiteral("true"))
            return false;
        v.type = Value::T_BOOL;
        v.boolean = true;
        return true;
    case 'f':
        if (!MatchLiteral("false"))
            return false;
        v.type = Value::T_BOOL;
        v.boolean = false;
        return true;
    case 'n':
        if (!MatchLiteral("null"))
            return false;
        v.type = Value::T_NULL;
        return true;
    default:
        return ParseNumber(v);
    }
}

}   // namespace

bool Parse(const std::string& text, Value& out)
{
    // 跳过可能存在的 UTF-8 BOM
    size_t offset = 0;
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
    {
        offset = 3;
    }

    Parser parser(text.data() + offset, text.data() + text.size());
    if (!parser.ParseValue(out, 0))
        return false;
    parser.SkipWhitespace();
    return parser.AtEnd();
}

}   // namespace mjson
