#pragma once
#define WIN32_LEAN_AND_MEAN             // 거의 사용되지 않는 내용을 Windows 헤더에서 제외합니다.

#include <charconv>
#include <memory>
#include <string>
#include <unordered_map>

namespace utility
{
enum eParserError
{
    Success = 0,
    NotFoundFile = 1,
    HasNotSemicolon = 2,
    HasNotEqual = 3,
    Invalid_Key = 4,
    Invalid_Equal = 5,
    Invalid_Value = 6,
    Exist_Key,
    Value_After_Data,
    Max,
};
struct stDebug
{
    explicit stDebug()
        : filename(nullptr), err(Success)
    {
    }
    void CreateDebugFile();

    const char *filename;
    std::string lastKey;
    std::string lastValue;
    eParserError err;
};
class Parser
{
  private:
    explicit Parser(const char *filename, stDebug &debug);
    void loadFile(const char *filename, stDebug &debug);

  public:
    static std::unique_ptr<Parser> CreateParser(const char *filename, stDebug &debug);

    Parser(const Parser &) = delete;
    Parser(Parser &&) = delete;

    template <typename T>
    _Success_(return) bool Get(const char *key, T &value) const;

  public:
    Parser &operator=(const Parser &) = delete;
    Parser &operator=(Parser &&) = delete;

  private:
    std::unordered_map<std::string, std::string> mValues;
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

// cfg의 0/1을 bool로 읽는다 - std::from_chars가 bool을 지원하지 않아 따로 둔다
template <>
inline _Success_(return) bool Parser::Get(const char *key, __out bool &value) const
{
    int number = 0;
    if (!Get(key, number)) // 숫자로 먼저 읽는다
    {
        return false;      // 키가 없거나 숫자가 아니면 value는 그대로(기본값 유지)
    }
    value = (number != 0);
    return true;
}
} // namespace parser