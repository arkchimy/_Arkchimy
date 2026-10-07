#include "ConfigFile.h"

#include <optional>
#include <stdexcept>

namespace config
{

namespace
{
    // 한 줄에서 잘라낸 키와 값. 원본 줄을 가리키는 뷰이므로 원본보다 오래 살 수 없다.
    struct KeyValue
    {
        std::string_view Key;
        std::string_view Value;
    };

    // 문자열 양쪽의 공백/탭/개행을 잘라낸 뷰를 돌려준다. 복사하지 않는다.
    std::string_view trim(std::string_view str)
    {
        constexpr std::string_view WHITESPACE = " \t\r\n";

        const size_t begin = str.find_first_not_of(WHITESPACE);
        if (begin == std::string_view::npos)
        {
            // 전부 공백인 경우
            return std::string_view{};
        }

        const size_t end = str.find_last_not_of(WHITESPACE);
        return str.substr(begin, end - begin + 1);
    }

    // 한 줄을 키와 값으로 쪼갠다.
    // 빈 줄과 주석 줄은 nullopt(건너뛸 줄)를 돌려주고, 형식이 잘못된 줄은 예외를 던진다.
    std::optional<KeyValue> parseLine(std::string_view line, int lineNo)
    {
        const std::string_view trimmed = trim(line);

        if (trimmed.empty() || trimmed.front() == '#')
        {
            return std::nullopt;
        }

        const size_t equalPos = trimmed.find('=');
        if (equalPos == std::string_view::npos)
        {
            throw std::runtime_error(
                "설정 " + std::to_string(lineNo) + "행: '=' 가 없습니다 -> " + std::string(trimmed));
        }

        const std::string_view key = trim(trimmed.substr(0, equalPos));
        if (key.empty())
        {
            throw std::runtime_error(
                "설정 " + std::to_string(lineNo) + "행: 키가 비어 있습니다 -> " + std::string(trimmed));
        }

        // 값 뒤에 붙은 주석을 잘라낸다.   port = 32000   # 서버 포트
        std::string_view value = trimmed.substr(equalPos + 1);
        const size_t commentPos = value.find('#');
        if (commentPos != std::string_view::npos)
        {
            value = value.substr(0, commentPos);
        }

        return KeyValue{ key, trim(value) };
    }
}

}
