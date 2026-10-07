// ParserProject.cpp : 이 파일에는 'main' 함수가 포함됩니다. 거기서 프로그램 실행이 시작되고 종료됩니다.
//

#include <iostream>
#include "Parser.h"

int main()
{
    parser::Parser parser("Config.txt");
    int number;
    float floatNumber;
    std::string DBIP;

    if (parser.Get("KeyAppendEqule", number) == false)
    {
        __debugbreak();
    }
    if (parser.Get("Invalid_Value", number) == false)
    {
        __debugbreak();
    }
    if (parser.Get("Invalid_Value2", number) == false)
    {
        __debugbreak();
    }
    if (parser.Get("WorkerThread", number) == false)
    {
        __debugbreak();
    }
    if (parser.Get("DB_IP", DBIP) == false)
    {
        __debugbreak();
    }

    std::cout << number << "\n" << DBIP;

}