#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace config
{

// 설정 파일과 커맨드라인 인수를 읽어 key=value 쌍으로 보관한다.
// 우선순위: 커맨드라인 > 설정 파일
class ConfigFile
{
public:
    // argv에서 --config=경로 를 찾고(없으면 defaultPath) 파일을 읽은 뒤,
    // 나머지 --key=value 로 덮어쓴다. 이 순서를 라이브러리가 강제한다.
    ConfigFile(int argc, char* argv[], const std::filesystem::path& defaultPath);

    // 키가 없으면 예외를 던진다. 필수 항목용.
    template <typename T>
    T get(std::string_view key) const;

    // 키가 없으면 defaultValue를 돌려준다. 선택 항목용.
    template <typename T>
    T getOr(std::string_view key, const T& defaultValue) const;

    bool has(std::string_view key) const;

private:
    void loadFile(const std::filesystem::path& path);
    void overrideFromArgs(int argc, char* argv[]);
    void setValue(std::string_view key, std::string_view value);

    std::unordered_map<std::string, std::string> mValues;
};

}
