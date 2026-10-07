// Parser.h
#pragma once
#include <charconv>
#include <string>
#include <unordered_map>

namespace parser
{
enum eParserError
{
    Success = 0,
    NotFoundFile = 1,
    HasNotSemicolon = 2,
    HasNotEqule = 3,
    Invalid_Key = 4,
    Invalid_Equle = 5,
    Invalid_Value = 6,
    Max,
};
struct stDebug
{
    explicit stDebug(const char *file)
        : filename(file), err(Success)
    {
    }
    const char *filename;
    std::string lastKey;
    std::string lastValue;
    eParserError err;
};
class Parser
{
  public:
    explicit Parser(const char *filename);
    Parser(const Parser &) = delete;
    Parser(Parser &&) = delete;

    template <typename T>
    _Success_(return) bool Get(const char *key, T &value) const;
    friend const Parser &operator>>(std::ifstream &file, Parser &parser);

  public:
    Parser &operator=(const Parser &) = delete;
    Parser &operator=(Parser &&) = delete;

  private:
    bool successChk() const { return mErr.err == eParserError::Success; }

  private:
    std::unordered_map<std::string, std::string> mValues;
    stDebug mErr;
};

template <typename T>
inline _Success_(return) bool Parser::Get(const char *key, __out T &value) const
{
    const auto iter = mValues.find(key);
    if (iter == mValues.end())
    {
        return false;
    }
    const std::string &str = iter->second;
    const std::from_chars_result &result = std::from_chars(str.c_str(), str.c_str() + str.size(), value);

    if (result.ec != std::errc{})
    {
        return false;
    }
    return true;
}

template <>
inline _Success_(return) bool Parser::Get(const char *key, __out std::string &value) const
{
    const auto iter = mValues.find(key);
    if (iter == mValues.end())
    {
        return false;
    }
    value = iter->second;
    return true;
}
} // namespace parser