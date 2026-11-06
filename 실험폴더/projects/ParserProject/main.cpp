// ParserProject.cpp : 이 파일에는 'main' 함수가 포함됩니다. 거기서 프로그램 실행이 시작되고 종료됩니다.
//

#include "Parser.h"
#include <Windows.h>
#include <fstream>
#include <iostream>
int main()
{
    parser::stDebug debug;
    auto parser = parser::Parser::CreateParser("Config.txt", debug);
    if (parser == nullptr)
    {
        debug.CreateDebugFile();
    }
    else
    {
        int workerNumber;
        std::string DBIP;

        parser->Get("WorkerThread", workerNumber);
        parser->Get("DB_IP", DBIP);
        std::cout << workerNumber << "\n"
                  << DBIP;
    }
}