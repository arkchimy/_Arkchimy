#include "Parser.h"
#include <cassert>
#include <fstream>
#include <iomanip>
#include <iostream>
using namespace parser;

Parser::Parser(const char *filename)
    : mErr{filename}
{
    std::ifstream file{filename};
    if (file.is_open() == false)
    {
        mErr.err = NotFoundFile;
        return;
    }
    file >> *this;
}

const Parser &parser::operator>>(std::ifstream &file, Parser &parser)
{
    std::string key, value;
    key.reserve(64);
    value.reserve(64);

    char equle[2];
    while (!file.eof())
    {
        if (parser.successChk() == false)
        {
            return parser;
        }
        file >> key;
        if (file.fail())
        {
            continue;
        }
        if (key[0] == '/')
        {
            file.ignore(65535, '\n');
            continue;
        }
        file >> std::setw(2) >> equle;
        if (equle[0] != '=')
        {
            parser.mErr.err = HasNotEqule;
            parser.mErr.lastKey = key;
            parser.mErr.lastValue = "";
            continue;
        }
        file >> value;
        
        if (file.fail())
        {
            parser.mErr.err = Invalid_Value;
            parser.mErr.lastKey = key;
            parser.mErr.lastValue = "";
            continue;
        }
        else if (value.back() != ';')
        {
            parser.mErr.err = HasNotSemicolon;
            parser.mErr.lastKey = key;
            parser.mErr.lastValue = value;
            continue;
        }
        else if (value.front() == '/')
        {
            parser.mErr.err = Invalid_Value;
            parser.mErr.lastKey = key;
            parser.mErr.lastValue = value;
            value = "";
            continue;
        }
        value.pop_back();
        parser.mValues[key] = value;
    }
    return parser;
}
