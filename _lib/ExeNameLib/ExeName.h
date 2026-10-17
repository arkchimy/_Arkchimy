#pragma once
#include <windows.h>
#include <charconv>
#include <cstring>
#include <string>
#include <vector>

// 실행 파일 이름에서 실험 변수를 읽는다 (cfg보다 우선)
// 규칙: 이름을 '_'로 나눈 토큰 중 "<키 문자><값>" 형태를 찾음, 순서 무관
//   FloodClient_s8.exe                → s = 8
//   networkLibProject_acceptex.exe    → 단어 "acceptex"
// 목적: 프로파일 결과 파일 이름이 exe 이름을 따르므로 조건 라벨이 결과에 자동으로 붙음
namespace ExeName
{
    // 실행 파일 이름에서 경로와 확장자를 뺀 뒤 '_'로 나눈 토큰 목록
    inline std::vector<std::string> GetTokens()
    {
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);

        const char* exeName = strrchr(exePath, '\\');
        if (exeName != nullptr)
        {
            ++exeName;
        }
        else
        {
            exeName = exePath;
        }

        std::string name = exeName;
        size_t dot = name.rfind('.');
        if (dot != std::string::npos)
        {
            name.resize(dot);
        }

        std::vector<std::string> tokens;
        size_t begin = 0;
        while (begin <= name.size())
        {
            size_t end = name.find('_', begin);
            if (end == std::string::npos)
            {
                end = name.size();
            }
            tokens.push_back(name.substr(begin, end - begin));
            begin = end + 1;
        }
        return tokens;
    }

    // 문자열 전체를 0 이상의 정수로 변환 (숫자 외 문자가 섞이면 false)
    inline bool ParseInt(const char* first, const char* last, int& out)
    {
        if (first == last)
        {
            return false;
        }
        auto result = std::from_chars(first, last, out);
        return result.ec == std::errc() && result.ptr == last && out >= 0;
    }

    // 토큰 중 단어와 정확히 같은 것이 있는지 (예: "acceptex")
    inline bool HasWord(const char* word)
    {
        for (const std::string& token : GetTokens())
        {
            if (token == word)
            {
                return true;
            }
        }
        return false;
    }

    // "<key><숫자>" 토큰을 찾아 out에 저장, 없으면 false (out 그대로)
    inline bool GetInt(char key, int& out)
    {
        for (const std::string& token : GetTokens())
        {
            if (token.size() < 2 || token[0] != key)
            {
                continue;
            }

            int value;
            if (ParseInt(token.data() + 1, token.data() + token.size(), value))
            {
                out = value;
                return true;
            }
        }
        return false;
    }
}
