#include "Parser.h"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <cassert>

static void MyAssert(bool isTrue, const char* str)
{
	if (isTrue == false)
	{
		std::cout << str << "\n";
		assert(false);
	}
}
Parser::Parser(const char* filename)
{
	std::ifstream file{ filename };
	MyAssert(file.is_open(), "File Not Found");

	std::string key, value;
	key.reserve(64);
	value.reserve(64);

	char equle[2];
	while (!file.eof())
	{
		file >> key;
		if (file.fail())
		{
			break;
		}
		if (key[0] == '\/' && key[1] == '\/')
		{
			file.ignore(65535, '\n');
			continue;
		}
		file >> std::setw(sizeof(equle)) >> equle;
		MyAssert(equle[0] == '=', "is not equle");
		file >> value;
		MyAssert(!file.fail(), "has not value");
		MyAssert(value[value.size() - 1] == ';', " has not semicolon ");
		value[value.size() - 1] = '\0';

		mValues[key] = value;
	}
}
