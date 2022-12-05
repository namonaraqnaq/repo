// http_server.cpp : This file contains the 'main' function. Program execution begins and ends there.
//
#include <boost/regex.hpp> 
#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <map>

using namespace std;

static int input_params_error() {
	cerr <<
		"Usage: --port=portnumber --reload=seconds --response=path_to_response.json" <<
		"Example:\n" <<
		"    http_server --port=80 --reload=30 --response=response.json";
	return EXIT_FAILURE;
}

int main(int argc, char *argv[])
{
	if (argc != 4) {
		return input_params_error();
	}
	map<string, string> settings;
	boost::regex args_format("^--(.+)=(.+)$");
	boost::cmatch cm;
	for (int i = 1; i < argc; ++i) {
		if (!boost::regex_match(argv[i], cm, args_format)) {
			return input_params_error();
		}
		settings[cm[1].str()] = cm[2].str();
	}

	const auto port(settings["port"]);
	if (port.empty()) {
		return input_params_error();
	}
	const auto reload_secs = atoi(settings["reload"].c_str());
	if (reload_secs <= 0) {
		return input_params_error();
	}

	const auto response_path(settings["response"]);
	ifstream f(response_path.c_str());
	if (!f.good()) {
		cerr << "bad path_to_response" << endl;
		return input_params_error();
	}

	return EXIT_SUCCESS;
}
