/*
 * 极简 JSON 解析器（只读、无外部依赖）
 *
 * Anthropic 的 /api/oauth/usage 响应结构简单且固定，但字段会随账号类型
 * （Pro / Max / Enterprise）出现或消失，甚至出现未知的新键，因此这里用一个
 * 通用解析器而不是硬编码字段偏移，保证遇到未知结构时不会解析失败。
 */
#pragma once

#include <map>
#include <string>
#include <vector>

namespace mjson {

class Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

class Value
{
public:
    enum Type
    {
        T_NULL,
        T_BOOL,
        T_NUMBER,
        T_STRING,
        T_ARRAY,
        T_OBJECT,
    };

    Type type{ T_NULL };
    bool boolean{ false };
    double number{ 0.0 };
    std::string string;
    Array array;
    Object object;

    bool IsNull() const { return type == T_NULL; }

    /** 取对象成员，不存在时返回 nullptr */
    const Value* Find(const char* key) const
    {
        if (type != T_OBJECT)
            return nullptr;
        auto it = object.find(key);
        return it == object.end() ? nullptr : &it->second;
    }

    /** 取值为对象的成员；成员为 null 或其他类型时返回 nullptr */
    const Value* FindObject(const char* key) const
    {
        const Value* v = Find(key);
        return (v != nullptr && v->type == T_OBJECT) ? v : nullptr;
    }

    bool GetNumber(const char* key, double& out) const
    {
        const Value* v = Find(key);
        if (v == nullptr || v->type != T_NUMBER)
            return false;
        out = v->number;
        return true;
    }

    bool GetString(const char* key, std::string& out) const
    {
        const Value* v = Find(key);
        if (v == nullptr || v->type != T_STRING)
            return false;
        out = v->string;
        return true;
    }

    bool GetBool(const char* key, bool& out) const
    {
        const Value* v = Find(key);
        if (v == nullptr || v->type != T_BOOL)
            return false;
        out = v->boolean;
        return true;
    }
};

/** 解析 UTF-8 编码的 JSON 文本。失败返回 false，out 内容未定义。 */
bool Parse(const std::string& text, Value& out);

}   // namespace mjson
