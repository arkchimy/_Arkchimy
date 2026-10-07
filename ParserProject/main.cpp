// ParserProject.cpp : 이 파일에는 'main' 함수가 포함됩니다. 거기서 프로그램 실행이 시작되고 종료됩니다.
//

#include <iostream>
#include "Parser.h"

int main()
{
    Parser parser("Config.txt");
    int number;
    float floatNumber;
    std::string DBIP;

    parser.Get("WorkerThread", number);
    parser.Get("DB_IP", DBIP);
    parser.Get("==", floatNumber);
    parser.Get("32", floatNumber);

    std::cout << number << "\n" << DBIP;

}