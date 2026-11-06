#include "Parser.h"
#include <cassert>
#include <fstream>
#include <sstream>

#include <iomanip>
#include <iostream>

using namespace parser;

Parser::Parser(const char *filename, stDebug& debug)
{
    debug.filename = filename;
    loadFile(filename,debug);
}

void parser::Parser::loadFile(const char *filename, stDebug &debug)
{
    std::ifstream file(filename);
    if (!file.is_open())
    {
        debug.err = NotFoundFile;
        return;
    }

    std::string lineBuffer;
    while (std::getline(file, lineBuffer))
    {
        std::istringstream ls{lineBuffer};
        std::string key, equal, value;
        ls >> key;
        if (ls.fail())
        {
            continue;
        }
        else if (key[0] == '/')
        {
            continue;
        }
        else if (mValues.find(key) != mValues.end())
        {
            debug.lastKey = key;
            debug.lastValue = value;
            debug.err = Exist_Key;
            return;
        }

        ls >> equal;
        if (ls.fail() || equal != "=")
        {
            debug.lastKey = key;
            debug.err = Invalid_Equal;
            return;
        }
        ls >> value;
        if (ls.fail() || value.back() != ';')
        {
            debug.lastKey = key;
            debug.err = Invalid_Value;
            return;
        }
        value.pop_back();

        mValues[key] = value;
        ls >> value;
        if (ls.fail() || value[0] == '/')
        {
            continue;
        }
        debug.err = Value_After_Data;
        return;
    }

    
    
}

std::unique_ptr<Parser> parser::Parser::CreateParser(const char *filename, stDebug &debug)
{
    Parser* obj = new Parser(filename, debug);
    std::unique_ptr<Parser> parser(obj);
    if (debug.err != Success)
    {
        return nullptr;
    }
    return std::move(parser);
}

void parser::stDebug::CreateDebugFile()
{
    std::string debugFileName = "ParserError";
    debugFileName += +filename;
   
    std::fstream debugFile{debugFileName};
Retry:
    if (debugFile.is_open())
    {
        switch (err)
        {
        case NotFoundFile:
            debugFile << "FileName : " << filename << "NotFoundFile \n";
            break;
        case HasNotSemicolon:
            debugFile << "value : " << lastValue << "HasNotSemicolon \n";
            break;
        case HasNotEqual:
            debugFile << "Key : " << lastKey << " hasNotEqual\n";
            break;
        case Invalid_Key:
            debugFile << "Key : " << lastKey << " Invalid_Key\n";
            break;
        case Invalid_Equal:
            debugFile << "equal : Invalid_Equal \n";
            break;
        case Invalid_Value:
            debugFile << "Value :" << lastValue << "Invalid_Value \n";
            break;
        case Exist_Key:
            debugFile << "Exist_Key :" << lastKey << "Exist_Key \n";
            break;
        }
        return;
    }
    else
    {
        debugFileName += rand();
        std::fstream debugFile{debugFileName};
        goto Retry;
    }

}
