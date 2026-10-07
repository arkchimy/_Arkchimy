//Parser.h
#pragma once
#include <unordered_map>
#include <string>
#include <charconv>

class Parser
{
public:
	explicit Parser(const char* filename);
	Parser(const Parser&) = delete;
	Parser(Parser&&) = delete;

	template<typename T>
	_Success_(return) bool Get(const char* key, T& value) const;

public:
	Parser& operator = (const Parser&) = delete;
	Parser& operator =(Parser&&) = delete;

private:
	std::unordered_map<std::string, std::string> mValues;
};

template<typename T>
inline _Success_(return) bool Parser::Get(const char* key, __out  T& value) const
{
	const auto iter = mValues.find(key);
	if (iter == mValues.end())
	{
		return false;
	}
	const std::string& str = iter->second;
	const std::from_chars_result& result = std::from_chars(str.c_str(),str.c_str() + str.size(), value);

	if (result.ec != std::errc{})
	{
		//struct from_chars_result
		//{
		//	const char* ptr;    // 변환을 멈춘 위치
		//	std::errc   ec;     // 에러코드. 성공이면 기본값(std::errc{})
		//};
		// 숫자가 아닌 값이 들어감.
		__debugbreak();
	}
	return true;
}

template<>
inline _Success_(return) bool Parser::Get(const char* key, __out std::string& value) const
{
	const auto iter = mValues.find(key);
	if (iter == mValues.end())
	{
		return false;
	}
	value = iter->second;
	return true;
}